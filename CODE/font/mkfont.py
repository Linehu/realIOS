#!/usr/bin/env python3
"""GNU Unifont .hex(.gz) -> realinit 的 RFNT 点阵字体。

RFNT 布局（小端）：
    "RFNT"  u32 version=1  u32 count=65536
    width[count]        0 = 无字形，1 = 8x16，2 = 16x16
    glyph[count][32]    8 宽：每行 1 字节（前 16 字节）；16 宽：每行 2 字节

用法：python3 mkfont.py unifont-16.0.04.hex.gz out.rfnt
"""
import gzip
import struct
import sys

COUNT = 0x10000


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    opener = gzip.open if src.endswith(".gz") else open
    widths = bytearray(COUNT)
    glyphs = bytearray(COUNT * 32)
    n8 = n16 = 0
    with opener(src, "rt", encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line or ":" not in line:
                continue
            cp_s, bits = line.split(":", 1)
            cp = int(cp_s, 16)
            if cp >= COUNT:
                continue
            data = bytes.fromhex(bits)
            if len(data) == 16:
                widths[cp] = 1
                n8 += 1
            elif len(data) == 32:
                widths[cp] = 2
                n16 += 1
            else:
                continue
            glyphs[cp * 32: cp * 32 + len(data)] = data
    # 空格系列必须有字形（宽度信息），内容全空即可
    for cp in (0x20, 0xA0):
        if not widths[cp]:
            widths[cp] = 1
    if not widths[0x3000]:
        widths[0x3000] = 2
    with open(dst, "wb") as out:
        out.write(b"RFNT" + struct.pack("<II", 1, COUNT))
        out.write(widths)
        out.write(glyphs)
    print(f"[font] {dst}: {n8} half-width + {n16} full-width glyphs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
