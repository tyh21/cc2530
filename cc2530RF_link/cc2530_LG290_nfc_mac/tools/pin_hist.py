#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
在位空间 SFR 里做位操作直方图 -> 找活跃引脚 / 找 EPD SPI 发送函数
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()

def hist(lo, hi, label):
    """统计 CLR bit(C2)/SETB bit(D2)/MOV C,bit(A2)/MOV bit,C(92) 的操作数分布"""
    c = collections.Counter()
    n = 0
    a = lo
    while a < hi:
        m, ln = D.disasm_one(img, a)
        op = img[a]
        if op in (0xC2, 0xD2, 0xA2, 0x92):
            c[img[a+1]] += 1
            n += 1
        a += ln
    print("\n===== %s  (0x%04X-0x%04X, 位操作 %d 条) =====" % (label, lo, hi, n))
    for op, cnt in c.most_common(18):
        nm = D.bitname(op)
        sys.stdout.write("  %-10s %4d   " % (nm, cnt))
    print()

if __name__ == '__main__':
    # root bank 全域
    hist(0x0000, 0x8000, "root bank 位操作直方图")
