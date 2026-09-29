# -*- coding: utf-8 -*-
"""极简 BMP(32bpp, 自下而上) -> PNG 转换，仅用标准库（本机无 PIL）。
用法: python _bmp2png.py <in.bmp> <out.png>
"""
import struct
import sys
import zlib


def convert(src, dst):
    b = open(src, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    step = bpp // 8
    if step not in (3, 4):
        raise SystemExit("only 24/32bpp supported, got %d" % bpp)
    top_down = h < 0
    h = abs(h)
    stride = ((w * step + 3) // 4) * 4   # BMP 行按 4 字节对齐
    rows = []
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        base = off + sy * stride
        # BGR[A] -> RGBA，行首过滤字节 0
        rgba = bytearray(1 + w * 4)
        if step == 4:
            row = b[base: base + w * 4]
            rgba[1::4] = row[2::4]
            rgba[2::4] = row[1::4]
            rgba[3::4] = row[0::4]
            rgba[4::4] = row[3::4]
        else:  # 24bpp 无 alpha，补 255
            for x in range(w):
                i = base + x * 3
                o = 1 + x * 4
                rgba[o] = b[i + 2]
                rgba[o + 1] = b[i + 1]
                rgba[o + 2] = b[i]
                rgba[o + 3] = 255
        rows.append(bytes(rgba))

    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    print("%s -> %s  %dx%d %dbpp" % (src, dst, w, h, bpp))


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
