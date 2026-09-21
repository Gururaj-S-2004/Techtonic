"""Serial transport for talking to the ESP32-S3 EventRobot.

This module is the Python half of the wire protocol documented in the
WIRE PROTOCOL comment block at the top of ../EventRobot.ino. The two files
must always agree exactly - if you change one, change the other and re-run
tests/test_offline_cycle.py.

Protocol recap (ASCII lines, '\\n'-terminated; ESP32 sends '\\r\\n' via
println() but both sides strip trailing '\\r\\n'):

  ESP32 -> laptop:
    STATUS:<text>
    TRIGGER:BUTTON | TRIGGER:PROXIMITY
    ERROR:<text>
    AUDIO_START:<len>:<crc32>  (+ <len> raw PCM16LE mono 16kHz bytes,
                                 + one blank-line separator, + "AUDIO_END")

  laptop -> ESP32:
    COMMAND:GREET | COMMAND:LISTEN | COMMAND:PROCESSING |
    COMMAND:SPEAK (+ one AUDIO_START/.../AUDIO_END frame) | COMMAND:IDLE
"""
from __future__ import annotations

import logging
import re
import time
import zlib
from typing import Optional, Protocol

logger = logging.getLogger("robot_backend.serial_link")

AUDIO_START_RE = re.compile(r"^AUDIO_START:(\d+):(\d+)$")

# Must match EventRobot.ino's SERIAL_BAUD.
DEFAULT_BAUD = 921600


class SerialLinkError(Exception):
    """Any protocol-level failure: malformed frame, CRC mismatch, etc."""


class SerialTimeout(SerialLinkError):
    """No data arrived before the deadline."""


class ByteStream(Protocol):
    """The subset of pyserial's Serial interface SerialLink depends on -
    lets tests substitute an in-memory fake instead of a real COM port."""

    timeout: Optional[float]

    def read(self, size: int = 1) -> bytes: ...
    def write(self, data: bytes) -> int: ...
    def flush(self) -> None: ...
    def close(self) -> None: ...


