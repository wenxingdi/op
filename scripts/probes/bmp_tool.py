"""零依赖 BMP -> PNG 工具（沙箱内无 numpy/PIL 时的兜底）。

支持 24/32bpp 未压缩 BMP（含 top-down，biHeight<0），输出 8bit RGB PNG。

用法:
    python workbench/bmp_tool.py in.bmp out.png [--scale 0.5] [--max WxH]
"""

import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP: " + path)

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    if header_size < 40:
        raise ValueError("unsupported BMP header size %d" % header_size)
    if compression != 0:
        raise ValueError("unsupported BMP compression %d" % compression)
    if bpp not in (24, 32):
        raise ValueError("unsupported BMP bpp %d" % bpp)

    top_down = height < 0
    height = abs(height)
    stride = ((width * bpp // 8) + 3) // 4 * 4

    rows = []
    for row_index in range(height):
        src_row = row_index if top_down else (height - 1 - row_index)
        base = pixel_offset + src_row * stride
        line = data[base:base + width * bpp // 8]
        if bpp == 32:
            rgb = bytearray(width * 3)
            for x in range(width):
                b, g, r = line[x * 4], line[x * 4 + 1], line[x * 4 + 2]
                rgb[x * 3] = r
                rgb[x * 3 + 1] = g
                rgb[x * 3 + 2] = b
            rows.append(bytes(rgb))
        else:
            rgb = bytearray(width * 3)
            for x in range(width):
                b, g, r = line[x * 3], line[x * 3 + 1], line[x * 3 + 2]
                rgb[x * 3] = r
                rgb[x * 3 + 1] = g
                rgb[x * 3 + 2] = b
            rows.append(bytes(rgb))
    return width, height, rows


def downsample_nn(width, height, rows, out_w, out_h):
    out = []
    for y in range(out_h):
        src_y = min(height - 1, y * height // out_h)
        src = rows[src_y]
        line = bytearray(out_w * 3)
        for x in range(out_w):
            src_x = min(width - 1, x * width // out_w)
            line[x * 3] = src[src_x * 3]
            line[x * 3 + 1] = src[src_x * 3 + 1]
            line[x * 3 + 2] = src[src_x * 3 + 2]
        out.append(bytes(line))
    return out


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + row for row in rows)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    src, dst = args[0], args[1]
    width, height, rows = read_bmp(src)
    print("read %s -> %dx%d" % (src, width, height))

    scale = 1.0
    for index, item in enumerate(sys.argv):
        if item == "--scale":
            scale = float(sys.argv[index + 1])
        elif item == "--max":
            tokens = sys.argv[index + 1].lower().split("x")
            limit_w, limit_h = int(tokens[0]), int(tokens[1])
            scale = min(1.0, limit_w / float(width), limit_h / float(height))

    if scale < 1.0:
        out_w = max(1, int(width * scale))
        out_h = max(1, int(height * scale))
        rows = downsample_nn(width, height, rows, out_w, out_h)
        width, height = out_w, out_h

    write_png(dst, width, height, rows)
    print("wrote %s -> %dx%d" % (dst, width, height))


if __name__ == "__main__":
    main()
