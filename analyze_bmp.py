"""Analyze bar_dump BMPs: verify tool icons, long entry glyph, utility buttons."""
import struct
import sys
from collections import Counter


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"BM", "not a BMP"
    off = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<IiiHHIIiiII", data, 14)
    w, h = hdr[1], hdr[2]
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 32, f"bpp={bpp}"
    topdown = h < 0
    h = abs(h)
    px = data[off:]
    row = w * 4
    pixels = []
    for y in range(h):
        src = y if topdown else (h - 1 - y)
        pixels.append(px[src * row:(src + 1) * row])
    return w, h, pixels


def px(pixels, x, y):
    """Return (b,g,r,a) as bytes."""
    row = pixels[y]
    i = x * 4
    return row[i], row[i+1], row[i+2], row[i+3]


def scan_region(w, h, pixels, x0, y0, x1, y1, label):
    """Summarize colors in a region."""
    colors = Counter()
    for y in range(y0, y1):
        for x in range(x0, x1):
            colors[px(pixels, x, y)] += 1
    total = (x1 - x0) * (y1 - y0)
    print(f"== {label}: {x1-x0}x{y1-y0} @ ({x0},{y0}), {total} px, {len(colors)} colors")
    for color, count in colors.most_common(8):
        b, g, r, a = color
        print(f"   #{r:02X}{g:02X}{b:02X} a={a} x{count} ({count*100//total}%)")
    print()


def count_bright(region_pixels):
    return sum(1 for (b, g, r, a) in region_pixels if r > 180 and g > 180 and b > 180 and a > 200)


def report_region(w, h, pixels, x0, y0, x1, y1, label):
    region = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            region.append(px(pixels, x, y))
    print(f"-- {label}: bright(white) px = {count_bright(region)} / {len(region)}")
    blue = sum(1 for (b, g, r, a) in region if 80 < b < 240 and 60 < g < 200 and r < 90)
    redish = sum(1 for (b, g, r, a) in region if r > 140 and g < 120 and b < 120)
    print(f"-- {label}: blue-ish px = {blue}, red-ish px = {redish}")


def main(path):
    w, h, pixels = load_bmp(path)
    print(f"{path}: {w}x{h}")
    # Toolbar geometry from dump3 headers: edit bar (636x168 @ 32,260)
    bar_left, bar_top, bar_right, bar_bottom = 32, 260, 668, 428
    print("\nTOOLBAR region (32,260)-(668,428)")
    report_region(w, h, pixels, bar_left, bar_top, bar_right, bar_bottom, "toolbar")
    # Tool button row: y = 260 + strip + margin .. +60ish ; x = 32+15 .. 32+15+6*48
    # Force: sample a strip across the first tool row.
    report_region(w, h, pixels, 50, 300, 330, 340, "tool-row-strip")
    # Long capture entry: centered ~ (541,314) per dump (529..561, 295..334)
    report_region(w, h, pixels, 524, 296, 562, 334, "entry")
    # Utility buttons: back ~ (609..637), close ~ (636..656) at y 264..284
    report_region(w, h, pixels, 600, 262, 668, 292, "utility-strip")
    scan_region(w, h, pixels, 600, 262, 668, 292, "utility-strip colors")
    scan_region(w, h, pixels, 524, 296, 562, 334, "entry colors")


if __name__ == "__main__":
    main(sys.argv[1])