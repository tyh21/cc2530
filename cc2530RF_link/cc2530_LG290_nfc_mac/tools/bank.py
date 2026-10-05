#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bank 感知的反汇编 + 数据 dump
用法:
  python bank.py dis <bank> <start_hex> <end_hex>    # 反汇编 bank 的 CODE 范围
  python bank.py dump <phys_hex> <len_hex>           # 物理偏移 hex dump
  python bank.py bankinfo                            # 各 bank 熵/内容概览
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()
BANK = 0x8000


def phys(code, bank):
    if code < 0x8000:
        return code
    return bank * BANK + (code - 0x8000)


def dis(bank, start, end):
    lo = phys(start, bank)
    hi = phys(end, bank)
    a = start
    p = lo
    while a < end:
        m, ln = D.disasm_one(img, p)
        raw = ' '.join('%02X' % img[p + k] for k in range(ln))
        print("  b%d:%04X (p%05X): %-14s %s" % (bank, a, p, raw, m))
        a += ln
        p += ln


def dump(p, n):
    for r in range(0, n, 16):
        ch = img[p + r:p + r + 16]
        hx = ' '.join('%02X' % b for b in ch)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ch)
        print("  %05X: %-47s  %s" % (p + r, hx, asc))


def bankinfo():
    for bk in range(8):
        seg = img[bk * BANK:(bk + 1) * BANK]
        nz = sum(1 for b in seg if b != 0xFF)
        ent = collections.Counter(seg)
        ff = ent.get(0xFF, 0)
        zero = ent.get(0x00, 0)
        print("  bank%d phys %05X-%05X: 非FF %6d (%.1f%%), 0xFF %6d, 0x00 %6d, 最多字节 %s"
              % (bk, bk * BANK, (bk + 1) * BANK - 1, nz, 100.0 * nz / BANK, ff, zero,
                 ['%02X:%d' % kv for kv in ent.most_common(3)]))


if __name__ == '__main__':
    if sys.argv[1] == 'dis':
        dis(int(sys.argv[2]), int(sys.argv[3], 16), int(sys.argv[4], 16))
    elif sys.argv[1] == 'dump':
        dump(int(sys.argv[2], 16), int(sys.argv[3], 16))
    elif sys.argv[1] == 'bankinfo':
        bankinfo()
