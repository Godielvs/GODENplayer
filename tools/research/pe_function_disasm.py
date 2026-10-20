#!/usr/bin/env python3
"""Disassemble selected PE64 functions using the image unwind table.

This is a read-only interoperability research helper. It never modifies the
input binary and prints only the requested function bodies.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pefile
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP


def parse_address(value: str) -> int:
    return int(value, 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("addresses", nargs="+", type=parse_address)
    parser.add_argument(
        "--xrefs",
        action="store_true",
        help="print RIP-relative and immediate references to the addresses",
    )
    args = parser.parse_args()

    pe = pefile.PE(str(args.binary), fast_load=False)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    entries = sorted(
        (
            image_base + entry.struct.BeginAddress,
            image_base + entry.struct.EndAddress,
        )
        for entry in pe.DIRECTORY_ENTRY_EXCEPTION
    )

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)

    if args.xrefs:
        decoder.detail = True
        decoder.skipdata = True
        targets = {
            address if address >= image_base else image_base + address
            for address in args.addresses
        }
        for section in pe.sections:
            if section.Name.rstrip(b"\0") != b".text":
                continue
            begin = image_base + section.VirtualAddress
            code = section.get_data()
            for instruction in decoder.disasm(code, begin):
                if instruction.id == 0:
                    continue
                references: set[int] = set()
                for operand in instruction.operands:
                    if operand.type == X86_OP_IMM:
                        references.add(operand.imm)
                    elif operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                        references.add(instruction.address + instruction.size + operand.mem.disp)
                hits = references & targets
                if hits:
                    hit_text = ", ".join(f"0x{hit:x}" for hit in sorted(hits))
                    print(
                        f"0x{instruction.address:x}: {instruction.mnemonic} "
                        f"{instruction.op_str} -> {hit_text}"
                    )
        return 0

    decoder.detail = False

    for requested in args.addresses:
        address = requested if requested >= image_base else image_base + requested
        match = next((entry for entry in entries if entry[0] <= address < entry[1]), None)
        if match is None:
            print(f"0x{address:x}: no unwind-table function", file=sys.stderr)
            continue

        begin, end = match
        code = pe.get_data(begin - image_base, end - begin)
        print(f"\nfunction 0x{begin:x}..0x{end:x} (requested 0x{address:x})")
        for instruction in decoder.disasm(code, begin):
            print(
                f"  {instruction.address:016x}  "
                f"{instruction.mnemonic:<8} {instruction.op_str}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
