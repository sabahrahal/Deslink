"""Genera desklink.ico (la tableta de DeskLink, degradado azul) con el mismo dibujo que CreateTabletIcon en system_info.cpp.

Uso: python3 make_icon.py  (escribe desklink.ico junto a este script). Solo hace falta si cambia el dibujo.
"""
import math
import os
import struct
import zlib

COLOR_FROM = (64, 156, 255)  # accent oscuro (design/tokens.json)
COLOR_TO = (0, 102, 204)    # accent claro
SCREEN = (28, 28, 30)       # background oscuro
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]


def round_rect_distance(x, y, x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r
    dx, dy = abs(x - cx) - hx, abs(y - cy) - hy
    ox, oy = max(dx, 0), max(dy, 0)
    outside = ox * ox + oy * oy
    inside = max(dx, dy)
    return (math.sqrt(outside) if outside > 0 else min(inside, 0)) - r


def render(size):
    rows = []
    for y in range(size):
        row = bytearray([0])  # filtro PNG «ninguno»
        for x in range(size):
            body = screen = 0
            for sy in range(4):
                for sx in range(4):
                    fx, fy = (x + (sx + 0.5) / 4) / size, (y + (sy + 0.5) / 4) / size
                    if round_rect_distance(fx, fy, 0.15, 0.27, 0.85, 0.73, 0.03) < 0:
                        screen += 1
                    elif round_rect_distance(fx, fy, 0.05, 0.16, 0.95, 0.84, 0.14) < 0:
                        body += 1
            a = (body + screen) / 16
            if a == 0:
                row += bytes(4)
                continue
            t = (x + y + 1.0) / (2 * size)
            body_rgb = [f + (e - f) * t for f, e in zip(COLOR_FROM, COLOR_TO)]
            rgb = [round((c * body + k * screen) / (body + screen)) for c, k in zip(body_rgb, SCREEN)]
            row += bytes(rgb + [round(a * 255)])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    images = [render(s) for s in SIZES]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, img in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    out += b"".join(images)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "desklink.ico"), "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
