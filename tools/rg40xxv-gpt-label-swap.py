#!/usr/bin/env python3
"""Crash-aware GPT label switch for one captured RG40XX V TF1 card.

This intentionally is not a general GPT editor.  It recognizes two complete,
byte-exact GPT states and changes only the UTF-16LE names of entries 4 and 8,
plus the CRC fields made stale by that change.  LBA0 is read and pinned by
SHA-256 but is never included in the write set.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import dataclasses
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import uuid
import zlib


PROFILE = "rg40xxv-current-tf1-20260825-p8-v4-persistent"
EXPECTED_BLOCK_PATH = "/dev/mmcblk0"
EXPECTED_CID = "00343253443136472000000054019aff"
EXPECTED_BLOCK_MAJOR = 179
EXPECTED_BLOCK_MINOR = 0
EXPECTED_LBA0_SHA256 = (
    "594b1dcc5c905e3ed19c53d45bf66df8b74bb84637b71d047afc9f59dfa52414"
)

SECTOR_SIZE = 512
DISK_SECTORS = 122_101_760
DISK_BYTES = DISK_SECTORS * SECTOR_SIZE
PRIMARY_HEADER_LBA = 1
PRIMARY_TABLE_LBA = 2
BACKUP_TABLE_LBA = 122_101_757
BACKUP_HEADER_LBA = 122_101_759
FIRST_USABLE_LBA = 4
LAST_USABLE_LBA = 122_101_756
ENTRY_COUNT = 8
ENTRY_SIZE = 128
TABLE_BYTES = ENTRY_COUNT * ENTRY_SIZE
HEADER_BYTES = 92
DISK_GUID = "ab6f3888-569a-4926-9668-80941dcb40bc"
DATA_TYPE_GUID = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"

STATE_STOCK = "stock-named"
STATE_ACTIVATED = "linux7-activated"
STATE_NAMES = {
    STATE_STOCK: {4: "boot", 8: "recovery"},
    STATE_ACTIVATED: {4: "recovery", 8: "boot"},
}

CONFIRM = {
    "activate": "RG40XXV-ACTIVATE-P4-P8",
    "deactivate": "RG40XXV-DEACTIVATE-P4-P8",
    "rollback": "RG40XXV-ROLLBACK-P4-P8",
}

BUNDLE_FORMAT = "rg40xxv-gpt-label-swap-rollback-v1"
MAX_BUNDLE_BYTES = 64 * 1024

# Linux block ioctls.  These values are identical on the target arm64 kernel.
BLKSSZGET = 0x1268
BLKGETSIZE64 = 0x80081272

HASH_CHUNK_BYTES = 1024 * 1024
PAYLOAD_GUARDS = (
    (
        "vendor_uboot",
        0x01004800,
        0x00100000,
        "c5242baec52ca91bcee0c93c523d4c2679325b5473186d3b46cac6b75a86ecbe",
    ),
    (
        "p3_env",
        92_413_952 * SECTOR_SIZE,
        16 * 1024 * 1024,
        "00f1f2862d95a4b89076e3ecd95322003dcd650e0450351fe7723221a9a4424f",
    ),
    (
        "p4_stock_boot",
        92_446_720 * SECTOR_SIZE,
        64 * 1024 * 1024,
        "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519",
    ),
    (
        "p8_linux7_boot",
        120_879_104 * SECTOR_SIZE,
        64 * 1024 * 1024,
        "7ffcba47d57bcf1dd00bc0e1856f7619ab83535777f93afbce8f11ea73312c03",
    ),
)


class Refusal(RuntimeError):
    """An expected safety-gate refusal, reported with exit status 2."""


@dataclasses.dataclass(frozen=True)
class Partition:
    number: int
    unique_guid: str
    first_lba: int
    last_lba: int
    attributes: int
    stock_name: str


PARTITIONS = (
    Partition(
        1,
        "a0085546-4166-744a-a353-fca9272b8e45",
        73_728,
        92_348_415,
        0x0000200000000000,
        "Roms",
    ),
    Partition(
        2,
        "a0085546-4166-744a-a353-fca9272b8e46",
        92_348_416,
        92_413_951,
        0xC000200000000000,
        "boot-resource",
    ),
    Partition(
        3,
        "a0085546-4166-744a-a353-fca9272b8e47",
        92_413_952,
        92_446_719,
        0xC000200000000000,
        "env",
    ),
    Partition(
        4,
        "a0085546-4166-744a-a353-fca9272b8e48",
        92_446_720,
        92_577_791,
        0xC000200000000000,
        "boot",
    ),
    Partition(
        5,
        "a0085546-4166-744a-a353-fca9272b8e49",
        92_577_792,
        107_257_855,
        0xC000200000000000,
        "rootfs",
    ),
    Partition(
        6,
        "a0085546-4166-744a-a353-fca9272b8e4a",
        107_257_856,
        115_646_463,
        0xC000200000000000,
        "appfs",
    ),
    Partition(
        7,
        "a0085546-4166-744a-a353-fca9272b8e4b",
        115_646_464,
        120_879_103,
        0xC000000000000000,
        "UDISK",
    ),
    Partition(
        8,
        "a0085546-4166-744a-a353-fca9272b8e4c",
        120_879_104,
        121_010_175,
        0xC000200000000000,
        "recovery",
    ),
)


@dataclasses.dataclass(frozen=True)
class Snapshot:
    primary_header: bytes
    primary_table: bytes
    backup_table: bytes
    backup_header: bytes

    def components(self) -> tuple[bytes, bytes, bytes, bytes]:
        return (
            self.primary_header,
            self.primary_table,
            self.backup_table,
            self.backup_header,
        )

    def metadata_sha256(self) -> str:
        return sha256(b"".join(self.components()))


@dataclasses.dataclass(frozen=True)
class TargetInfo:
    path: str
    kind: str
    logical_bytes: int
    sector_size: int
    cid: str | None


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def utf16le_name(name: str) -> bytes:
    if "\0" in name:
        raise AssertionError("embedded NUL in a compiled-in GPT name")
    encoded = name.encode("utf-16le")
    if len(encoded) > 72:
        raise AssertionError("compiled-in GPT name exceeds 36 UTF-16 code units")
    return encoded.ljust(72, b"\0")


def table_for_state(state_name: str) -> bytes:
    if state_name not in STATE_NAMES:
        raise AssertionError(f"unknown compiled-in state: {state_name}")
    names = STATE_NAMES[state_name]
    entries = bytearray(TABLE_BYTES)
    for part in PARTITIONS:
        name = names.get(part.number, part.stock_name)
        entry = bytearray(ENTRY_SIZE)
        entry[0:16] = uuid.UUID(DATA_TYPE_GUID).bytes_le
        entry[16:32] = uuid.UUID(part.unique_guid).bytes_le
        struct.pack_into(
            "<QQQ", entry, 32, part.first_lba, part.last_lba, part.attributes
        )
        entry[56:128] = utf16le_name(name)
        start = (part.number - 1) * ENTRY_SIZE
        entries[start : start + ENTRY_SIZE] = entry
    return bytes(entries)


def header_for_table(table: bytes, *, backup: bool) -> bytes:
    if len(table) != TABLE_BYTES:
        raise AssertionError("wrong compiled-in table size")
    table_crc = zlib.crc32(table) & 0xFFFFFFFF
    current_lba = BACKUP_HEADER_LBA if backup else PRIMARY_HEADER_LBA
    other_lba = PRIMARY_HEADER_LBA if backup else BACKUP_HEADER_LBA
    table_lba = BACKUP_TABLE_LBA if backup else PRIMARY_TABLE_LBA
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        b"EFI PART",
        0x00010000,
        HEADER_BYTES,
        0,
        0,
        current_lba,
        other_lba,
        FIRST_USABLE_LBA,
        LAST_USABLE_LBA,
        uuid.UUID(DISK_GUID).bytes_le,
        table_lba,
        ENTRY_COUNT,
        ENTRY_SIZE,
        table_crc,
    )
    header_crc = zlib.crc32(header[:HEADER_BYTES]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, header_crc)
    return bytes(header)


def expected_snapshot(state_name: str) -> Snapshot:
    table = table_for_state(state_name)
    return Snapshot(
        primary_header=header_for_table(table, backup=False),
        primary_table=table,
        backup_table=table,
        backup_header=header_for_table(table, backup=True),
    )


EXPECTED = {
    STATE_STOCK: expected_snapshot(STATE_STOCK),
    STATE_ACTIVATED: expected_snapshot(STATE_ACTIVATED),
}

# Audited hashes make an accidental change to the compiled profile fail at
# startup instead of silently defining a new card identity.
KNOWN_METADATA_SHA256 = {
    STATE_STOCK: "246354e921d08ad63d61bf90ecd6938a0db81517b1b1c5cec2cd6ca42906c3ca",
    STATE_ACTIVATED: "4c186e78cbef347a32177b5c507c8f86fe70b6fcb4cadf2dc8bfb551c5c8eb1a",
}
for _state, _digest in KNOWN_METADATA_SHA256.items():
    if EXPECTED[_state].metadata_sha256() != _digest:
        raise RuntimeError(f"compiled GPT profile self-check failed for {_state}")


def pread_exact(fd: int, length: int, offset: int) -> bytes:
    chunks: list[bytes] = []
    done = 0
    while done < length:
        try:
            chunk = os.pread(fd, length - done, offset + done)
        except InterruptedError:
            continue
        if not chunk:
            raise Refusal(
                f"short read at byte {offset + done}; target is not the exact card image"
            )
        chunks.append(chunk)
        done += len(chunk)
    return b"".join(chunks)


def pwrite_exact(fd: int, data: bytes, offset: int) -> None:
    done = 0
    while done < len(data):
        try:
            count = os.pwrite(fd, data[done:], offset + done)
        except InterruptedError:
            continue
        if count <= 0:
            raise OSError(f"short write at byte {offset + done}")
        done += count


def pwrite_sectors(fd: int, data: bytes, offset: int) -> None:
    if offset % SECTOR_SIZE or len(data) % SECTOR_SIZE:
        raise AssertionError("GPT write is not an exact whole-sector region")
    for relative in range(0, len(data), SECTOR_SIZE):
        pwrite_exact(fd, data[relative : relative + SECTOR_SIZE], offset + relative)


def fsync_retry(fd: int) -> None:
    while True:
        try:
            os.fsync(fd)
            return
        except InterruptedError:
            continue


def hash_region(fd: int, offset: int, length: int) -> str:
    hasher = hashlib.sha256()
    done = 0
    while done < length:
        count = min(HASH_CHUNK_BYTES, length - done)
        hasher.update(pread_exact(fd, count, offset + done))
        done += count
    return hasher.hexdigest()


def validate_payload_guards(fd: int) -> dict[str, str]:
    results: dict[str, str] = {}
    for name, offset, length, expected in PAYLOAD_GUARDS:
        actual = hash_region(fd, offset, length)
        if actual != expected:
            raise Refusal(
                f"payload hash mismatch for {name}: got {actual}, expected {expected}"
            )
        results[name] = actual
    return results


def read_snapshot(fd: int) -> Snapshot:
    return Snapshot(
        primary_header=pread_exact(fd, SECTOR_SIZE, PRIMARY_HEADER_LBA * SECTOR_SIZE),
        primary_table=pread_exact(fd, TABLE_BYTES, PRIMARY_TABLE_LBA * SECTOR_SIZE),
        backup_table=pread_exact(fd, TABLE_BYTES, BACKUP_TABLE_LBA * SECTOR_SIZE),
        backup_header=pread_exact(fd, SECTOR_SIZE, BACKUP_HEADER_LBA * SECTOR_SIZE),
    )


def read_lba0(fd: int) -> bytes:
    return pread_exact(fd, SECTOR_SIZE, 0)


def ioctl_uint(fd: int, request: int, size: int) -> int:
    buf = bytearray(size)
    fcntl.ioctl(fd, request, buf, True)
    return int.from_bytes(buf, sys.byteorder)


def inspect_target_info(
    fd: int, target: str, *, allow_regular_write: bool, writing: bool
) -> TargetInfo:
    st = os.fstat(fd)
    if stat.S_ISREG(st.st_mode):
        if writing and not allow_regular_write:
            raise Refusal(
                "regular-file writes require --allow-regular-file (test clones only)"
            )
        logical_bytes = st.st_size
        sector_size = SECTOR_SIZE
        kind = "regular-file"
        cid = None
    elif stat.S_ISBLK(st.st_mode):
        if target != EXPECTED_BLOCK_PATH:
            raise Refusal(
                f"block target must be the literal {EXPECTED_BLOCK_PATH}; no aliases or scanning"
            )
        if (
            os.major(st.st_rdev) != EXPECTED_BLOCK_MAJOR
            or os.minor(st.st_rdev) != EXPECTED_BLOCK_MINOR
        ):
            raise Refusal(
                "block major/minor mismatch: "
                f"got {os.major(st.st_rdev)}:{os.minor(st.st_rdev)}, expected "
                f"{EXPECTED_BLOCK_MAJOR}:{EXPECTED_BLOCK_MINOR}"
            )
        logical_bytes = ioctl_uint(fd, BLKGETSIZE64, 8)
        sector_size = ioctl_uint(fd, BLKSSZGET, 4)
        cid_path = Path("/sys/class/block/mmcblk0/device/cid")
        try:
            cid = cid_path.read_text(encoding="ascii").strip().lower()
        except OSError as exc:
            raise Refusal(f"cannot read exact-card CID guard: {exc}") from exc
        if cid != EXPECTED_CID:
            raise Refusal(f"CID mismatch: got {cid!r}")
        kind = "block-device"
        if writing and os.geteuid() != 0:
            raise Refusal("block-device apply requires root")
    else:
        raise Refusal("target must be a regular sparse clone or the exact block device")

    if logical_bytes != DISK_BYTES:
        raise Refusal(f"byte-size mismatch: got {logical_bytes}, expected {DISK_BYTES}")
    if sector_size != SECTOR_SIZE:
        raise Refusal(
            f"logical-sector mismatch: got {sector_size}, expected {SECTOR_SIZE}"
        )
    return TargetInfo(target, kind, logical_bytes, sector_size, cid)


@contextlib.contextmanager
def opened_target(target: str, *, writing: bool, allow_regular_write: bool):
    flags = os.O_RDWR if writing else os.O_RDONLY
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(target, flags)
    except OSError as exc:
        mode = "read/write" if writing else "read-only"
        raise Refusal(f"cannot open target {mode}: {exc}") from exc
    try:
        lock = fcntl.LOCK_EX if writing else fcntl.LOCK_SH
        try:
            fcntl.flock(fd, lock | fcntl.LOCK_NB)
        except OSError as exc:
            raise Refusal(f"cannot lock target: {exc}") from exc
        info = inspect_target_info(
            fd, target, allow_regular_write=allow_regular_write, writing=writing
        )
        yield fd, info
    finally:
        os.close(fd)


def validate_lba0(lba0: bytes) -> None:
    actual = sha256(lba0)
    if actual != EXPECTED_LBA0_SHA256:
        raise Refusal(
            f"LBA0 hash mismatch: got {actual}, expected {EXPECTED_LBA0_SHA256}"
        )


def validate_gpt_copy(header: bytes, table: bytes, *, backup: bool) -> None:
    if len(header) != SECTOR_SIZE or len(table) != TABLE_BYTES:
        raise Refusal("internal GPT read length mismatch")
    if header[:8] != b"EFI PART":
        raise Refusal("GPT signature mismatch")
    revision, size, stored_crc, reserved = struct.unpack_from("<IIII", header, 8)
    if revision != 0x00010000 or size != HEADER_BYTES or reserved != 0:
        raise Refusal("GPT header format mismatch")
    if any(header[HEADER_BYTES:]):
        raise Refusal("GPT header padding is not the captured all-zero value")
    crc_input = bytearray(header[:size])
    struct.pack_into("<I", crc_input, 16, 0)
    if (zlib.crc32(crc_input) & 0xFFFFFFFF) != stored_crc:
        raise Refusal("GPT header CRC mismatch")
    fields = struct.unpack_from("<QQQQ16sQIII", header, 24)
    (
        current_lba,
        other_lba,
        first,
        last,
        disk_guid,
        table_lba,
        count,
        esize,
        tcrc,
    ) = fields
    expected_current = BACKUP_HEADER_LBA if backup else PRIMARY_HEADER_LBA
    expected_other = PRIMARY_HEADER_LBA if backup else BACKUP_HEADER_LBA
    expected_table = BACKUP_TABLE_LBA if backup else PRIMARY_TABLE_LBA
    if (
        current_lba != expected_current
        or other_lba != expected_other
        or first != FIRST_USABLE_LBA
        or last != LAST_USABLE_LBA
        or disk_guid != uuid.UUID(DISK_GUID).bytes_le
        or table_lba != expected_table
        or count != ENTRY_COUNT
        or esize != ENTRY_SIZE
    ):
        raise Refusal("GPT geometry/disk-GUID fields do not match the exact profile")
    if (zlib.crc32(table) & 0xFFFFFFFF) != tcrc:
        raise Refusal("GPT entry-array CRC mismatch")


def classify_exact(snapshot: Snapshot) -> str:
    for state_name, expected in EXPECTED.items():
        if snapshot == expected:
            validate_gpt_copy(
                snapshot.primary_header, snapshot.primary_table, backup=False
            )
            validate_gpt_copy(
                snapshot.backup_header, snapshot.backup_table, backup=True
            )
            return state_name
    hashes = component_hashes(snapshot)
    raise Refusal(
        "GPT metadata is not either audited state; component hashes="
        + json.dumps(hashes, sort_keys=True, separators=(",", ":"))
    )


def component_hashes(snapshot: Snapshot) -> dict[str, str]:
    return {
        "primary_header": sha256(snapshot.primary_header),
        "primary_table": sha256(snapshot.primary_table),
        "backup_table": sha256(snapshot.backup_table),
        "backup_header": sha256(snapshot.backup_header),
        "metadata": snapshot.metadata_sha256(),
    }


def changed_offsets(before: bytes, after: bytes) -> set[int]:
    if len(before) != len(after):
        raise AssertionError("diff inputs have different lengths")
    return {
        index for index, pair in enumerate(zip(before, after)) if pair[0] != pair[1]
    }


def assert_only_names_and_crcs_change(before: Snapshot, after: Snapshot) -> None:
    allowed_table = set(range(3 * ENTRY_SIZE + 56, 4 * ENTRY_SIZE))
    allowed_table.update(range(7 * ENTRY_SIZE + 56, 8 * ENTRY_SIZE))
    allowed_header = set(range(16, 20)) | set(range(88, 92))
    for old, new, allowed, label in (
        (before.primary_table, after.primary_table, allowed_table, "primary table"),
        (before.backup_table, after.backup_table, allowed_table, "backup table"),
        (before.primary_header, after.primary_header, allowed_header, "primary header"),
        (before.backup_header, after.backup_header, allowed_header, "backup header"),
    ):
        diff = changed_offsets(old, new)
        if not diff or not diff <= allowed:
            raise AssertionError(f"compiled profile changes forbidden bytes in {label}")


assert_only_names_and_crcs_change(EXPECTED[STATE_STOCK], EXPECTED[STATE_ACTIVATED])


def exact_copy_is_valid(
    header: bytes, table: bytes, before: Snapshot, after: Snapshot, *, backup: bool
) -> bool:
    if backup:
        return (header == before.backup_header and table == before.backup_table) or (
            header == after.backup_header and table == after.backup_table
        )
    return (header == before.primary_header and table == before.primary_table) or (
        header == after.primary_header and table == after.primary_table
    )


def assert_transaction_components(
    snapshot: Snapshot, before: Snapshot, after: Snapshot
) -> None:
    for label, value, old, new in zip(
        ("primary header", "primary table", "backup table", "backup header"),
        snapshot.components(),
        before.components(),
        after.components(),
    ):
        if value != old and value != new:
            raise Refusal(
                f"{label} is neither transaction endpoint (torn/foreign write)"
            )
    primary_ok = exact_copy_is_valid(
        snapshot.primary_header,
        snapshot.primary_table,
        before,
        after,
        backup=False,
    )
    backup_ok = exact_copy_is_valid(
        snapshot.backup_header,
        snapshot.backup_table,
        before,
        after,
        backup=True,
    )
    if not primary_ok and not backup_ok:
        raise Refusal("transaction state has no exact valid GPT copy")


def write_snapshot_ordered(
    fd: int,
    current: Snapshot,
    destination: Snapshot,
    before: Snapshot,
    after: Snapshot,
    lba0_before: bytes,
    *,
    stage_hook=None,
) -> Snapshot:
    """Write backup table/header, then primary table/header, syncing each stage."""
    stages = (
        ("backup-table", BACKUP_TABLE_LBA * SECTOR_SIZE, destination.backup_table),
        ("backup-header", BACKUP_HEADER_LBA * SECTOR_SIZE, destination.backup_header),
        ("primary-table", PRIMARY_TABLE_LBA * SECTOR_SIZE, destination.primary_table),
        (
            "primary-header",
            PRIMARY_HEADER_LBA * SECTOR_SIZE,
            destination.primary_header,
        ),
    )
    snapshot = current
    for stage_name, offset, data in stages:
        pwrite_sectors(fd, data, offset)
        fsync_retry(fd)
        if pread_exact(fd, len(data), offset) != data:
            raise OSError(f"read-back mismatch after {stage_name}")
        if read_lba0(fd) != lba0_before:
            raise OSError(f"LBA0 changed unexpectedly after {stage_name}")
        snapshot = read_snapshot(fd)
        assert_transaction_components(snapshot, before, after)
        if stage_hook is not None:
            stage_hook(stage_name, snapshot)
    return snapshot


def snapshot_to_json(snapshot: Snapshot) -> dict[str, str]:
    return {
        "primary_header": base64.b64encode(snapshot.primary_header).decode("ascii"),
        "primary_table": base64.b64encode(snapshot.primary_table).decode("ascii"),
        "backup_table": base64.b64encode(snapshot.backup_table).decode("ascii"),
        "backup_header": base64.b64encode(snapshot.backup_header).decode("ascii"),
        "metadata_sha256": snapshot.metadata_sha256(),
    }


def snapshot_from_json(value: object) -> Snapshot:
    if not isinstance(value, dict):
        raise Refusal("rollback snapshot is not an object")
    try:
        decoded = {
            key: base64.b64decode(value[key], validate=True)
            for key in (
                "primary_header",
                "primary_table",
                "backup_table",
                "backup_header",
            )
        }
        snapshot = Snapshot(**decoded)
        claimed = value["metadata_sha256"]
    except (KeyError, TypeError, ValueError) as exc:
        raise Refusal(f"invalid rollback snapshot: {exc}") from exc
    if not isinstance(claimed, str) or snapshot.metadata_sha256() != claimed:
        raise Refusal("rollback snapshot digest mismatch")
    return snapshot


def canonical_json(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")


def bundle_with_digest(payload: dict[str, object]) -> dict[str, object]:
    result = dict(payload)
    result["bundle_sha256"] = sha256(canonical_json(payload))
    return result


def raw_segment(lba: int, data: bytes) -> dict[str, object]:
    if len(data) % SECTOR_SIZE:
        raise AssertionError("rollback preimage is not whole-sector sized")
    return {
        "lba": lba,
        "count": len(data) // SECTOR_SIZE,
        "sha256": sha256(data),
        "data_base64": base64.b64encode(data).decode("ascii"),
    }


def make_bundle(
    operation: str, before_state: str, after_state: str, lba0: bytes
) -> dict[str, object]:
    if sha256(lba0) != EXPECTED_LBA0_SHA256:
        raise AssertionError("cannot bundle an unrecognized LBA0")
    before = EXPECTED[before_state]
    payload: dict[str, object] = {
        "format": BUNDLE_FORMAT,
        "profile": PROFILE,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "operation_uuid": str(uuid.uuid4()),
        "operation": operation,
        "before_state": before_state,
        "after_state": after_state,
        "target": {
            "logical_bytes": DISK_BYTES,
            "sector_size": SECTOR_SIZE,
            "block_path": EXPECTED_BLOCK_PATH,
            "cid": EXPECTED_CID,
            "disk_guid": DISK_GUID,
            "lba0_sha256": EXPECTED_LBA0_SHA256,
            "payload_sha256": {
                name: expected for name, _offset, _length, expected in PAYLOAD_GUARDS
            },
        },
        "before": snapshot_to_json(before),
        "after": snapshot_to_json(EXPECTED[after_state]),
        "raw_preimages": [
            raw_segment(0, lba0),
            raw_segment(PRIMARY_HEADER_LBA, before.primary_header),
            raw_segment(PRIMARY_TABLE_LBA, before.primary_table),
            raw_segment(BACKUP_TABLE_LBA, before.backup_table),
            raw_segment(BACKUP_HEADER_LBA, before.backup_header),
        ],
        "write_order": [
            "backup-table",
            "backup-header",
            "primary-table",
            "primary-header",
        ],
    }
    return bundle_with_digest(payload)


def validate_bundle(bundle: object) -> tuple[str, str, Snapshot, Snapshot]:
    if not isinstance(bundle, dict):
        raise Refusal("rollback bundle is not a JSON object")
    claimed = bundle.get("bundle_sha256")
    payload = {key: value for key, value in bundle.items() if key != "bundle_sha256"}
    if not isinstance(claimed, str) or sha256(canonical_json(payload)) != claimed:
        raise Refusal("rollback bundle digest mismatch")
    if bundle.get("format") != BUNDLE_FORMAT or bundle.get("profile") != PROFILE:
        raise Refusal("rollback bundle format/profile mismatch")
    target = bundle.get("target")
    expected_target = {
        "logical_bytes": DISK_BYTES,
        "sector_size": SECTOR_SIZE,
        "block_path": EXPECTED_BLOCK_PATH,
        "cid": EXPECTED_CID,
        "disk_guid": DISK_GUID,
        "lba0_sha256": EXPECTED_LBA0_SHA256,
        "payload_sha256": {
            name: expected for name, _offset, _length, expected in PAYLOAD_GUARDS
        },
    }
    if target != expected_target:
        raise Refusal("rollback bundle target identity mismatch")
    try:
        uuid.UUID(str(bundle.get("operation_uuid")))
    except (ValueError, AttributeError) as exc:
        raise Refusal("rollback bundle operation UUID is invalid") from exc
    before_state = bundle.get("before_state")
    after_state = bundle.get("after_state")
    if (
        not isinstance(before_state, str)
        or not isinstance(after_state, str)
        or before_state not in EXPECTED
        or after_state not in EXPECTED
        or before_state == after_state
    ):
        raise Refusal("rollback bundle endpoint states are invalid")
    before = snapshot_from_json(bundle.get("before"))
    after = snapshot_from_json(bundle.get("after"))
    if before != EXPECTED[before_state] or after != EXPECTED[after_state]:
        raise Refusal("rollback bundle bytes differ from the audited endpoints")
    raw_preimages = bundle.get("raw_preimages")
    if not isinstance(raw_preimages, list) or len(raw_preimages) != 5:
        raise Refusal("rollback raw-preimage list is invalid")
    expected_preimages: list[tuple[int, bytes | None]] = [
        (0, None),
        (PRIMARY_HEADER_LBA, before.primary_header),
        (PRIMARY_TABLE_LBA, before.primary_table),
        (BACKUP_TABLE_LBA, before.backup_table),
        (BACKUP_HEADER_LBA, before.backup_header),
    ]
    lba0_preimage: bytes | None = None
    for index, (expected_lba, expected_data) in enumerate(expected_preimages):
        segment = raw_preimages[index]
        if not isinstance(segment, dict):
            raise Refusal("rollback raw-preimage entry is invalid")
        try:
            data = base64.b64decode(segment["data_base64"], validate=True)
        except (KeyError, TypeError, ValueError) as exc:
            raise Refusal("rollback raw-preimage data is invalid") from exc
        if (
            segment.get("lba") != expected_lba
            or segment.get("count")
            != (1 if expected_data is None else len(expected_data) // SECTOR_SIZE)
            or segment.get("sha256") != sha256(data)
            or (expected_data is not None and data != expected_data)
        ):
            raise Refusal("rollback raw-preimage position/hash/data mismatch")
        if index == 0:
            lba0_preimage = data
    if lba0_preimage is None or sha256(lba0_preimage) != EXPECTED_LBA0_SHA256:
        raise Refusal("rollback LBA0 preimage mismatch")
    if bundle.get("write_order") != [
        "backup-table",
        "backup-header",
        "primary-table",
        "primary-header",
    ]:
        raise Refusal("rollback bundle write-order marker mismatch")
    return before_state, after_state, before, after


def write_new_bundle(path_text: str, bundle: dict[str, object]) -> None:
    path = Path(path_text)
    if not path.is_absolute():
        raise Refusal("--rollback-out must be an absolute path")
    parent = path.parent
    data = canonical_json(bundle) + b"\n"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags, 0o600)
    except OSError as exc:
        raise Refusal(f"cannot create new rollback bundle: {exc}") from exc
    try:
        done = 0
        while done < len(data):
            try:
                count = os.write(fd, data[done:])
            except InterruptedError:
                continue
            if count <= 0:
                raise OSError("short rollback-bundle write")
            done += count
        fsync_retry(fd)
    except Exception:
        os.close(fd)
        raise
    else:
        os.close(fd)
    try:
        dirfd = os.open(parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        try:
            fsync_retry(dirfd)
        finally:
            os.close(dirfd)
    except OSError as exc:
        raise Refusal(f"cannot sync rollback-bundle directory: {exc}") from exc
    loaded = load_bundle(path_text)
    if loaded != bundle:
        raise Refusal("rollback bundle read-back mismatch")


def load_bundle(path_text: str) -> dict[str, object]:
    path = Path(path_text)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise Refusal(f"cannot open rollback bundle: {exc}") from exc
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            raise Refusal("rollback bundle must be a regular file")
        if st.st_size <= 0 or st.st_size > MAX_BUNDLE_BYTES:
            raise Refusal("rollback bundle size is invalid")
        data = pread_exact(fd, st.st_size, 0)
    finally:
        os.close(fd)
    try:
        value = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise Refusal(f"rollback bundle is invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise Refusal("rollback bundle is not an object")
    validate_bundle(value)
    return value


# Transaction component bits are ordered PH, PT, BT, BH; 0 is the bundle's
# before endpoint and 1 its after endpoint.  These are precisely the states
# reachable by the forward write order or an interrupted reverse write order.
ALLOWED_TRANSACTION_BITS = {
    (0, 0, 0, 0): "before",
    (0, 0, 1, 0): "forward-after-backup-table",
    (0, 0, 1, 1): "forward-after-backup-header",
    (0, 1, 1, 1): "forward-after-primary-table",
    (1, 1, 1, 1): "after",
    (1, 1, 0, 1): "rollback-after-backup-table",
    (1, 1, 0, 0): "rollback-after-backup-header",
    (1, 0, 0, 0): "rollback-after-primary-table",
    # Reachable when rollback starts from forward-after-backup-header and is
    # interrupted after restoring only the old backup table.  The old primary
    # copy remains valid throughout this state.
    (0, 0, 0, 1): "rollback-from-forward-after-backup-table",
}


def transaction_stage(
    snapshot: Snapshot, before: Snapshot, after: Snapshot
) -> tuple[str, tuple[int, int, int, int]]:
    bits: list[int] = []
    for label, current, old, new in zip(
        ("primary header", "primary table", "backup table", "backup header"),
        snapshot.components(),
        before.components(),
        after.components(),
    ):
        if current == old:
            bits.append(0)
        elif current == new:
            bits.append(1)
        else:
            raise Refusal(f"{label} does not match either rollback endpoint")
    key = tuple(bits)
    if key not in ALLOWED_TRANSACTION_BITS:
        raise Refusal(
            f"metadata combination is not a reachable transaction stage: {key}"
        )
    assert_transaction_components(snapshot, before, after)
    return ALLOWED_TRANSACTION_BITS[key], key


def copy_endpoint(
    snapshot: Snapshot, before: Snapshot, after: Snapshot, *, backup: bool
) -> int | None:
    if backup:
        pair = (snapshot.backup_header, snapshot.backup_table)
        if pair == (before.backup_header, before.backup_table):
            return 0
        if pair == (after.backup_header, after.backup_table):
            return 1
    else:
        pair = (snapshot.primary_header, snapshot.primary_table)
        if pair == (before.primary_header, before.primary_table):
            return 0
        if pair == (after.primary_header, after.primary_table):
            return 1
    return None


def rollback_topology(
    snapshot: Snapshot, before: Snapshot, after: Snapshot
) -> tuple[int | None, int | None]:
    primary = copy_endpoint(snapshot, before, after, backup=False)
    backup = copy_endpoint(snapshot, before, after, backup=True)
    if primary is None and backup is None:
        raise Refusal(
            "rollback target has no exact valid transaction-endpoint GPT copy"
        )
    return primary, backup


def endpoint_text(endpoint: int | None) -> str:
    return {0: "before-valid", 1: "after-valid", None: "invalid"}[endpoint]


def write_one_gpt_copy(
    fd: int,
    destination: Snapshot,
    before: Snapshot,
    after: Snapshot,
    lba0_before: bytes,
    *,
    backup: bool,
) -> None:
    if backup:
        label = "backup"
        table_lba = BACKUP_TABLE_LBA
        header_lba = BACKUP_HEADER_LBA
        table = destination.backup_table
        header = destination.backup_header
    else:
        label = "primary"
        table_lba = PRIMARY_TABLE_LBA
        header_lba = PRIMARY_HEADER_LBA
        table = destination.primary_table
        header = destination.primary_header

    pwrite_sectors(fd, table, table_lba * SECTOR_SIZE)
    fsync_retry(fd)
    if pread_exact(fd, len(table), table_lba * SECTOR_SIZE) != table:
        raise OSError(f"read-back mismatch after {label} table")
    if read_lba0(fd) != lba0_before:
        raise OSError(f"LBA0 changed after {label} table")
    interim = read_snapshot(fd)
    rollback_topology(interim, before, after)

    pwrite_sectors(fd, header, header_lba * SECTOR_SIZE)
    fsync_retry(fd)
    if pread_exact(fd, len(header), header_lba * SECTOR_SIZE) != header:
        raise OSError(f"read-back mismatch after {label} header")
    if read_lba0(fd) != lba0_before:
        raise OSError(f"LBA0 changed after {label} header")
    committed = read_snapshot(fd)
    endpoint = copy_endpoint(committed, before, after, backup=backup)
    if endpoint != 0:
        raise OSError(f"{label} rollback copy did not commit the before endpoint")


def print_status(
    info: TargetInfo, state: str, snapshot: Snapshot, payload_hashes: dict[str, str]
) -> None:
    hashes = component_hashes(snapshot)
    print("PASS RG40XXV_GPT_LABEL_SWAP_INSPECT")
    print(f"profile={PROFILE}")
    print(
        f"target_kind={info.kind} bytes={info.logical_bytes} sector={info.sector_size}"
    )
    if info.cid is not None:
        print(f"cid={info.cid}")
    print(f"state={state} p4={STATE_NAMES[state][4]} p8={STATE_NAMES[state][8]}")
    print(f"disk_guid={DISK_GUID} lba0_sha256={EXPECTED_LBA0_SHA256}")
    print(f"metadata_sha256={hashes['metadata']}")
    print(
        "payload_sha256="
        + ",".join(f"{name}:{value}" for name, value in payload_hashes.items())
    )
    print("write_set=NONE")


def inspect_command(args: argparse.Namespace) -> int:
    with opened_target(args.target, writing=False, allow_regular_write=False) as (
        fd,
        info,
    ):
        lba0 = read_lba0(fd)
        validate_lba0(lba0)
        payload_hashes = validate_payload_guards(fd)
        snapshot = read_snapshot(fd)
        state = classify_exact(snapshot)
        if args.json:
            result = {
                "ok": True,
                "profile": PROFILE,
                "target": dataclasses.asdict(info),
                "state": state,
                "p4": STATE_NAMES[state][4],
                "p8": STATE_NAMES[state][8],
                "lba0_sha256": sha256(lba0),
                "hashes": component_hashes(snapshot),
                "payload_hashes": payload_hashes,
                "write_set": [],
            }
            print(json.dumps(result, sort_keys=True))
        else:
            print_status(info, state, snapshot, payload_hashes)
    return 0


def transition_command(args: argparse.Namespace) -> int:
    operation = args.command
    before_state, after_state = (
        (STATE_STOCK, STATE_ACTIVATED)
        if operation == "activate"
        else (STATE_ACTIVATED, STATE_STOCK)
    )
    writing = bool(args.apply)
    if writing:
        if args.confirm != CONFIRM[operation]:
            raise Refusal(
                f"apply confirmation mismatch; required: --confirm {CONFIRM[operation]}"
            )
        if not args.rollback_out:
            raise Refusal("--rollback-out is mandatory before any apply")

    with opened_target(
        args.target,
        writing=writing,
        allow_regular_write=bool(args.allow_regular_file),
    ) as (fd, info):
        lba0 = read_lba0(fd)
        validate_lba0(lba0)
        payload_hashes = validate_payload_guards(fd)
        current = read_snapshot(fd)
        current_state = classify_exact(current)
        if current_state == after_state:
            print(f"NO-OP already={after_state} write_set=NONE")
            return 0
        if current_state != before_state:
            raise Refusal(f"{operation} requires {before_state}, found {current_state}")
        destination = EXPECTED[after_state]
        print(
            f"{'APPLY' if writing else 'DRY-RUN'} operation={operation} "
            f"from={before_state} to={after_state}"
        )
        print(
            f"labels=p4:{STATE_NAMES[before_state][4]}->{STATE_NAMES[after_state][4]},"
            f"p8:{STATE_NAMES[before_state][8]}->{STATE_NAMES[after_state][8]}"
        )
        print(
            "write_order="
            f"LBA{BACKUP_TABLE_LBA}-{BACKUP_TABLE_LBA + 1}:table,"
            f"LBA{BACKUP_HEADER_LBA}:header,"
            f"LBA{PRIMARY_TABLE_LBA}-{PRIMARY_TABLE_LBA + 1}:table,"
            f"LBA{PRIMARY_HEADER_LBA}:header"
        )
        print(
            "payload_sha256="
            + ",".join(f"{name}:{value}" for name, value in payload_hashes.items())
        )
        if not writing:
            print(f"required_confirmation={CONFIRM[operation]}")
            print("write_set=NONE")
            return 0

        bundle = make_bundle(operation, before_state, after_state, lba0)
        write_new_bundle(args.rollback_out, bundle)

        # The target remains locked, but re-read after the durable bundle to
        # catch changes by non-cooperating writers before our first pwrite.
        info_recheck = inspect_target_info(
            fd,
            args.target,
            allow_regular_write=bool(args.allow_regular_file),
            writing=True,
        )
        if info_recheck != info:
            raise Refusal("target identity changed after rollback bundle creation")
        if validate_payload_guards(fd) != payload_hashes:
            raise Refusal(
                "payload changed after rollback bundle creation; no writes made"
            )
        if read_lba0(fd) != lba0 or read_snapshot(fd) != current:
            raise Refusal(
                "target changed after rollback bundle creation; no writes made"
            )

        try:
            final = write_snapshot_ordered(
                fd, current, destination, current, destination, lba0
            )
        except Exception as exc:
            raise Refusal(
                f"write stopped: {exc}; keep rollback bundle {args.rollback_out!r}"
            ) from exc
        if final != destination or classify_exact(final) != after_state:
            raise Refusal("post-write exact-state verification failed")
        if read_lba0(fd) != lba0:
            raise Refusal("post-write LBA0 preservation check failed")
        print(f"PASS RG40XXV_GPT_LABEL_SWAP_{operation.upper()}")
        print(f"rollback_bundle={args.rollback_out}")
        print(f"state={after_state} metadata_sha256={final.metadata_sha256()}")
        print("lba0=UNCHANGED geometry_guids_attributes=UNCHANGED")
        print("reboot_required=yes partprobe=NOT_RUN")
    return 0


def rollback_command(args: argparse.Namespace) -> int:
    bundle = load_bundle(args.from_bundle)
    before_state, after_state, before, after = validate_bundle(bundle)
    writing = bool(args.apply)
    if writing and args.confirm != CONFIRM["rollback"]:
        raise Refusal(
            f"apply confirmation mismatch; required: --confirm {CONFIRM['rollback']}"
        )

    with opened_target(
        args.target,
        writing=writing,
        allow_regular_write=bool(args.allow_regular_file),
    ) as (fd, info):
        del info
        lba0 = read_lba0(fd)
        validate_lba0(lba0)
        validate_payload_guards(fd)
        current = read_snapshot(fd)
        primary_endpoint, backup_endpoint = rollback_topology(current, before, after)
        if primary_endpoint == 0 and backup_endpoint == 0:
            print(f"NO-OP rollback_already={before_state} write_set=NONE")
            return 0
        topology = (
            f"primary={endpoint_text(primary_endpoint)},"
            f"backup={endpoint_text(backup_endpoint)}"
        )
        # If primary is invalid, the backup is the only exact valid copy: keep
        # it untouched while committing the desired primary copy.  Otherwise
        # retain the normal backup-first ordering.  This also recovers torn
        # table/header writes so long as one recognized copy remains valid.
        copy_order = (False, True) if primary_endpoint is None else (True, False)
        order_text = "primary,backup" if copy_order[0] is False else "backup,primary"
        print(
            f"{'APPLY' if writing else 'DRY-RUN'} operation=rollback "
            f"topology={topology} restore={before_state}"
        )
        print(f"copy_order={order_text}; each_copy=table,header")
        if not writing:
            print(f"required_confirmation={CONFIRM['rollback']}")
            print("write_set=NONE")
            return 0

        try:
            for backup_copy in copy_order:
                write_one_gpt_copy(
                    fd,
                    before,
                    before,
                    after,
                    lba0,
                    backup=backup_copy,
                )
            final = read_snapshot(fd)
        except Exception as exc:
            raise Refusal(
                f"rollback stopped: {exc}; retain bundle {args.from_bundle!r}"
            ) from exc
        if final != before or classify_exact(final) != before_state:
            raise Refusal("rollback exact-state verification failed")
        if read_lba0(fd) != lba0:
            raise Refusal("rollback LBA0 preservation check failed")
        print("PASS RG40XXV_GPT_LABEL_SWAP_ROLLBACK")
        print(f"state={before_state} metadata_sha256={final.metadata_sha256()}")
        print("lba0=UNCHANGED geometry_guids_attributes=UNCHANGED")
        print("reboot_required=yes partprobe=NOT_RUN")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Exact-card RG40XX V GPT p4/p8 label activator. Commands are "
            "read-only unless --apply is supplied."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser(
        "inspect", help="strict read-only identity/state check"
    )
    inspect_parser.add_argument("target")
    inspect_parser.add_argument("--json", action="store_true")
    inspect_parser.set_defaults(handler=inspect_command)

    for command, help_text in (
        ("activate", "swap p4=boot,p8=recovery to p4=recovery,p8=boot"),
        ("deactivate", "swap the two names back to the stock-named state"),
    ):
        sub = subparsers.add_parser(command, help=help_text)
        sub.add_argument("target")
        sub.add_argument(
            "--apply",
            action="store_true",
            help="perform the guarded writes; without this flag it is a dry-run",
        )
        sub.add_argument("--rollback-out", help="new, durable rollback-bundle path")
        sub.add_argument("--confirm")
        sub.add_argument(
            "--allow-regular-file",
            action="store_true",
            help="permit apply to an exact-size regular-file test clone",
        )
        sub.set_defaults(handler=transition_command)

    rollback_parser = subparsers.add_parser(
        "rollback", help="restore an apply from its durable rollback bundle"
    )
    rollback_parser.add_argument("target")
    rollback_parser.add_argument("--from", dest="from_bundle", required=True)
    rollback_parser.add_argument(
        "--apply",
        action="store_true",
        help="perform the guarded writes; without this flag it is a dry-run",
    )
    rollback_parser.add_argument("--confirm")
    rollback_parser.add_argument(
        "--allow-regular-file",
        action="store_true",
        help="permit apply to an exact-size regular-file test clone",
    )
    rollback_parser.set_defaults(handler=rollback_command)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.handler(args))
    except Refusal as exc:
        print(f"REFUSE: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print(
            "REFUSE: interrupted; if apply began, retain and use the rollback bundle",
            file=sys.stderr,
        )
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
