#!/usr/bin/env python3
"""Verify an RG40XX V stock-U-Boot Android-v2 persistent boot image.

This verifier reads regular files only.  It never opens a block device.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


ANDROID_MAGIC = b"ANDROID!"
FDT_MAGIC = 0xD00DFEED
PAGE_SIZE = 0x800
HEADER_SIZE_V2 = 1660
PARTITION_SIZE = 64 * 1024 * 1024
KERNEL_LOAD = 0x40200000
DTB_LOAD = 0x44000000
FRAMEBUFFER_GUARD = 0x46400000


class VerifyError(RuntimeError):
    pass


def require(value: bool, message: str) -> None:
    if not value:
        raise VerifyError(message)


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def read_regular(path: Path) -> bytes:
    require(path.is_file(), f"not a regular file: {path}")
    return path.read_bytes()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify(args: argparse.Namespace) -> None:
    boot = read_regular(args.boot_img.resolve())
    image = read_regular(args.image.resolve())
    dtb = read_regular(args.dtb.resolve())

    require(HEADER_SIZE_V2 <= len(boot) <= PARTITION_SIZE,
            f"boot image size {len(boot)} is outside the 64 MiB p8 slot")
    require(boot[:8] == ANDROID_MAGIC, "missing ANDROID! magic")
    fields = struct.unpack_from("<10I", boot, 8)
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page_size, header_version, os_version) = fields
    require(page_size == PAGE_SIZE, f"page size is {page_size}, expected 2048")
    require(header_version == 2, f"header version is {header_version}, expected 2")
    require(kernel_addr == KERNEL_LOAD,
            f"kernel address is 0x{kernel_addr:x}, expected 0x{KERNEL_LOAD:x}")
    require(ramdisk_size == 0, "external ramdisk must be empty (initramfs is built in)")
    require(second_size == 0, "second-stage payload must be empty")
    require(ramdisk_addr == 0x42000000, "unexpected ramdisk address")
    require(second_addr == 0x40F00000, "unexpected second-stage address")
    require(tags_addr == 0x40000100, "unexpected tags address")
    require(os_version == 0, "unexpected Android os_version")
    require(boot[0x30:0x40].split(b"\0", 1)[0] == b"sun50i_arm64",
            "unexpected board name")
    cmdline = boot[0x40:0x240] + boot[0x260:0x660]
    require(not cmdline.rstrip(b"\0"), "outer Android cmdline must be empty")

    recovery_dtbo_size = struct.unpack_from("<I", boot, 0x660)[0]
    recovery_dtbo_offset = struct.unpack_from("<Q", boot, 0x664)[0]
    header_size = struct.unpack_from("<I", boot, 0x66C)[0]
    dtb_size = struct.unpack_from("<I", boot, 0x670)[0]
    dtb_addr = struct.unpack_from("<Q", boot, 0x674)[0]
    require(recovery_dtbo_size == 0 and recovery_dtbo_offset == 0,
            "recovery-DTBO field must be empty")
    require(header_size == HEADER_SIZE_V2,
            f"header size is {header_size}, expected {HEADER_SIZE_V2}")
    require(dtb_addr == DTB_LOAD,
            f"DTB address is 0x{dtb_addr:x}, expected 0x{DTB_LOAD:x}")

    kernel_offset = PAGE_SIZE
    dtb_offset = align_up(kernel_offset + kernel_size, PAGE_SIZE)
    declared_end = dtb_offset + dtb_size
    require(declared_end <= len(boot), "declared payload extends past EOF")
    require(boot[kernel_offset:kernel_offset + kernel_size] == image,
            "embedded kernel differs from supplied Image")
    require(boot[dtb_offset:declared_end] == dtb,
            "embedded DTB differs from supplied DTB")
    require(set(boot[kernel_offset + kernel_size:dtb_offset]) <= {0},
            "kernel-to-DTB alignment padding is not zero")
    require(set(boot[declared_end:]) <= {0},
            "nonzero trailing bytes follow the DTB")

    require(len(image) >= 64 and image[56:60] == b"ARM\x64",
            "kernel lacks the arm64 Image magic")
    text_offset, effective_size, _flags = struct.unpack_from("<QQQ", image, 8)
    require(text_offset == 0, "arm64 Image text_offset is not zero")
    require(effective_size >= len(image), "effective Image size is too small")
    require(KERNEL_LOAD % (2 * 1024 * 1024) == 0,
            "kernel load address is not 2 MiB aligned")
    require(KERNEL_LOAD + effective_size < FRAMEBUFFER_GUARD,
            "kernel effective range reaches the observed framebuffer guard")
    source_start = 0x45000000
    source_end = source_start + len(boot)
    require(KERNEL_LOAD + effective_size <= source_start or KERNEL_LOAD >= source_end,
            "kernel load range overlaps vendor U-Boot's boot-image buffer")
    require(len(dtb) >= 40 and struct.unpack_from(">I", dtb, 0)[0] == FDT_MAGIC,
            "DTB magic is invalid")
    require(struct.unpack_from(">I", dtb, 4)[0] == len(dtb),
            "DTB totalsize differs from file size")
    require(len(dtb) <= 2 * 1024 * 1024, "DTB is unexpectedly large")

    print("PASS: RG40XX V persistent Android-v2 image is internally consistent")
    print(f"boot.img  {len(boot):>10}  sha256 {sha256(boot)}")
    print(f"Image     {len(image):>10}  sha256 {sha256(image)}")
    print(f"DTB       {len(dtb):>10}  sha256 {sha256(dtb)}")
    print(f"kernel effective: 0x{KERNEL_LOAD:08x}-0x{KERNEL_LOAD + effective_size:08x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot_img", type=Path)
    parser.add_argument("image", type=Path)
    parser.add_argument("dtb", type=Path)
    args = parser.parse_args()
    try:
        verify(args)
    except VerifyError as error:
        print(f"FAIL: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
