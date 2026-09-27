"""Keep large offline transfers from retaining whole artifacts in Linux's page cache."""

from __future__ import annotations

import os

IO_CHUNK_BYTES = 8 * 1024 * 1024
WRITEBACK_BYTES = 64 * 1024 * 1024
# Windows-port 2026-09-26: page-cache APIs are POSIX-only; on Windows the
# fixed size is used for alignment arithmetic only and discarding is a no-op.
_POSIX = hasattr(os, "sysconf")
if _POSIX:
    _PAGE_BYTES = os.sysconf("SC_PAGE_SIZE")
else:
    _PAGE_BYTES = 4096


if _POSIX:
    def _pread(fd: int, count: int, offset: int) -> bytes:
        return os.pread(fd, count, offset)

    def _pwrite(fd: int, data, offset: int) -> int:
        return os.pwrite(fd, data, offset)

    def _fsync_fd(fd: int) -> None:
        os.fdatasync(fd)
else:
    # Windows-port 2026-09-26: on this host the CRT fd path (os.lseek +
    # os.read / os.write) returns short reads with wrong content at arbitrary
    # offsets in files > ~1 GB (repro: 100-byte reads at 1.0/2.0/3.64/4.5 GB
    # return 28/58/41/6 bytes, byte-identical across CPython 3.11.15 and
    # 3.12.10; kernel32 ReadFile/WriteFile on the same file and open()-based
    # reads are byte-exact at every offset). Route positioned I/O through the
    # Win32 handle behind the CRT fd instead. Verified 2026-09-26:
    # test_win32.py / test_osfh.py — reads and writes match byte-for-byte.
    import ctypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _ucrtbase = ctypes.CDLL("ucrtbase.dll")
    _ucrtbase._get_osfhandle.argtypes = [ctypes.c_int]
    _ucrtbase._get_osfhandle.restype = ctypes.c_void_p

    def _handle(fd: int):
        handle = _ucrtbase._get_osfhandle(fd)
        if not handle:
            raise OSError(f"fd {fd} has no Win32 handle (last error {ctypes.get_last_error()})")
        return handle

    def _pread(fd: int, count: int, offset: int) -> bytes:
        handle = _handle(fd)
        total = bytearray()
        pos = offset
        while len(total) < count:
            n = count - len(total)
            buf = (ctypes.c_char * n)()
            got = ctypes.c_uint32()
            if not _kernel32.SetFilePointerEx(handle, ctypes.c_longlong(pos), None, 0):
                raise OSError(f"SetFilePointerEx failed: {ctypes.get_last_error()}")
            if not _kernel32.ReadFile(handle, buf, n, ctypes.byref(got), None) and got.value == 0:
                raise OSError(f"ReadFile failed: {ctypes.get_last_error()}")
            if got.value == 0:
                break  # EOF before `count` bytes
            total.extend(buf.raw[:got.value])
            pos += got.value
        return bytes(total)

    def _pwrite(fd: int, data, offset: int) -> int:
        handle = _handle(fd)
        raw = bytes(data)
        written = 0
        pos = offset
        while written < len(raw):
            chunk = raw[written:]
            got = ctypes.c_uint32()
            if not _kernel32.SetFilePointerEx(handle, ctypes.c_longlong(pos), None, 0):
                raise OSError(f"SetFilePointerEx failed: {ctypes.get_last_error()}")
            if not _kernel32.WriteFile(handle, chunk, len(chunk), ctypes.byref(got), None) and got.value == 0:
                raise OSError(f"WriteFile failed: {ctypes.get_last_error()}")
            if got.value == 0:
                raise OSError("WriteFile wrote 0 bytes")
            written += got.value
            pos += got.value
        return written

    def _fsync_fd(fd: int) -> None:
        if not _kernel32.FlushFileBuffers(_handle(fd)):
            raise OSError(f"FlushFileBuffers failed: {ctypes.get_last_error()}")


def pread(fd: int, count: int, offset: int) -> bytes:
    return _pread(fd, count, offset)


def pwrite(fd: int, data, offset: int) -> int:
    return _pwrite(fd, data, offset)


def discard_cached_pages(fd: int, offset: int = 0, count: int | None = None) -> None:
    if not _POSIX:
        return
    if count is None:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    elif count > 0:
        begin = offset // _PAGE_BYTES * _PAGE_BYTES
        end = (offset + count + _PAGE_BYTES - 1) // _PAGE_BYTES * _PAGE_BYTES
        os.posix_fadvise(fd, begin, end - begin, os.POSIX_FADV_DONTNEED)


class Writeback:
    """Bound dirty output across all open shards; release clean pages after writeback."""

    def __init__(self) -> None:
        self._bytes = 0
        self._fds: set[int] = set()

    def written(self, fd: int, count: int) -> None:
        self._fds.add(fd)
        self._bytes += count
        if self._bytes >= WRITEBACK_BYTES:
            self.flush()

    def flush(self) -> None:
        for fd in self._fds:
            _fsync_fd(fd)
            discard_cached_pages(fd)
        self._fds.clear()
        self._bytes = 0
