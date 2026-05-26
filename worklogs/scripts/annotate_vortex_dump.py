#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


ABI_REGS = [
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
]

UNKNOWN_RE = re.compile(
    r"^(?P<prefix>\s*[0-9a-fA-F]+:\s+)"
    r"(?P<bytes>(?:[0-9a-fA-F]{2}\s+){4})"
    r"(?P<gap>\s*)<unknown>(?P<suffix>.*)$"
)


def reg_name(index):
    if 0 <= index < len(ABI_REGS):
        return ABI_REGS[index]
    return f"x{index}"


def decode_wctl(code):
    opcode = code & 0x7f
    if opcode != 0x0B:
        return None

    rd = (code >> 7) & 0x1F
    funct3 = (code >> 12) & 0x7
    rs1 = (code >> 15) & 0x1F
    rs2 = (code >> 20) & 0x1F
    funct7 = (code >> 25) & 0x7F
    if funct7 != 0:
        return None

    if funct3 == 0:
        return f"tmc\t{reg_name(rs1)}"
    if funct3 == 1:
        return f"wspawn\t{reg_name(rs1)}, {reg_name(rs2)}"
    if funct3 == 2:
        name = "split.n" if rs2 != 0 else "split"
        return f"{name}\t{reg_name(rd)}, {reg_name(rs1)}"
    if funct3 == 3:
        return f"join\t{reg_name(rs1)}"
    if funct3 == 4:
        return f"bar\t{reg_name(rs1)}, {reg_name(rs2)}"
    if funct3 == 5:
        name = "pred.n" if rd != 0 else "pred"
        return f"{name}\t{reg_name(rs1)}, {reg_name(rs2)}"
    return None


def annotate_line(line):
    match = UNKNOWN_RE.match(line.rstrip("\n"))
    if not match:
        return line

    raw = bytes(int(part, 16) for part in match.group("bytes").split())
    code = int.from_bytes(raw, byteorder="little")
    decoded = decode_wctl(code)
    if decoded is None:
        return line

    return f"{match.group('prefix')}{match.group('bytes')}{decoded}{match.group('suffix')}\n"


def main():
    parser = argparse.ArgumentParser(
        description="Replace llvm-objdump <unknown> lines for Vortex EXT1 warp-control instructions."
    )
    parser.add_argument("dump", type=Path, help="input llvm-objdump text file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="output path; defaults to overwriting the input file",
    )
    args = parser.parse_args()

    output = args.output or args.dump
    lines = args.dump.read_text(encoding="utf-8").splitlines(keepends=True)
    output.write_text("".join(annotate_line(line) for line in lines), encoding="utf-8")


if __name__ == "__main__":
    main()
