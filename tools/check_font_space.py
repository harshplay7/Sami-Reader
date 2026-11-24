#!/usr/bin/env python3
"""Check that the X4 reader fonts still fit next to the firmware.

The ESP32-C3 sees flash through an 8 MB window: 128 pages of 64 KB, shared by
the running firmware's code and data and by everything mapped later. The X4
uses its reader font straight from the font partition, one text size at a time
(platforms/esp32/font_manager.h), so what's left after the firmware decides
how many sizes can be loaded together.

Usage: check_font_space.py firmware.bin partitions.csv font.mfb [font.mfb ...]

Prints, for each font, how many of its sizes fit around the default Font Size.
Fails (exit 1) if any single size of a font can't fit at all, which would leave
the reader without its book font.
"""

from __future__ import annotations

import struct
import sys

PAGE = 0x10000
TOTAL_PAGES = 128
RESERVE_PAGES = 6  # kept free by the font manager (kReservePages)
HEADER_SECTOR = 4096  # the font partition's own header comes first
DEFAULT_BASE = 1  # default Font Size (22 px)
HEADING_PCTS = (120, 140, 80, 160)  # same order as microreader/FontWindow.h


def mapped_pages(firmware: bytes) -> int:
    """64 KB pages of the window the firmware's flash segments take."""
    if firmware[0] != 0xE9:
        raise SystemExit("not an ESP app image")
    count = firmware[1]
    off = 24  # common header + extended header
    pages = set()
    for _ in range(count):
        addr, length = struct.unpack_from("<II", firmware, off)
        off += 8 + length
        # Code (0x42...) and read-only data (0x3C...) share one set of pages on the C3.
        if 0x3C000000 <= addr < 0x3C800000 or 0x42000000 <= addr < 0x42800000:
            start = addr & 0x7FFFFF
            for p in range(start // PAGE, (start + length - 1) // PAGE + 1):
                pages.add(p)
    return len(pages)


def font_partition_address(csv_path: str) -> int:
    with open(csv_path, encoding="utf-8") as f:
        for line in f:
            cols = [c.strip() for c in line.split("#", 1)[0].split(",")]
            if len(cols) >= 4 and cols[0] == "spiffs":
                return int(cols[3], 0)
    raise SystemExit("no spiffs partition in " + csv_path)


def sizes_of(bundle: bytes):
    """(offset, length, pixel size) of each text size in an FNTS bundle."""
    if bundle[:4] != b"FNTS":
        raise SystemExit("not an FNTS font bundle")
    num = bundle[4]
    lengths = [struct.unpack_from("<I", bundle, 40 + 4 * i)[0] for i in range(num)]
    off = 40 + 4 * num
    out = []
    for n in lengths:
        nominal = struct.unpack_from("<H", bundle, off + 14)[0]
        out.append((off, n, nominal))
        off += n
    return out


def load_order(nominal: list[int], base: int) -> list[int]:
    order = [base]
    for pct in HEADING_PCTS:
        target = nominal[base] * pct
        best = min(range(len(nominal)), key=lambda i: abs(nominal[i] * 100 - target))
        if best not in order:
            order.append(best)
    for d in range(1, len(nominal)):
        for i in (base + d, base - d):
            if 0 <= i < len(nominal) and i not in order:
                order.append(i)
    return order


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    with open(sys.argv[1], "rb") as f:
        firmware = f.read()
    part_addr = font_partition_address(sys.argv[2])
    used = mapped_pages(firmware)
    free = TOTAL_PAGES - used
    print(f"Firmware uses {used} of {TOTAL_PAGES} flash window pages; {free} free ({free * 64} KB).")

    ok = True
    for path in sys.argv[3:]:
        with open(path, "rb") as f:
            sizes = sizes_of(f.read())

        def pages(i: int) -> int:
            start = part_addr + HEADER_SECTOR + sizes[i][0]
            return (start + sizes[i][1] - 1) // PAGE - start // PAGE + 1

        biggest = max(pages(i) for i in range(len(sizes)))
        nominal = [s[2] for s in sizes]
        base = min(DEFAULT_BASE, len(sizes) - 1)
        left = free
        loaded = []
        for i in load_order(nominal, base):
            need = pages(i)
            if i != base and left < need + RESERVE_PAGES:
                continue
            if need > left:
                continue
            left -= need
            loaded.append(nominal[i])
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        print(f"  {name}: all sizes need {sum(pages(i) for i in range(len(sizes)))} pages; "
              f"at {nominal[base]} px loads {sorted(loaded)} px")
        if biggest > free:
            print(f"::error::{name}: its {biggest}-page size can't fit in the {free} free pages. "
                  "The firmware is too big for the reader to load this font.")
            ok = False
        elif len(loaded) < 3:
            print(f"::warning::{name}: only {len(loaded)} sizes fit; headings will look smaller.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