class SerialLink:
    def __init__(self, stream: ByteStream, default_timeout: float = 20.0):
        self._stream = stream
        self._default_timeout = default_timeout

    @classmethod
    def open(cls, port: str, baud: int = DEFAULT_BAUD, timeout: float = 20.0) -> "SerialLink":
        import serial  # local import: keep pyserial optional for pure-logic tests

        ser = serial.Serial(port, baud, timeout=timeout)
        # Let the ESP32 finish its boot/auto-reset-on-open before we talk.
        time.sleep(2.0)
        ser.reset_input_buffer()
        return cls(ser, default_timeout=timeout)

    def close(self):
        self._stream.close()

    # -- raw line I/O ---------------------------------------------------

    def readline(self, timeout: Optional[float] = None) -> str:
        """Reads one '\\n'-terminated line (CR/LF stripped). Raises
        SerialTimeout if nothing arrives within `timeout` seconds."""
        deadline_timeout = timeout if timeout is not None else self._default_timeout
        buf = bytearray()
        deadline = time.monotonic() + deadline_timeout
        original_timeout = self._stream.timeout
        self._stream.timeout = 0.2
        try:
            while True:
                if time.monotonic() > deadline:
                    raise SerialTimeout("no line received before timeout")
                chunk = self._stream.read(1)
                if not chunk:
                    continue
                if chunk == b"\n":
                    break
                buf.extend(chunk)
        finally:
            self._stream.timeout = original_timeout
        return buf.decode("utf-8", errors="replace").rstrip("\r")

    def send_line(self, text: str):
        self._stream.write((text + "\n").encode("utf-8"))
        self._stream.flush()

    def send_command(self, name: str):
        logger.debug("-> COMMAND:%s", name)
        self.send_line(f"COMMAND:{name}")

    # -- high-level protocol helpers -------------------------------------

    def wait_for_trigger(self, poll_timeout: float = 1.0) -> str:
        """Blocks (polling in poll_timeout-sized slices, forever) until a
        TRIGGER: line arrives. STATUS:/ERROR: lines seen while waiting are
        logged and ignored. Returns the trigger source, e.g. "BUTTON"."""
        while True:
            try:
                line = self.readline(timeout=poll_timeout)
            except SerialTimeout:
                continue
            if not line:
                continue
            if line.startswith("TRIGGER:"):
                return line[len("TRIGGER:"):]
            if line.startswith("STATUS:"):
                logger.info("[ESP32] %s", line[len("STATUS:"):])
            elif line.startswith("ERROR:"):
                logger.warning("[ESP32] %s", line[len("ERROR:"):])
            else:
                logger.debug("[ESP32] unrecognized line while idle: %r", line)

    def wait_for_status(self, expected: str, timeout: Optional[float] = None) -> None:
        """Blocks for a specific "STATUS:<expected>" line, raising
        SerialLinkError if an ERROR: line or a non-matching STATUS arrives
        first (mirrors the ESP32's own strict COMMAND: expectations)."""
        line = self.readline(timeout=timeout)
        if line == f"STATUS:{expected}":
            return
        if line.startswith("ERROR:"):
            raise SerialLinkError(f"ESP32 reported error: {line[len('ERROR:'):]}")
        raise SerialLinkError(f"expected STATUS:{expected}, got: {line!r}")

    # -- audio framing ----------------------------------------------------

    def read_audio_frame(self, timeout: Optional[float] = None) -> bytes:
        """Blocks for one AUDIO_START:<len>:<crc32> / bytes / AUDIO_END
        frame and returns the raw PCM16LE mono bytes."""
        header = self.readline(timeout=timeout)
        m = AUDIO_START_RE.match(header)
        if not m:
            raise SerialLinkError(f"expected AUDIO_START, got: {header!r}")
        length = int(m.group(1))
        expected_crc = int(m.group(2))

        payload = self._read_exact(length, timeout=timeout or self._default_timeout)

        # One blank-line separator, then the AUDIO_END line - see the
        # matching comment in EventRobot.ino's streamMicToBackend().
        self.readline(timeout=2.0)
        end_line = self.readline(timeout=2.0)
        if end_line != "AUDIO_END":
            raise SerialLinkError(f"expected AUDIO_END, got: {end_line!r}")

        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise SerialLinkError(
                f"audio CRC mismatch: expected {expected_crc}, got {actual_crc} "
                f"({len(payload)} bytes) - stream may be desynced"
            )
        return payload

    def send_audio_frame(self, pcm_bytes: bytes, baud: int = 921600):
        """Sends one AUDIO_START:<len>:<crc32> / bytes / AUDIO_END frame.

        Paces writes to stay within the ESP32's UART RX buffer capacity.
        921600 baud = ~92160 bytes/sec on the wire.

        IMPORTANT - Windows timer resolution:
        time.sleep() on Windows defaults to 15.6ms granularity. Any sleep
        shorter than ~16ms is effectively sleep(0), blasting bytes at OS
        buffer speed (~800KB/s) instead of wire speed (~92KB/s), which
        overflows the ESP32's UART RX buffer and causes:
          - GetOverlappedResult errors in the Windows serial driver
          - Dropped/corrupted bytes on the ESP32
          - Crackling at the end of audio playback

        Fix: use 4096-byte chunks so sleep_per_chunk ≈ 74ms, which is
        safely above the 15.6ms Windows timer floor. At 60% of line speed
        the ESP32 always has comfortable headroom to drain the UART while
        simultaneously feeding the I2S speaker pipeline.
        """
        crc = zlib.crc32(pcm_bytes) & 0xFFFFFFFF
        self._stream.write(f"AUDIO_START:{len(pcm_bytes)}:{crc}\n".encode("utf-8"))
        self._stream.flush()

        bytes_per_sec = baud / 10  # 8N1 = 10 bits per byte
        # 60% utilization → sleep_per_chunk for 4096-byte chunk ≈ 74ms.
        # This is reliable on Windows where time.sleep has 15.6ms resolution.
        target_bytes_per_sec = bytes_per_sec * 0.60
        chunk_size = 4096
        sleep_per_chunk = chunk_size / target_bytes_per_sec
        # Hard floor: never sleep less than 30ms regardless of baud setting.
        sleep_per_chunk = max(sleep_per_chunk, 0.030)

        logger.debug(
            "Sending %d bytes in %d-byte chunks, %.1fms inter-chunk sleep (%.0f%% line speed)",
            len(pcm_bytes), chunk_size, sleep_per_chunk * 1000,
            (chunk_size / sleep_per_chunk) / bytes_per_sec * 100,
        )

        for offset in range(0, len(pcm_bytes), chunk_size):
            chunk = pcm_bytes[offset : offset + chunk_size]
            self._stream.write(chunk)
            self._stream.flush()
            time.sleep(sleep_per_chunk)

        self._stream.write(b"\n")  # blank-line separator, mirrors EventRobot.ino
        self._stream.write(b"AUDIO_END\n")
        self._stream.flush()

    def _read_exact(self, length: int, timeout: float) -> bytes:
        buf = bytearray()
        deadline = time.monotonic() + timeout
        original_timeout = self._stream.timeout
        self._stream.timeout = 0.5
        try:
            while len(buf) < length:
                if time.monotonic() > deadline:
                    raise SerialTimeout(
                        f"timed out reading audio payload ({len(buf)}/{length} bytes)"
                    )
                chunk = self._stream.read(length - len(buf))
                if chunk:
                    buf.extend(chunk)
        finally:
            self._stream.timeout = original_timeout
        return bytes(buf)
