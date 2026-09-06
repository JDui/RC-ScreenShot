"""Precise pixel dump of specific tool regions from bar_dump BMPs."""
import struct
import sys
from collections import Counter


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = abs(struct.unpack_from("<i", data, 22)[0])
    px = data[off:]
    row = w * 4
    pixels = []
    for y in range(h):
        pixels.append(px[y * row:(y + 1) * row])
    return w, h, pixels


def px(pixels, x, y):
    b, g, r, a = pixels[y][x*4], pixels[y][x*4+1], pixels[y][x*4+2], pixels[y][x*4+3]
    return f"#{r:02X}{g:02X}{b:02X}/{a}"


def colors_in(pixels, x0, y0, x1, y1):
    c = Counter()
    for y in range(y0, y1):
        for x in range(x0, x1):
            b, g, r, a = pixels[y][x*4], pixels[y][x*4+1], pixels[y][x*4+2], pixels[y][x*4+3]
            c[(r, g, b, a)] += 1
    return c


def show(pixels, x0, y0, x1, y1, label, det):
    c = colors_in(pixels, x0, y0, x1, y1)
    print(f"== {label} ({x0},{y0})-({x1},{y1}) n_colors={len(c)}")
    for (r, g, b, a), n in c.most_common(det):
        print(f"   #{r:02X}{g:02X}{b:02X} a={a} x{n}")
    print()


w, h, pixels = load_bmp(sys.argv[1])
show(pixels, 40, 294, 40+36, 294+36, "tool0 pen", 4)
show(pixels, 81, 294, 81+36, 294+36, "tool1 rectangle", 4)
show(pixels, 560, 294, 560+40, 294+36, "copy", 4)
show(pixels, 605, 294, 605+40, 294+36, "save", 4)
show(pixels, 524, 296, 562, 334, "entry", 6)
# utility strip
show(pixels, 618, 262, 640, 284, "back disc", 6)
show(pixels, 640, 262, 662, 284, "close disc", 6)
# property row y=260+94=354 .. 354+? and size slider area
show(pixels, 40, 354, 76, 390, "prop0", 4)
show(pixels, 120, 354, 156, 390, "prop1", 4)