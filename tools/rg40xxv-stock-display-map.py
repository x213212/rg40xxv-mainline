#!/usr/bin/env python3
"""Build a deterministic symbol and direct-call map for the stock display stack.

The stock 4.9 image happens to retain function symbols, so this deliberately uses
binutils only.  It does not pretend that an indirect callback is a direct edge;
those have to be correlated separately with the vendor source or disassembly.
"""

from __future__ import annotations

import argparse
import csv
import re
import subprocess
from collections import Counter
from pathlib import Path


DISPLAY_NAME = re.compile(
    r"^(?:"
    r"disp_|bsp_disp_|sync_event_proc$|"
    r"de_(?:rtmx|top|ovl|bld|fmt|feat|vsu|gsu|ccsc|cdc|fbd|snr|ksc|enhance|smbl|wb)_|"
    r"tcon\d?_|lcd_|panel_|sunxi_lcd_"
    r")"
)
FUNC_TYPES = frozenset("tTwW")
NM_LINE = re.compile(r"^([0-9a-fA-F]+)\s+([A-Za-z])\s+(.+)$")
LABEL = re.compile(r"^([0-9a-fA-F]+)\s+<([^>]+)>:$")
CALL = re.compile(r"\bbl\s+([0-9a-fA-F]+)\s+<([^>]+)>")


def run(*argv: str) -> str:
    return subprocess.run(argv, check=True, text=True, stdout=subprocess.PIPE).stdout


def subsystem(name: str) -> str:
    for prefix, label in (
        ("disp_lcd_", "lcd-policy"),
        ("disp_mgr_", "manager"),
        ("disp_al_", "abstraction"),
        ("de_rtmx_", "rt-mixer"),
        ("de_top_", "de-top"),
        ("de_ovl_", "overlay"),
        ("de_bld_", "blender"),
        ("de_fmt_", "formatter"),
        ("tcon", "tcon"),
        ("lcd_", "panel"),
        ("panel_", "panel"),
    ):
        if name.startswith(prefix):
            return label
    if name == "sync_event_proc":
        return "vblank"
    if name.startswith("disp_"):
        return "display-core"
    if name.startswith("de_"):
        return "de-other"
    return "other"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--elf", default="lab/reverse/stock-4.9.170-vmlinux.elf", type=Path
    )
    parser.add_argument(
        "--out", default="reports/rg40xxv-stock-4.9-display", type=Path
    )
    args = parser.parse_args()

    nm_text = run("aarch64-linux-gnu-nm", "-n", str(args.elf))
    all_funcs: list[tuple[int, str, str]] = []
    for line in nm_text.splitlines():
        match = NM_LINE.match(line)
        if not match or match.group(2) not in FUNC_TYPES:
            continue
        all_funcs.append((int(match.group(1), 16), match.group(2), match.group(3)))

    relevant = [row for row in all_funcs if DISPLAY_NAME.match(row[2])]
    if not relevant:
        raise SystemExit("no display symbols found")
    relevant_names = {name for _, _, name in relevant}
    start = min(address for address, _, _ in relevant)
    end_candidates = [address for address, _, _ in all_funcs if address > max(x[0] for x in relevant)]
    end = min(end_candidates) if end_candidates else max(x[0] for x in relevant) + 4

    disassembly = run(
        "aarch64-linux-gnu-objdump",
        "-d",
        "--no-show-raw-insn",
        f"--start-address=0x{start:x}",
        f"--stop-address=0x{end:x}",
        str(args.elf),
    )

    caller: str | None = None
    edges: Counter[tuple[str, str, int]] = Counter()
    indirect_calls: Counter[str] = Counter()
    for raw in disassembly.splitlines():
        line = raw.strip()
        label = LABEL.match(line)
        if label:
            caller = label.group(2).split("+", 1)[0]
            continue
        if caller not in relevant_names:
            continue
        direct = CALL.search(line)
        if direct:
            callee = direct.group(2).split("+", 1)[0]
            edges[(caller, callee, int(direct.group(1), 16))] += 1
        elif re.search(r"\bblr\s+x\d+", line):
            indirect_calls[caller] += 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    symbols_path = args.out.with_name(args.out.name + "-symbols.tsv")
    calls_path = args.out.with_name(args.out.name + "-direct-calls.tsv")
    summary_path = args.out.with_name(args.out.name + "-map.md")

    with symbols_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(("address", "type", "subsystem", "symbol"))
        for address, kind, name in relevant:
            writer.writerow((f"0x{address:016x}", kind, subsystem(name), name))

    with calls_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(("caller", "caller_subsystem", "callee", "callee_address", "count"))
        for (src, dst, address), count in sorted(edges.items()):
            writer.writerow((src, subsystem(src), dst, f"0x{address:016x}", count))

    counts = Counter(subsystem(name) for _, _, name in relevant)
    relevant_edge_count = sum(
        count for (src, dst, _), count in edges.items() if dst in relevant_names
    )
    lines = [
        "# RG40XX V stock 4.9 display symbol map",
        "",
        f"- ELF: `{args.elf}`",
        f"- Display functions: {len(relevant)}",
        f"- Direct call sites from display functions: {sum(edges.values())}",
        f"- Direct display-to-display call sites: {relevant_edge_count}",
        f"- Functions containing indirect calls: {len(indirect_calls)}",
        "",
        "## Subsystems",
        "",
        "| Subsystem | Functions |",
        "|---|---:|",
    ]
    lines.extend(f"| {name} | {count} |" for name, count in sorted(counts.items()))
    lines += [
        "",
        "## Generated evidence",
        "",
        f"- Symbols: `{symbols_path}`",
        f"- Direct calls: `{calls_path}`",
        "",
        "Indirect calls are counted but deliberately not resolved here. They must be "
        "correlated with the vendor callback tables or instruction-level data flow.",
    ]
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(summary_path)
    print(symbols_path)
    print(calls_path)


if __name__ == "__main__":
    main()
