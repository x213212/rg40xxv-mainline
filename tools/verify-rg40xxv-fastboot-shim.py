#!/usr/bin/env python3
"""Static verifier for the RG40XX V vendor-Fastboot ARM64 shim image.

This program only reads regular files.  It does not open USB devices, block
devices, the live handheld, or an SD card.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path


ANDROID_MAGIC = b"ANDROID!"
ANDROID_PAGE = 0x800
FASTBOOT_BUFFER_BASE = 0x41000000
FASTBOOT_MAX = 32 * 1024 * 1024

UIMAGE_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM = 2
IH_TYPE_KERNEL = 2
IH_COMP_NONE = 0

VENDOR_FDT_START = 0x44000000
VENDOR_FDT_SLOT_END = 0x44023400

SHIM_LOAD = 0x441FF000
IMAGE_LOAD = 0x44200000
SHIM_SLOT = IMAGE_LOAD - SHIM_LOAD
DTB_LOAD = 0x46000000

# Current 76-byte shim.  It first makes active-high PI11 a solid-red handoff
# marker, then sets x0 = DTB_LOAD, x1..x3 = 0, x4 = IMAGE_LOAD and branches.
EXPECTED_SHIM = bytes.fromhex(
    "052696d2"  # mov x5, #0xb130
    "0560a0f2"  # movk x5, #0x300, lsl #16
    "a60040b9"  # ldr w6, [x5]
    "c6001532"  # orr w6, w6, #0x800
    "a60000b9"  # str w6, [x5]
    "a53000d1"  # sub x5, x5, #0xc
    "a60040b9"  # ldr w6, [x5]
    "c66c1012"  # and w6, w6, #0xffff0fff
    "c6001432"  # orr w6, w6, #0x1000
    "a60000b9"  # str w6, [x5]
    "9f3f03d5"  # dsb sy
    "00c0a8d2"  # mov x0, #0x46000000
    "e1031faa"  # mov x1, xzr
    "e2031faa"  # mov x2, xzr
    "e3031faa"  # mov x3, xzr
    "0484a8d2"  # mov x4, #0x44200000
    "9f3f03d5"  # dsb sy
    "df3f03d5"  # isb
    "80001fd6"  # br x4
)


class VerifyError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerifyError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def u32be(data: bytes) -> int:
    return struct.unpack(">I", data)[0]


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def no_overlap(a_start: int, a_end: int, b_start: int, b_end: int) -> bool:
    return a_end <= b_start or b_end <= a_start


def read_regular(path: Path) -> bytes:
    require(path.is_file(), f"not a regular file: {path}")
    return path.read_bytes()


def verify(args: argparse.Namespace) -> None:
    boot_path = args.boot_img.resolve()
    image_path = args.image.resolve()
    dtb_path = args.dtb.resolve()

    boot = read_regular(boot_path)
    image = read_regular(image_path)
    dtb = read_regular(dtb_path)

    require(len(boot) <= FASTBOOT_MAX,
            f"download is {len(boot)} bytes, exceeds vendor 32 MiB limit")
    require(len(boot) > ANDROID_PAGE,
            "vendor handler rejects downloads of 2048 bytes or less")
    require(len(boot) >= ANDROID_PAGE + 64,
            "download is too short for Android page plus uImage header")

    # Android v0 fields.  Vendor U-Boot ignores them except cmdline and always
    # skips a fixed 0x800 bytes; checking them still catches packaging drift.
    require(boot[:8] == ANDROID_MAGIC, "missing outer ANDROID! magic")
    fields = struct.unpack("<10I", boot[8:48])
    (kernel_size, kernel_addr, ramdisk_size, _ramdisk_addr, second_size,
     _second_addr, _tags_addr, page_size, header_version, _os_version) = fields
    require(page_size == ANDROID_PAGE,
            f"Android page size is {page_size}, expected 2048")
    require(header_version == 0,
            f"Android header version is {header_version}, expected v0")
    require(kernel_addr == SHIM_LOAD,
            f"Android kernel_addr is 0x{kernel_addr:x}, expected 0x{SHIM_LOAD:x}")
    require(ramdisk_size == 0 and second_size == 0,
            "RAM shim image must not carry ramdisk or second-stage payload")
    require(boot[64:64 + 512].rstrip(b"\0") == b"",
            "outer Android cmdline is nonempty and vendor U-Boot would copy it")
    require(kernel_size >= 64, "Android kernel_size cannot contain a uImage")

    uimage_end = ANDROID_PAGE + kernel_size
    require(uimage_end <= len(boot), "Android kernel_size runs past EOF")
    require(len(boot) == align_up(uimage_end, ANDROID_PAGE),
            "boot image length is not the exact page-aligned uImage envelope")
    require(set(boot[uimage_end:]) <= {0},
            "nonzero bytes follow the declared uImage payload")

    uimage = boot[ANDROID_PAGE:uimage_end]
    header = uimage[:64]
    data = uimage[64:]
    (magic, hcrc, _timestamp, data_size, load, entry, dcrc) = \
        struct.unpack(">7I", header[:28])
    os_id, arch, image_type, compression = struct.unpack("4B", header[28:32])

    require(magic == UIMAGE_MAGIC,
            f"legacy magic is 0x{magic:08x}, expected 0x{UIMAGE_MAGIC:08x}")
    header_zero_crc = bytearray(header)
    header_zero_crc[4:8] = b"\0\0\0\0"
    calc_hcrc = zlib.crc32(header_zero_crc) & 0xFFFFFFFF
    calc_dcrc = zlib.crc32(data) & 0xFFFFFFFF
    require(hcrc == calc_hcrc,
            f"uImage hCRC mismatch: header=0x{hcrc:08x}, calc=0x{calc_hcrc:08x}")
    require(dcrc == calc_dcrc,
            f"uImage dCRC mismatch: header=0x{dcrc:08x}, calc=0x{calc_dcrc:08x}")
    require(data_size == len(data),
            f"uImage data size is {data_size}, actual {len(data)}")
    require((os_id, arch, image_type, compression) ==
            (IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_KERNEL, IH_COMP_NONE),
            "uImage must be Linux/ARM/kernel/uncompressed; notably ARM, not ARM64")
    require(load == SHIM_LOAD and entry == SHIM_LOAD,
            f"uImage load/entry must both be 0x{SHIM_LOAD:x}")
    require(len(data) <= 32 * 1024 * 1024,
            "uImage data exceeds CONFIG_SYS_BOOTM_LEN=32 MiB")

    require(len(data) >= SHIM_SLOT + len(image) + len(dtb),
            "bundle is too short for shim slot, Image and DTB")
    require(data[:len(EXPECTED_SHIM)] == EXPECTED_SHIM,
            "bundle does not start with the audited 76-byte AArch64 shim")
    require(set(data[len(EXPECTED_SHIM):SHIM_SLOT]) <= {0},
            "remainder of the 4 KiB shim slot is not zero-filled")
    require(data[SHIM_SLOT:SHIM_SLOT + len(image)] == image,
            "raw Image bytes do not match the supplied Image")

    require(len(image) >= 64 and image[56:60] == b"ARM\x64",
            "supplied Image lacks the arm64 Image header magic")
    text_offset, effective_size, flags = struct.unpack("<QQQ", image[8:32])
    require(text_offset == 0,
            f"Image text_offset is 0x{text_offset:x}, expected zero")
    require(effective_size >= len(image),
            "Image effective size is smaller than its file length")
    require(IMAGE_LOAD % (2 * 1024 * 1024) == 0,
            "arm64 Image destination is not 2 MiB aligned")

    image_effective_end = IMAGE_LOAD + effective_size
    require(image_effective_end <= DTB_LOAD,
            "Image effective range reaches the shim's DTB address")
    # DTB_LOAD is the audited fixed address used by the 76-byte shim.  A
    # smaller rescue kernel may end more than one 2 MiB region below it; that
    # zero-filled safety gap is valid.  Reject growth into the fixed slot, but
    # do not require the kernel footprint to consume the entire slot.
    require(align_up(image_effective_end, 2 * 1024 * 1024) <= DTB_LOAD,
            "Image footprint reaches beyond the fixed DTB boundary")
    dtb_offset = DTB_LOAD - SHIM_LOAD
    require(set(data[SHIM_SLOT + len(image):dtb_offset]) <= {0},
            "Image/DTB padding is not all zero")
    require(data[dtb_offset:dtb_offset + len(dtb)] == dtb,
            "bundled DTB bytes do not match the supplied DTB")
    require(len(data) == dtb_offset + len(dtb),
            "unexpected bytes follow the bundled DTB")

    require(len(dtb) >= 40 and u32be(dtb[:4]) == 0xD00DFEED,
            "bundled DTB has no flattened-device-tree magic")
    dtb_total = u32be(dtb[4:8])
    require(dtb_total == len(dtb),
            f"DTB totalsize is {dtb_total}, file length is {len(dtb)}")
    require(DTB_LOAD % 8 == 0, "DTB address is not 8-byte aligned")
    require(len(dtb) <= 2 * 1024 * 1024, "DTB exceeds arm64 2 MiB limit")
    require((DTB_LOAD >> 21) == ((DTB_LOAD + len(dtb) - 1) >> 21),
            "DTB crosses a 2 MiB boundary")

    source_data_start = FASTBOOT_BUFFER_BASE + ANDROID_PAGE + 64
    source_data_end = source_data_start + len(data)
    dest_start = SHIM_LOAD
    dest_end = dest_start + len(data)
    require(no_overlap(source_data_start, source_data_end, dest_start, dest_end),
            "U-Boot source and destination ranges overlap")
    require(no_overlap(VENDOR_FDT_START, VENDOR_FDT_SLOT_END,
                       dest_start, dest_end),
            "destination overlaps vendor U-Boot's fixed 0x44000000 FDT copy")
    require(dest_end < 0x46400000,
            "destination reaches the observed low framebuffer safety boundary")
    require(dest_end < 0x48000000,
            "destination reaches bundled vendor-reserved@48000000")
    require(dest_end < 0x4A000000,
            "destination reaches resident vendor U-Boot")

    print("PASS: RG40XX V vendor-Fastboot shim is internally consistent")
    print(f"boot.img  {len(boot):>10}  sha256 {sha256(boot)}")
    print(f"Image     {len(image):>10}  sha256 {sha256(image)}")
    print(f"DTB       {len(dtb):>10}  sha256 {sha256(dtb)}")
    print(f"uImage data: 0x{SHIM_LOAD:08x}-0x{dest_end:08x} ({len(data)} bytes)")
    print(f"Image range: 0x{IMAGE_LOAD:08x}-0x{IMAGE_LOAD + effective_size:08x}")
    print(f"DTB range:   0x{DTB_LOAD:08x}-0x{DTB_LOAD + len(dtb):08x}")
    print(f"arm64 flags: 0x{flags:x}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("boot_img", type=Path,
                        help="outer Android v0 image passed to fastboot boot")
    parser.add_argument("image", type=Path, help="expected raw arm64 Image")
    parser.add_argument("dtb", type=Path, help="expected direct-root DTB")
    return parser.parse_args()


def main() -> int:
    try:
        verify(parse_args())
    except (OSError, struct.error, VerifyError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
