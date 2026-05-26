#!/usr/bin/env python3

import argparse
import subprocess
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
        description="Infer analyzer PC filter args from saved Vortex device ELF symbols."
    )
    parser.add_argument("--elf-dir", type=Path, required=True)
    parser.add_argument("--symbols", required=True, help="space or comma separated symbol names")
    parser.add_argument("--nm", type=Path, required=True, help="llvm-nm path")
    parser.add_argument("--pc-base", default="0x80000000")
    parser.add_argument("--summary", type=Path, default=None)
    args = parser.parse_args()

    wanted = [s for s in args.symbols.replace(",", " ").split() if s]
    if not wanted:
        raise SystemExit("no symbol names were provided")

    elf_files = sorted(args.elf_dir.glob("*.elf"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not elf_files:
        raise SystemExit(f"no saved ELF files found in {args.elf_dir}")

    matches = []
    used_elf = None
    used_startup = None
    used_nm = ""
    for elf_path in elf_files:
        startup, symbols, nm_text = load_symbols(args.nm, elf_path)
        if startup is None:
            continue
        found = [(name, addr, size) for name, addr, size in symbols if name in wanted and size > 0]
        if found:
            matches = found
            used_elf = elf_path
            used_startup = startup
            used_nm = nm_text
            break

    if not matches:
        available = []
        if elf_files:
            startup, symbols, nm_text = load_symbols(args.nm, elf_files[0])
            available = [name for name, _, size in symbols if size > 0]
            used_nm = nm_text
        if args.summary:
            args.summary.write_text(
                "status: no_match\n"
                f"elf_dir: {args.elf_dir}\n"
                f"wanted: {' '.join(wanted)}\n"
                f"available_text_symbols: {' '.join(available[:80])}\n",
                encoding="utf-8",
            )
        raise SystemExit(f"none of the requested symbols were found: {' '.join(wanted)}")

    pc_from = min(addr - used_startup for _, addr, _ in matches)
    pc_to = max(addr + size - 4 - used_startup for _, addr, size in matches)

    if args.summary:
        lines = [
            "status: ok",
            f"elf: {used_elf}",
            f"startup_addr: 0x{used_startup:x}",
            f"pc_base: {args.pc_base}",
            f"pc_from: 0x{pc_from:x}",
            f"pc_to: 0x{pc_to:x}",
            "symbols:",
        ]
        for name, addr, size in matches:
            lines.append(f"  {name}: addr=0x{addr:x}, size=0x{size:x}, offset=0x{addr - used_startup:x}")
        args.summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
        (args.summary.parent / "device_symbols.nm").write_text(used_nm, encoding="utf-8")

    print("--pc-base")
    print(args.pc_base)
    print("--pc-from")
    print(f"0x{pc_from:x}")
    print("--pc-to")
    print(f"0x{pc_to:x}")


if __name__ == "__main__":
    main()
