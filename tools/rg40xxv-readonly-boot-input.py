#!/usr/bin/env python3
"""Read-only RG40XX V stock-Linux input mapper.

This tool opens an evdev character device O_RDONLY|O_NONBLOCK.  It does not
grab the device, request GPIO lines, change pinmux, write sysfs, or reboot.
Its purpose is to confirm the *physical* buttons and the stock Linux event
mapping before implementing an independent U-Boot input path.
"""

from __future__ import annotations

import argparse
import glob
import os
import select
import stat
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path


EV_SYN = 0
EV_KEY = 1
EV_ABS = 3
ABS_HAT0X = 16
ABS_HAT0Y = 17

# Native Linux input_event.  On the stock aarch64 userspace this is 24 bytes:
# struct timeval (two longs), u16 type, u16 code, s32 value.
INPUT_EVENT = struct.Struct("@llHHi")

CODE_NAMES = {
    (EV_ABS, ABS_HAT0X): "ABS_HAT0X",
    (EV_ABS, ABS_HAT0Y): "ABS_HAT0Y",
    (EV_KEY, 114): "KEY_VOLUMEDOWN",
    (EV_KEY, 115): "KEY_VOLUMEUP",
    (EV_KEY, 304): "BTN_SOUTH (stock physical A)",
    (EV_KEY, 305): "BTN_EAST (stock physical B)",
    (EV_KEY, 306): "BTN_C",
    (EV_KEY, 307): "BTN_NORTH",
    (EV_KEY, 308): "BTN_WEST",
    (EV_KEY, 309): "BTN_Z",
    (EV_KEY, 310): "BTN_TL",
    (EV_KEY, 311): "BTN_TR",
    (EV_KEY, 312): "BTN_TL2 (stock Menu)",
    (EV_KEY, 313): "BTN_TR2",
    (EV_KEY, 314): "BTN_SELECT",
    (EV_KEY, 315): "BTN_START",
    (EV_KEY, 316): "BTN_MODE",
    (EV_KEY, 354): "KEY_GOTO (stock Menu companion)",
}

# Buttons needed by the proposed boot selector.  Each step requires a fresh
# press and release from the named physical control.
GUIDED_STEPS = (
    ("方向鍵 上", EV_ABS, ABS_HAT0Y, -1, 0),
    ("方向鍵 下", EV_ABS, ABS_HAT0Y, +1, 0),
    ("方向鍵 左", EV_ABS, ABS_HAT0X, -1, 0),
    ("方向鍵 右", EV_ABS, ABS_HAT0X, +1, 0),
    ("實體 A", EV_KEY, 304, 1, 0),
    ("實體 B", EV_KEY, 305, 1, 0),
    ("Menu", EV_KEY, 312, 1, 0),
)

# Controls whose non-neutral activity is relevant to a fail-closed U-Boot
# implementation.  Analog stick axes are intentionally not in this set.
DIRECT_CONTROL_CODES = {
    (EV_ABS, ABS_HAT0X),
    (EV_ABS, ABS_HAT0Y),
    *((EV_KEY, code) for code in range(114, 116)),
    *((EV_KEY, code) for code in range(304, 317)),
    (EV_KEY, 354),
}


@dataclass(frozen=True)
class Event:
    sec: int
    usec: int
    kind: int
    code: int
    value: int

    def text(self) -> str:
        name = CODE_NAMES.get((self.kind, self.code), "unknown")
        return (
            f"{self.sec}.{self.usec:06d} type={self.kind} "
            f"code={self.code} {name} value={self.value}"
        )


def input_devices() -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for sys_event in sorted(glob.glob("/sys/class/input/event*")):
        name_path = Path(sys_event) / "device" / "name"
        try:
            name = name_path.read_text(encoding="utf-8", errors="replace").strip()
        except OSError:
            name = "<unreadable>"
        found.append((f"/dev/input/{Path(sys_event).name}", name))
    return found


def choose_device(explicit: str | None) -> str:
    if explicit:
        return explicit
    matches = [path for path, name in input_devices() if name == "ANBERNIC-keys"]
    if len(matches) != 1:
        detail = ", ".join(matches) if matches else "none"
        raise RuntimeError(
            "expected exactly one input named ANBERNIC-keys; "
            f"found {detail}. Use --list and --device explicitly."
        )
    return matches[0]


def open_read_only(path: str) -> int:
    info = os.stat(path, follow_symlinks=False)
    if not stat.S_ISCHR(info.st_mode):
        raise RuntimeError(f"refusing non-character device: {path}")
    flags = os.O_RDONLY | os.O_NONBLOCK
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    return os.open(path, flags)


def unpack_events(buffer: bytes) -> tuple[list[Event], bytes]:
    complete = len(buffer) - (len(buffer) % INPUT_EVENT.size)
    events = [Event(*values) for values in INPUT_EVENT.iter_unpack(buffer[:complete])]
    return events, buffer[complete:]


def drain(fd: int) -> None:
    while True:
        try:
            if not os.read(fd, INPUT_EVENT.size * 64):
                return
        except BlockingIOError:
            return


def read_available(fd: int, pending: bytes) -> tuple[list[Event], bytes]:
    chunks = [pending]
    while True:
        try:
            chunk = os.read(fd, INPUT_EVENT.size * 64)
        except BlockingIOError:
            break
        if not chunk:
            break
        chunks.append(chunk)
    return unpack_events(b"".join(chunks))


