"""thumb.py in.png cx cy cw ch out.png out.bin [ow oh]

Crop a capture to a window centred on (cx, cy), box-downsample it to the thumbnail size, and write
both a PNG to look at and the raw blob the screen saver embeds.

The blob is: width u32 LE, height u32 LE, then w*h BGRA pixels, top row first. BGRA rather than BGR
because a 32-bit DIB needs no row padding, and top-down because a BITMAPINFOHEADER with a negative
height reads it in that order -- between them that is a StretchDIBits call with nothing to get
wrong.
"""
import struct
import sys

from png_io import read_png, write_png

src = sys.argv[1]
cx, cy, cw, ch = (int(v) for v in sys.argv[2:6])
out_png, out_bin = sys.argv[6], sys.argv[7]
ow, oh = (int(v) for v in sys.argv[8:10]) if len(sys.argv) > 8 else (320, 240)

w, h, rows = read_png(src)

x0 = max(0, min(cx - cw // 2, w - cw))
y0 = max(0, min(cy - ch // 2, h - ch))

out_rows = []
blob = bytearray()
for j in range(oh):
    sy0 = y0 + j * ch // oh
    sy1 = max(sy0 + 1, y0 + (j + 1) * ch // oh)
    line = bytearray()
    for i in range(ow):
        sx0 = x0 + i * cw // ow
        sx1 = max(sx0 + 1, x0 + (i + 1) * cw // ow)
        r = g = b = n = 0
        for sy in range(sy0, sy1):
            row = rows[sy]
            for sx in range(sx0, sx1):
                o = sx * 3
                r += row[o]
                g += row[o + 1]
                b += row[o + 2]
                n += 1
        r //= n
        g //= n
        b //= n
        line += bytes((r, g, b))
        blob += bytes((b, g, r, 255))
    out_rows.append(line)

write_png(out_png, ow, oh, out_rows)
with open(out_bin, 'wb') as f:
    f.write(struct.pack('<II', ow, oh))
    f.write(bytes(blob))

print('%s: %dx%d crop at (%d,%d) -> %dx%d, %d bytes' %
      (out_bin, cw, ch, x0, y0, ow, oh, 8 + len(blob)))
