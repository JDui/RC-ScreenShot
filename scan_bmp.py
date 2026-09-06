"""Search the whole frame for white glyph pixels and red pixels."""
import struct
import sys


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


w, h, pixels = load_bmp(sys.argv[1])
white = []
red = []
bright_blue = []
for y in range(h):
    row = pixels[y]
    for x in range(w):
        i = x * 4
        r, g, b = row[i+2], row[i+1], row[i]
        if r > 235 and g > 235 and b > 235:
            white.append((x, y))
        elif r > 150 and g < 110 and b < 110:
            red.append((x, y))
        elif b > 180 and r < 60 and 80 < g < 170:
            bright_blue.append((x, y))
print(f"white(>235) px: {len(white)}")
if white:
    xs = [p[0] for p in white]; ys = [p[1] for p in white]
    print(f"  x range {min(xs)}..{max(xs)}, y range {min(ys)}..{max(ys)}")
    # cluster by 48px buckets
    from collections import Counter
    buckets = Counter((p[0] // 64, p[1] // 64) for p in white)
    print("  top buckets:", buckets.most_common(12))
print(f"red px: {len(red)}")
if red:
    xs = [p[0] for p in red]; ys = [p[1] for p in red]
    print(f"  x range {min(xs)}..{max(xs)}, y range {min(ys)}..{max(ys)}")
    from collections import Counter
    buckets = Counter((p[0] // 64, p[1] // 64) for p in red)
    print("  top buckets:", buckets.most_common(12))
print(f"bright-blue px: {len(bright_blue)}")
if bright_blue:
    from collections import Counter
    buckets = Counter((p[0] // 64, p[1] // 64) for p in bright_blue)
    print("  top buckets:", buckets.most_common(12))