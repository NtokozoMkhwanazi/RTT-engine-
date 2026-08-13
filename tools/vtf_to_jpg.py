#!/usr/bin/env python3
"""Convert Valve VTF skybox faces (DXT1/DXT5) to JPG files.

Pure-python DXT block decoder; no external deps beyond Pillow (for JPG
output). Usage:
    python3 tools/vtf_to_jpg.py <face.vtf> <out.jpg> [--face-size N]
"""
import struct
import sys
from PIL import Image

# ---- DXT decoders -----------------------------------------------------------

def _c565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def decode_dxt1_block(block):
    """8 bytes -> 4x4 list of (r,g,b,a)."""
    c0 = struct.unpack('<H', block[0:2])[0]
    c1 = struct.unpack('<H', block[2:4])[0]
    rgb0 = _c565(c0)
    rgb1 = _c565(c1)
    if c0 > c1:
        palette = [rgb0, rgb1,
                   tuple((2 * a + b) // 3 for a, b in zip(rgb0, rgb1)),
                   tuple((a + 2 * b) // 3 for a, b in zip(rgb0, rgb1))]
    else:
        palette = [rgb0, rgb1,
                   tuple((a + b) // 2 for a, b in zip(rgb0, rgb1)),
                   (0, 0, 0, 0)]
    idx = struct.unpack('<I', block[4:8])[0]
    px = []
    for i in range(16):
        code = (idx >> (2 * i)) & 3
        p = palette[code]
        if len(p) == 3:
            p = (p[0], p[1], p[2], 255)
        px.append(p)
    return px


def decode_dxt5_block(block):
    """16 bytes -> 4x4 list of (r,g,b,a)."""
    a0, a1 = block[0], block[1]
    alphas = [a0, a1]
    if a0 > a1:
        alphas += [a0 + (a1 - a0) * i // 7 for i in range(1, 7)]
    else:
        alphas += [a0 + (a1 - a0) * i // 5 for i in range(1, 5)]
        alphas += [0, 255]
    c0 = struct.unpack('<H', block[8:10])[0]
    c1 = struct.unpack('<H', block[10:12])[0]
    rgb0 = _c565(c0)
    rgb1 = _c565(c1)
    palette = [rgb0, rgb1,
               tuple((2 * a + b) // 3 for a, b in zip(rgb0, rgb1)),
               tuple((a + 2 * b) // 3 for a, b in zip(rgb0, rgb1))]
    aidx = struct.unpack('<Q', block[2:10])[0]
    cidx = struct.unpack('<I', block[12:16])[0]
    px = []
    for i in range(16):
        a = alphas[(aidx >> (3 * i)) & 7]
        c = palette[(cidx >> (2 * i)) & 3]
        px.append((c[0], c[1], c[2], a))
    return px


def decode_blocks(data, w, h, bpp, fmt):
    out = Image.new('RGBA', (w, h))
    px = out.load()
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    if fmt == 0x0C:  # DXT1
        decode = decode_dxt1_block
        blk = 8
    elif fmt in (0x0D, 0x0E, 0x0F):  # DXT5, RGBA8888, RGBA8888 (bc3)
        decode = decode_dxt5_block
        blk = 16
    else:
        raise ValueError('unsupported high-res format %s' % hex(fmt))
    off = 0
    for by in range(bh):
        for bx in range(bw):
            b = data[off:off + blk]
            off += blk
            if len(b) < blk:
                continue
            block = decode(b)
            for i, (r, g, bb, a) in enumerate(block):
                x = bx * 4 + (i % 4)
                y = by * 4 + (i // 4)
                if x < w and y < h:
                    px[x, y] = (r, g, bb, a)
    return out


# ---- VTF parsing ------------------------------------------------------------

def parse_vtf(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[0:4] != b'VTF\x00':
        raise ValueError('%s: not a VTF file' % path)
    major, minor = struct.unpack('<II', data[4:12])
    hsize = struct.unpack('<I', data[12:16])[0]
    w, h = struct.unpack('<HH', data[16:20])
    flags = struct.unpack('<I', data[20:24])[0]
    frames = struct.unpack('<H', data[24:26])[0]
    highfmt = struct.unpack('<I', data[52:56])[0]
    mipcount = data[56]
    numres = struct.unpack('<I', data[71:75])[0] if hsize >= 80 else 0
    return {
        'major': major, 'minor': minor, 'hsize': hsize, 'w': w, 'h': h,
        'flags': flags, 'frames': frames, 'highfmt': highfmt,
        'mipcount': mipcount, 'numres': numres, 'data': data,
    }


def extract_mip0(info):
    """Return decoded RGBA image of the first (largest) mip level."""
    w, h = info['w'], info['h']
    fmt = info['highfmt']
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    if fmt in (0x0C,):      # DXT1
        mipsize = bw * bh * 8
    elif fmt in (0x0D,):    # DXT5
        mipsize = bw * bh * 16
    elif fmt in (0x02, 0x03):  # RGB888 / BGR888 uncompressed
        mipsize = w * h * 3
    else:
        raise ValueError('unsupported format %s' % hex(fmt))
    # Data layout: lowres thumbnail (if any) first, then high-res mips.
    loww, lowh = info.get('loww', 0), info.get('lowh', 0)
    # We parse from header 80; resources start right after header.
    data = info['data']
    pos = info['hsize']
    if info['numres'] > 0:
        # skip resource directory entries
        pos = 80 + info['numres'] * 8
    mip_data = data[pos:pos + mipsize]
    if fmt in (0x02, 0x03):
        img = Image.frombytes('RGB', (w, h), mip_data, 'raw', 'BGR' if fmt == 0x03 else 'RGB')
        return img.convert('RGBA')
    return decode_blocks(mip_data, w, h, 4, fmt)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    src = args[0]
    out = args[1] if len(args) > 1 else src + '.jpg'
    info = parse_vtf(src)
    img = extract_mip0(info)
    # VTF rows are stored top-down; PIL matches that already.
    print('%s: %dx%d fmt=%s mips=%d -> %s' % (
        src, info['w'], info['h'], hex(info['highfmt']),
        info['mipcount'], out))
    img.convert('RGB').save(out, 'JPEG', quality=90)
    return 0


if __name__ == '__main__':
    sys.exit(main())
