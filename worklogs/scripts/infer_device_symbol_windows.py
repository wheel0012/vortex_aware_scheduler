#!/usr/bin/env python3

import argparse
import csv
import subprocess
import sys
from pathlib import Path


def parse_hex(value):
    return int(value, 16)


def load_symbols(nm_path, elf_path):
    result = subprocess.run(
        [str(nm_path), "-S", "-n", str(elf_path)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    startup = None
    symbols = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        if len(fields) >= 4:
            addr_s, size_s, kind, name = fields[:4]
        else:
            addr_s, kind, name = fields[:3]
            size_s = "0"
        if name == "STARTUP_ADDR":
            startup = parse_hex(addr_s)
        if kind in {"T", "t"}:
            symbols.append((name, parse_hex(addr_s), parse_hex(size_s)))
    return startup, symbols, result.stdout


def main():
    parser = argparse.ArgumentParser(
        description="Infer per-symbol PC windows from saved Vortex device ELF symbols."
    )
    parser.add_argument("--elf-dir", type=Path, required=True)
    parser.add_argument("--symbols", required=True, help="space or comma separated symbol names")
    parser.add_argument("--nm", type=Path, required=True, help="llvm-nm path")
    parser.add_argument("--pc-base", default="0x80000000")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--nm-out", type=Path, default=None)
    args = parser.parse_args()

    wanted = [s for s in args.symbols.replace(",", " ").split() if s]
    if not wanted:
        raise SystemExit("no symbol names were provided")

    elf_files = sorted(args.elf_dir.glob("*.elf"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not elf_files:
        raise SystemExit(f"no saved ELF files found in {args.elf_dir}")

    wanted_set = set(wanted)
    matches_by_name = {}
    used_elf = None
    used_startup = None
    used_nm = ""
    for elf_path in elf_files:
        startup, symbols, nm_text = load_symbols(args.nm, elf_path)
        if startup is None:
            continue
        matches = [(name, addr, size) for name, addr, size in symbols if name in wanted_set and size > 0]
        if matches:
            matches_by_name = {name: (addr, size) for name, addr, size in matches}
            used_elf = elf_path
            used_startup = startup
            used_nm = nm_text
            break

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["symbol", "pc_base", "pc_from", "pc_to", "addr", "size", "elf"],
        )
        writer.writeheader()
        if used_elf is not None:
            for name in wanted:
                if name not in matches_by_name:
                    continue
                addr, size = matches_by_name[name]
                pc_from = addr - used_startup
                pc_to = addr + size - 4 - used_startup
                writer.writerow({
                    "symbol": name,
                    "pc_base": args.pc_base,
                    "pc_from": f"0x{pc_from:x}",
                    "pc_to": f"0x{pc_to:x}",
                    "addr": f"0x{addr:x}",
                    "size": f"0x{size:x}",
                    "elf": str(used_elf),
                })

    if args.nm_out and used_nm:
        args.nm_out.write_text(used_nm, encoding="utf-8")

    if used_elf is None:
        print(f"no matching symbols found: {' '.join(wanted)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
