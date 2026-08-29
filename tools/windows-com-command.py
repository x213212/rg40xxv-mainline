#!/usr/bin/env python3
"""Send one command over a Windows COM port while keeping one handle open."""

import ctypes
from ctypes import wintypes
import sys
import time


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


def fail(message: str) -> None:
    raise SystemExit(f"{message}: WinError {ctypes.get_last_error()}")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} COMx command")

    port = rf"\\.\{sys.argv[1]}"
    payload = (sys.argv[2] + "\r\n").encode("ascii")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(COMMTIMEOUTS)]
    kernel32.SetCommTimeouts.restype = wintypes.BOOL
    kernel32.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.EscapeCommFunction.restype = wintypes.BOOL
    kernel32.WriteFile.argtypes = [
        wintypes.HANDLE,
        wintypes.LPCVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,
    ]
    kernel32.WriteFile.restype = wintypes.BOOL
    kernel32.ReadFile.argtypes = [
        wintypes.HANDLE,
        wintypes.LPVOID,
        wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD),
        wintypes.LPVOID,
    ]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    handle = kernel32.CreateFileW(
        port,
        0x80000000 | 0x40000000,
        0,
        None,
        3,
        0x80,
        None,
    )
    if handle == wintypes.HANDLE(-1).value:
        fail("CreateFileW failed")

    try:
        timeouts = COMMTIMEOUTS(50, 0, 200, 0, 1000)
        if not kernel32.SetCommTimeouts(handle, ctypes.byref(timeouts)):
            fail("SetCommTimeouts failed")
        kernel32.EscapeCommFunction(handle, 5)  # SETDTR
        kernel32.EscapeCommFunction(handle, 3)  # SETRTS
        time.sleep(0.5)

        written = wintypes.DWORD()
        buffer = ctypes.create_string_buffer(payload)
        if not kernel32.WriteFile(handle, buffer, len(payload), ctypes.byref(written), None):
            fail("WriteFile failed")
        if written.value != len(payload):
            raise SystemExit(f"short write: {written.value}/{len(payload)}")

        deadline = time.monotonic() + 4.0
        idle_deadline = time.monotonic() + 1.0
        output = bytearray()
        while time.monotonic() < deadline and time.monotonic() < idle_deadline:
            chunk = ctypes.create_string_buffer(4096)
            got = wintypes.DWORD()
            if not kernel32.ReadFile(handle, chunk, len(chunk), ctypes.byref(got), None):
                break
            if got.value:
                output.extend(chunk.raw[: got.value])
                idle_deadline = time.monotonic() + 1.0
        if output:
            sys.stdout.buffer.write(output)
    finally:
        kernel32.CloseHandle(handle)


if __name__ == "__main__":
    main()
