"""An in-memory, thread-safe duplex byte stream pair standing in for a real
serial port, so serial_link.SerialLink can be exercised without any COM
port or hardware - see test_offline_cycle.py.
"""
from __future__ import annotations

import queue
import time
from typing import Optional


class _HalfDuplex:
    """One direction of a duplex pipe: bytes written on the 'in' side show
    up (as arbitrarily-chunked reads) on the 'out' side."""

    def __init__(self):
        self._buf = bytearray()
        self._q: "queue.Queue[bytes]" = queue.Queue()
        self.timeout: Optional[float] = 1.0

    def write(self, data: bytes) -> int:
        self._q.put(bytes(data))
        return len(data)

    def read(self, size: int = 1) -> bytes:
        deadline = time.monotonic() + (self.timeout if self.timeout is not None else 3600)
        while len(self._buf) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            try:
                chunk = self._q.get(timeout=remaining)
                self._buf.extend(chunk)
            except queue.Empty:
                break
        n = min(size, len(self._buf))
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def flush(self) -> None:
        pass

    def close(self) -> None:
        pass


class FakeSerialPair:
    """Two ByteStream-compatible endpoints, `device` and `host`, wired so
    that writes on one side are reads on the other - like a null-modem
    cable between the (simulated) ESP32 and the backend."""

    def __init__(self):
        device_to_host = _HalfDuplex()
        host_to_device = _HalfDuplex()

        self.device = _Endpoint(read_from=device_to_host, write_to=host_to_device)
        self.host = _Endpoint(read_from=host_to_device, write_to=device_to_host)


class _Endpoint:
    def __init__(self, read_from: _HalfDuplex, write_to: _HalfDuplex):
        self._read_from = read_from
        self._write_to = write_to

    @property
    def timeout(self) -> Optional[float]:
        return self._read_from.timeout

    @timeout.setter
    def timeout(self, value: Optional[float]) -> None:
        self._read_from.timeout = value

    def read(self, size: int = 1) -> bytes:
        return self._read_from.read(size)

    def write(self, data: bytes) -> int:
        return self._write_to.write(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        pass
