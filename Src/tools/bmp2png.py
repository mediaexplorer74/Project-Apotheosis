# Apotheosis 2026-09-19: BMP32 (RGBA, bottom-up) -> PNG, pure stdlib. Used to look at what the
# autodiag path actually painted. Usage:
#   python Src/tools/bmp2png.py <in.bmp> <out.png> [cropTopRows] [magnify]
import struct, sys, zlib

def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp != 32:
        raise SystemExit('expected 32bpp, got %d' % bpp)
    flip = h > 0          # positive height: bottom-up rows
    h = abs(h)
    stride = w * 4
    rows = []
    for y in range(h):
        src = h - 1 - y if flip else y
        start = off + src * stride
        rows.append(d[start:start + stride])
    return w, h, rows

def png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 6))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    crop = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    mag = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    w, h, rows = read_bmp(src)
    if crop:
        rows = rows[:crop]
        h = len(rows)
    if mag > 1:
        big = []
        for r in rows:
            px = [r[i:i + 4] for i in range(0, len(r), 4)]
            line = b''.join(p * mag for p in px)
            for _ in range(mag):
                big.append(line)
        rows, h = big, h * mag
    png(dst, w, h, rows)
    print('%s -> %s  %dx%d' % (src, dst, w, h))

main()
