#!/usr/bin/env python3
"""Derive the RG40XX V BOE panel blob with this unit's stock RGB timing.

The command payload is the vendor BOE sequence up to Sleep Out.  The generic
panel driver deliberately owns the following sleep delay and Display On
lifecycle, so those commands must not also be appended to the firmware blob.
"""

import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_INPUT_SHA256 = "95bc84fdbbabdaa29a4a5934155c519a5855f19f4d26e7a4b957bb8176a1b743"
EXPECTED_MAGIC = b"PANEL-FIRMWARE\0\x01"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    blob = bytearray(args.source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit(f"unexpected source SHA256: {digest}")
    if blob[:16] != EXPECTED_MAGIC or len(blob) != 827:
        raise SystemExit("unexpected panel firmware framing")
    if blob[63] != 1:
        raise SystemExit(f"expected exactly one timing, found {blob[63]}")

    # Keep the BOE SPI register sequence. Change only RGB sampling and scan
    # timing to the values extracted from this device's stock live DT:
    # 640x480, htotal 770, vtotal 522, 24 MHz.
    struct.pack_into(">HH", blob, 32, 50, 100)
    struct.pack_into(">I", blob, 56, 0x4A)
    struct.pack_into(
        ">HHHHHHHHII",
        blob,
        64,
        640, 84, 20, 26,
        480, 27, 4, 11,
        24000, 0x5,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(f"{hashlib.sha256(blob).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()
