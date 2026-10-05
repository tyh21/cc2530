#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
综合追踪: 原厂固件 "数据模块 / NFC / EPD" 三者关系
- 解 thunk 表 (0x143A 起, 487 项)
- 列出指向 NFC (0x7Dxx/0x7Exx) 和 bank1 EPD (F6xx-FBxx) 的 thunk 存根
- 全 256KB 扫描 LCALL/ACALL 到这些存根的调用点
- dump 数据模块区域
- 扫描 MOV DPTR,#88xx / MOVX / MOVC, 判断 0x88xx 是 XDATA 还是 CODE
用法: python analyze_flow.py <cmd>
    thunks     列出全部 thunk 存根(带目标)
    nfc        只列 NFC 相关 thunk + 调用点
    epd        只列 bank1 EPD 相关 thunk + 调用点
    dump        dump 数据模块
    ref88      搜索 88xx 引用
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()
BANK = 0x8000
N = len(img)

# ---- thunk 表 ----
thunks = {}          # stub_addr -> (target_code_addr, bank)
a = 0x143A
while a < 0x2000 - 6:
    if img[a] == 0x12 and img[a+1] == 0x11 and img[a+2] == 0xF0:
        thunks[a] = ((img[a+4] << 8) | img[a+3], img[a+5])
        a += 6
    else:
        a += 1

def nm(t):
    tgt, bk = thunks[t]
    return "->%04X@b%d" % (tgt, bk)

def scan_calls(stub_list):
    """全图扫 LCALL(12 hi lo) / ACALL / LJMP(02 hi lo) 指向 stub"""
    S = set(stub_list)
    hits = []
    for p in range(0, N - 3):
        if 0x143A <= p < 0x1FA4:      # 排除 thunk 表本体, 免误判
            continue
        op = img[p]
        if op == 0x12 or op == 0x02:      # LCALL / LJMP 3 字节: 12 ah al
            tg = (img[p+1] << 8) | img[p+2]
            if tg in S:
                hits.append((p, op, tg))
        elif (op & 0xF1) == 0x11 or (op & 0xF1) == 0x01:   # ACALL / AJMP 2 字节
            pg = (p + 2) & 0xF800
            tg = pg | (((op >> 5) & 0x07) << 8) | img[p+1]
            if tg in S:
                hits.append((p, op, tg))
    return hits

def phys_to_bank(phys):
    return phys // BANK, phys % BANK if phys < 0x8000 else 0x8000 + (phys - BANK*(phys//BANK))

def desc_phys(p):
    bk = p // BANK
    off = p % BANK
    code = off if bk == 0 else 0x8000 + off
    return "b%d:%04X(p%05X)" % (bk, code, p)

cmd = sys.argv[1] if len(sys.argv) > 1 else "thunks"

if cmd == "thunks":
    print("共 %d 个 thunk 存根:" % len(thunks))
    for s in sorted(thunks):
        print("  stub %04X %s" % (s, nm(s)))

elif cmd == "nfc":
    stubs = [s for s, (t, bk) in thunks.items() if 0x7B00 <= t <= 0x7FFF]
    print("NFC 相关 thunk 存根:")
    for s in sorted(stubs):
        print("  stub %04X %s" % (s, nm(s)))
    hits = scan_calls(stubs)
    print("\n调用点 (%d):" % len(hits))
    for p, op, tg in hits:
        print("  %s  %s stub%04X" % (desc_phys(p), "LCALL" if op == 0x12 else
              ("LJMP" if op == 0x02 else ("ACALL" if (op & 0xF1) == 0x11 else "AJMP")), tg))

elif cmd == "epd":
    # bank1 目标: 0xF600-0xFB00 ; 也含 root 0x7xxx? 这里只 bank1
    stubs = [s for s, (t, bk) in thunks.items() if bk == 1 and 0xF600 <= t <= 0xFB00]
    print("bank1 EPD(F600-FB00) 相关 thunk 存根:")
    for s in sorted(stubs):
        print("  stub %04X %s" % (s, nm(s)))
    hits = scan_calls(stubs)
    print("\n调用点 (%d):" % len(hits))
    for p, op, tg in hits:
        print("  %s  -> stub%04X %s" % (desc_phys(p), tg, nm(tg)))

elif cmd == "dump":
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x8700
    n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x300
    for r in range(0, n, 16):
        ch = img[start + r: start + r + 16]
        hx = ' '.join('%02X' % b for b in ch)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in ch)
        print("  %05X: %-47s  %s" % (start + r, hx, asc))

elif cmd == "ctx":
    # python analyze_flow.py ctx <stub_hex> [before] [after]
    stub = int(sys.argv[2], 16)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    hits = scan_calls([stub])
    print("stub %04X %s 调用点 %d 个:" % (stub, nm(stub), len(hits)))
    for p, op, tg in hits:
        lo = max(0, p - before)
        hi = min(N, p + after + 3)
        print("--- 调用点 %s ---" % desc_phys(p))
        for ad, m, ln, raw in D.disasm(img, lo, hi):
            mark = " <== CALL" if ad == p else ""
            print("    %s: %-14s %s%s" % (desc_phys(ad), raw, m, mark))

elif cmd == "ref88":
    print("MOV DPTR,#88xx/89xx/8Axx/8Bxx 引用:")
    cnt = 0
    for p in range(0, N - 3):
        if img[p] == 0x90 and img[p+1] in (0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F):
            cnt += 1
            if cnt <= 80:
                print("  %s DPTR=#%02X%02X" % (desc_phys(p), img[p+1], img[p+2]))
    print("  ... 共 %d 处" % cnt)
else:
    print("unknown cmd")
