#!/usr/bin/env python3
"""Verify a raw 64 MiB RG40XX V legacy-uImage recovery partition."""

from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path


P8_SIZE = 64 * 1024 * 1024
UIMAGE_MAGIC = 0x27051956
SHIM_LOAD = 0x441FF000
IMAGE_LOAD = 0x44200000
DTB_LOAD = 0x46000000
SHIM_SLOT = IMAGE_LOAD - SHIM_LOAD
BOOTM_MAX = 32 * 1024 * 1024
FRAMEBUFFER_GUARD = 0x46400000
EXPECTED_SHIM = bytes.fromhex(
    "052696d20560a0f2a60040b9c6001532a60000b9a53000d1a60040b9"
    "c66c1012c6001432a60000b99f3f03d500c0a8d2e1031faae2031faa"
    "e3031faa0484a8d29f3f03d5df3f03d580001fd6"
)


class VerifyError(RuntimeError):
    pass


def require(value: bool, message: str) -> None:
    if not value:
        raise VerifyError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_regular(path: Path) -> bytes:
    require(path.is_file(), f"not a regular file: {path}")
    return path.read_bytes()


def verify(args: argparse.Namespace) -> None:
    p8 = read_regular(args.p8_image.resolve())
    image = read_regular(args.image.resolve())
    dtb = read_regular(args.dtb.resolve())
    require(len(p8) == P8_SIZE, f"p8 image is {len(p8)} bytes, expected 64 MiB")

    header = p8[:64]
    (magic, header_crc, _timestamp, data_size, load, entry, data_crc) = \
        struct.unpack_from(">7I", header, 0)
    os_id, arch, image_type, compression = struct.unpack_from("4B", header, 28)
    require(magic == UIMAGE_MAGIC, "missing legacy uImage magic")
    header_for_crc = bytearray(header)
    header_for_crc[4:8] = b"\0\0\0\0"
    require(header_crc == (zlib.crc32(header_for_crc) & 0xFFFFFFFF),
            "legacy header CRC mismatch")
    require(0 < data_size <= BOOTM_MAX, "legacy data exceeds vendor bootm limit")
    data = p8[64:64 + data_size]
    require(data_crc == (zlib.crc32(data) & 0xFFFFFFFF),
            "legacy data CRC mismatch")
    require((os_id, arch, image_type, compression) == (5, 2, 2, 0),
            "legacy type must be Linux/ARM/kernel/uncompressed")
    require(load == SHIM_LOAD and entry == SHIM_LOAD,
            "legacy load/entry address mismatch")
    require(set(p8[64 + data_size:]) <= {0}, "p8 tail is not deterministic zero padding")

    require(data[:len(EXPECTED_SHIM)] == EXPECTED_SHIM,
            "AArch64 shim bytes differ from the audited version")
    require(set(data[len(EXPECTED_SHIM):SHIM_SLOT]) <= {0},
            "shim slot padding is not zero")
    require(data[SHIM_SLOT:SHIM_SLOT + len(image)] == image,
            "embedded arm64 Image differs from supplied Image")
    dtb_offset = DTB_LOAD - SHIM_LOAD
    require(set(data[SHIM_SLOT + len(image):dtb_offset]) <= {0},
            "Image-to-DTB padding is not zero")
    require(data[dtb_offset:dtb_offset + len(dtb)] == dtb,
            "embedded DTB differs from supplied DTB")
    require(len(data) == dtb_offset + len(dtb),
            "unexpected bytes follow embedded DTB")

    require(len(image) >= 64 and image[56:60] == b"ARM\x64",
            "embedded kernel lacks arm64 Image magic")
    text_offset, effective_size, _flags = struct.unpack_from("<QQQ", image, 8)
    require(text_offset == 0, "arm64 Image text_offset is not zero")
    require(effective_size >= len(image), "arm64 effective size is too small")
    require(IMAGE_LOAD + effective_size <= DTB_LOAD,
            "arm64 effective range reaches the fixed DTB address")
    require(SHIM_LOAD + data_size < FRAMEBUFFER_GUARD,
            "destination bundle reaches the framebuffer guard")
    require(len(dtb) >= 40 and struct.unpack_from(">I", dtb, 0)[0] == 0xD00DFEED,
            "embedded DTB magic is invalid")
    require(struct.unpack_from(">I", dtb, 4)[0] == len(dtb),
            "embedded DTB totalsize mismatch")

    # sunxi_flash loads the uImage at 0x45000000.  This source overlaps the
    # lower destination bundle, but legacy bootm uses memmove_wd and treats
    # BOOTM_ERR_OVERLAP as a warning; unlike Android/FIT it does not reset.
    source_start = 0x45000000 + 64
    source_end = source_start + data_size
    destination_end = SHIM_LOAD + data_size
    require(SHIM_LOAD < source_start < destination_end < source_end,
            "unexpected legacy source/destination overlap geometry")

    print("PASS: RG40XX V persistent raw legacy-uImage p8 is internally consistent")
    print(f"p8       {len(p8):>10}  sha256 {sha256(p8)}")
    print(f"uImage   {64 + data_size:>10}  sha256 {sha256(p8[:64 + data_size])}")
    print(f"Image    {len(image):>10}  sha256 {sha256(image)}")
    print(f"DTB      {len(dtb):>10}  sha256 {sha256(dtb)}")
    print(f"destination: 0x{SHIM_LOAD:08x}-0x{destination_end:08x}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("p8_image", type=Path)
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
