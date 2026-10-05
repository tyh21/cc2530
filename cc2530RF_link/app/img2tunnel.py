#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
img2tunnel.py —— 把图片预处理成 MN63Y1213 Tunnel 传图数据（PC 端）

输出：
  1. <name>.bin      : 2756 字节（212x104 或 104x212 的 1-bit 位流）
  2. <name>_chunks.txt : 每行一次 Tunnel WRITE 的 hex（地址 + 数据），供 Android App 直接读

用法：
  python img2tunnel.py 图片.png [--w 212] [--h 104] [--invert]

依赖：Pillow (pip install Pillow)
"""
import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("需要 Pillow: pip install Pillow")
    sys.exit(1)

MAX_WRITE_LEN = 248   # MN63Y1213 Tunnel 单次写最大长度


def load_and_binarize(path, w, h, invert, threshold=128):
    img = Image.open(path).convert("L")
    # 居中裁剪到目标宽高比，再缩放
    img = img.resize((w, h), Image.LANCZOS)
    px = img.load()
    bits = bytearray()
    for y in range(h):
        byte = 0
        cnt = 0
        for x in range(w):
            v = px[x, y]
            bit = 1 if v < threshold else 0   # 黑=1
            if invert:
                bit ^= 1
            byte = (byte << 1) | bit
            cnt += 1
            if cnt == 8:
                bits.append(byte)
                byte = 0
                cnt = 0
        if cnt:  # 宽度非 8 倍数时补齐
            bits.append(byte << (8 - cnt))
    return bytes(bits)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--w", type=int, default=212)
    ap.add_argument("--h", type=int, default=104)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--base-addr", type=lambda s: int(s, 0), default=0x0000,
                    help="Tunnel 写入的起始地址，默认 0")
    args = ap.parse_args()

    if not os.path.isfile(args.image):
        print("文件不存在:", args.image)
        sys.exit(1)

    data = load_and_binarize(args.image, args.w, args.h, args.invert)
    name = os.path.splitext(os.path.basename(args.image))[0]
    outdir = os.path.dirname(os.path.abspath(args.image))

    bin_path = os.path.join(outdir, name + ".bin")
    with open(bin_path, "wb") as f:
        f.write(data)
    print(f"[OK] 位图: {bin_path}  ({len(data)} 字节, {args.w}x{args.h})")

    chunks_path = os.path.join(outdir, name + "_chunks.txt")
    addr = args.base_addr
    n = 0
    with open(chunks_path, "w", encoding="utf-8") as f:
        f.write(f"# {name}.bin  {len(data)} 字节, 切成 Tunnel WRITE 分块
")
        f.write(f"# 每行: ADDR(hex) DATA(hex)
")
        off = 0
        while off < len(data):
            chunk = data[off:off + MAX_WRITE_LEN]
            a = addr + off
            f.write(f"{a:04X} {chunk.hex().upper()}
")
            off += len(chunk)
            n += 1
    print(f"[OK] 分块: {chunks_path}  ({n} 块)")


if __name__ == "__main__":
    main()
