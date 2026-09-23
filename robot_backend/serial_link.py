"""Serial transport for talking to the ESP32-S3 KibiKibi.

This module is the Python half of the wire protocol documented in the
WIRE PROTOCOL comment block at the top of ../KibiKibi.ino. The two files
must always agree exactly - if you change one, change the other and re-run
tests/test_offline_cycle.py.

Protocol recap (ASCII lines, '\\n'-terminated; ESP32 sends '\\r\\n' via
println() but both sides strip trailing '\\r\\n'):

  ESP32 -> laptop:
    STATUS:<text>
    TRIGGER:BUTTON | TRIGGER:PROXIMITY
    ERROR:<text>

  laptop -> ESP32:
    COMMAND:GREET | COMMAND:LISTEN | COMMAND:PROCESSING |
    COMMAND:SPEAK (laptop plays TTS on its own speaker, then sends
                   COMMAND:IDLE when done) | COMMAND:IDLE
"""
from __future__ import annotations

import logging
import time
from typing import Optional, Protocol

logger = logging.getLogger("robot_backend.serial_link")

# Must match KibiKibi.ino's SERIAL_BAUD.
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
