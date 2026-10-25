# -*- coding: utf-8 -*-
"""把 hostcheck 生成的 PBM (P4, 1=黑) 转成 PNG, 方便直接看。
用法: python tools/hostcheck/pbm2png.py <in.pbm> [out.png]
"""
import sys, zlib, struct

def read_pbm(path):
    with open(path, "rb") as f:
        data = f.read()
    # 解析 P4 header (支持 # 注释)
    pos = 0
    fields = []
    while len(fields) < 3:
        # skip whitespace/comments
        while data[pos:pos+1].isspace():
            pos += 1
        if data[pos:pos+1] == b'#':
            while data[pos:pos+1] not in (b'\n', b''):
                pos += 1
            continue
        start = pos
        while not data[pos:pos+1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1
    assert fields[0] == b'P4', fields[0]
    w, h = int(fields[1]), int(fields[2])
    raw = data[pos:]
    row_bytes = (w + 7) // 8
    px = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            bit = (raw[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1
            px[y * w + x] = 0 if bit else 255
    return w, h, px

def write_png(path, w, h, gray):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter: none
        raw += gray[y * w:(y + 1) * w]
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, "wb") as f:
        f.write(png)

if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
    w, h, px = read_pbm(src)
    # 放大 2x 便于看
    S = 2
    big = bytearray(w * S * h * S)
    for y in range(h * S):
        for x in range(w * S):
            big[y * w * S + x] = px[(y // S) * w + (x // S)]
    write_png(dst, w * S, h * S, big)
    print("wrote", dst, w, h)