def is_active(event: Event) -> bool:
    if event.kind == EV_KEY:
        return event.value != 0
    if event.kind in (EV_ABS,):
        return event.value != 0
    return False


def guided_step(
    fd: int,
    label: str,
    kind: int,
    code: int,
    press_value: int,
    release_value: int,
    timeout_seconds: float,
) -> bool:
    drain(fd)
    print(f"\n[{label}] 請現在只按一下並放開；不要同時按別顆鍵。", flush=True)
    deadline = time.monotonic() + timeout_seconds
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
    pending = b""
    seen_press = False
    seen_release = False
    unexpected: list[Event] = []
    last_event = time.monotonic()

    while time.monotonic() < deadline:
        remaining_ms = max(1, min(200, int((deadline - time.monotonic()) * 1000)))
        ready = poller.poll(remaining_ms)
        if not ready:
            if seen_release and time.monotonic() - last_event >= 0.25:
                break
            continue
        if any(mask & (select.POLLERR | select.POLLHUP) for _, mask in ready):
            print("  FAIL: evdev poll error/hangup")
            return False
        events, pending = read_available(fd, pending)
        for event in events:
            if event.kind == EV_SYN:
                continue
            print(" ", event.text())
            last_event = time.monotonic()
            identity = (event.kind, event.code)
            if identity == (kind, code):
                if not seen_press and event.value == press_value:
                    seen_press = True
                elif seen_press and event.value == release_value:
                    seen_release = True
                elif event.value not in (press_value, release_value):
                    unexpected.append(event)
            elif label == "Menu" and identity == (EV_KEY, 354):
                # Stock software may emit KEY_GOTO together with BTN_TL2.
                continue
            elif identity in DIRECT_CONTROL_CODES and is_active(event):
                unexpected.append(event)

    ok = seen_press and seen_release and not unexpected
    if ok:
        print("  PASS: fresh press/release mapping matched")
    else:
        print(
            "  FAIL: "
            f"press={seen_press} release={seen_release} "
            f"unexpected_active={len(unexpected)}"
        )
    return ok


def guided(fd: int, timeout_seconds: float) -> int:
    print(
        "mode=read-only guided mapping\n"
        "注意：這只證明 stock Linux evdev；不證明 U-Boot 已能讀按鍵。\n"
        "不要按 Reset，也不要測 PH9 recovery 線。",
        flush=True,
    )
    results = [guided_step(fd, *step, timeout_seconds) for step in GUIDED_STEPS]
    passed = sum(results)
    print(f"\nsummary={passed}/{len(results)} passed")
    return 0 if all(results) else 1


def raw_watch(fd: int, seconds: float) -> int:
    drain(fd)
    deadline = time.monotonic() + seconds
    poller = select.poll()
    poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
    pending = b""
    print(f"mode=read-only raw watch seconds={seconds:g}", flush=True)
    while time.monotonic() < deadline:
        remaining_ms = max(1, min(500, int((deadline - time.monotonic()) * 1000)))
        ready = poller.poll(remaining_ms)
        if any(mask & (select.POLLERR | select.POLLHUP) for _, mask in ready):
            print("evdev poll error/hangup", file=sys.stderr)
            return 2
        if ready:
            events, pending = read_available(fd, pending)
            for event in events:
                if event.kind != EV_SYN:
                    print(event.text(), flush=True)
    return 0


def print_ph9_debugfs() -> None:
    print(
        "PH9 passive debugfs inventory only; this does not prove wiring, pull, "
        "polarity, or a safe recovery action. Do not press Reset."
    )
    paths = sorted(
        glob.glob("/sys/kernel/debug/pinctrl/*/pinmux-pins")
        + glob.glob("/sys/kernel/debug/pinctrl/*/pinconf-pins")
    )
    matched = False
    for path in paths:
        try:
            lines = Path(path).read_text(errors="replace").splitlines()
        except OSError as error:
            print(f"{path}: unreadable: {error}")
            continue
        selected = [line for line in lines if "PH9" in line or "pin 233 " in line]
        for line in selected:
            matched = True
            print(f"{path}: {line.strip()}")
    if not paths:
        print("debugfs pinctrl files unavailable")
    elif not matched:
        print("PH9/global pin 233 not named in available debugfs files")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="list input nodes and names")
    parser.add_argument("--device", help="explicit /dev/input/eventN")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--guided", action="store_true", help="guided D-pad/A/B/Menu test")
    mode.add_argument(
        "--raw-seconds", type=float, metavar="N", help="print raw events for N seconds"
    )
    parser.add_argument(
        "--step-timeout", type=float, default=10.0, help="guided timeout per button"
    )
    parser.add_argument(
        "--ph9-info", action="store_true", help="passively read PH9 pinctrl debugfs text"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.list:
        for path, name in input_devices():
            print(f"{path}\t{name}")
    if args.ph9_info:
        print_ph9_debugfs()
    if not args.guided and args.raw_seconds is None:
        if not args.list and not args.ph9_info:
            print("No active test requested. Use --list, --guided, or --raw-seconds N.")
        return 0
    if args.step_timeout <= 0 or (args.raw_seconds is not None and args.raw_seconds <= 0):
        raise RuntimeError("timeouts must be positive")

    device = choose_device(args.device)
    print(f"device={device}")
    fd = open_read_only(device)
    try:
        if args.guided:
            return guided(fd, args.step_timeout)
        return raw_watch(fd, args.raw_seconds)
    finally:
        os.close(fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
