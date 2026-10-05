#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
原厂固件「物理连接层」提取器
 - 反汇编指定范围, 标注所有对端口/方向/功能/输入模式寄存器的写操作
 - 全镜像扫描, 找出所有配置 P0DIR/P0SEL/P0INP/P1DIR/... 的代码位置
用法:
  python pincfg.py dis <start_hex> <end_hex>   # 反汇编+端口标注 (物理偏移)
  python pincfg.py scan                        # 全镜像扫端口写点
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()

# ---- 直寻址 SFR 名 (CC2530 关键) ----
SFR = {
    0x80: 'P0', 0x87: 'PCON', 0x88: 'TCON', 0x89: 'P0IFG', 0x8A: 'P1IFG',
    0x8B: 'P2IFG', 0x8C: 'PICTL', 0x8D: 'P1IEN', 0x8F: 'P0INP',
    0x90: 'P1', 0x92: 'DPS', 0x98: 'SCON', 0x99: 'SBUF',
    0xA0: 'P2', 0xA8: 'IEN0', 0xA9: 'IEN2', 0xAB: 'WDCTL',
    0xB8: 'IEN1', 0xB9: 'P2IEN', 0xBA: 'ST0', 0xBB: 'ST1', 0xBC: 'ST2', 0xBD: 'ST3',
    0xBE: 'SLEEPCMD', 0xBF: 'SLEEPSTA',
    0xC6: 'CLKCONCMD', 0xC7: 'CLKCONSTA',
    0xE1: 'T1STAT', 0xE2: 'T1CNTL', 0xE3: 'T1CNTH', 0xE4: 'T1CTL',
    0xE5: 'T1CCTL0', 0xE6: 'T1CCTL1', 0xE7: 'T1CCTL2',
    0xF1: 'PERCFG', 0xF2: 'APCFG', 0xF3: 'P0SEL', 0xF4: 'P1SEL', 0xF5: 'P2SEL',
    0xF6: 'P1INP', 0xF7: 'P2INP', 0xFD: 'P0DIR', 0xFE: 'P1DIR', 0xFF: 'P2DIR',
}
# 端口相关寄存器 (用于 scan 时判定"是物理连接配置")
PORTREGS = {0x80, 0x8F, 0x89, 0x8A, 0x8B, 0x8C, 0x90, 0xA0,
            0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xFD, 0xFE, 0xFF}

BITBASE = {0x80: 'P0', 0x90: 'P1', 0xA0: 'P2'}


def bitname(b):
    for base, nm in BITBASE.items():
        if base <= b <= base + 7:
            return '%s.%d' % (nm, b - base)
    return None


def annotate(p):
    """返回 (text_len, note): 若该处是端口写操作, 给出注解"""
    op = img[p]
    # MOV direct, A : 0xF5 da
    if op == 0xF5 and p + 2 <= len(img):
        d = img[p + 1]
        if d in SFR:
            return 3, '<=> %s = A' % SFR[d]
    # MOV direct, #imm : 0x75 da imm
    if op == 0x75 and p + 3 <= len(img):
        d, v = img[p + 1], img[p + 2]
        if d in SFR:
            return 4, '<=> %s = #%02X' % (SFR[d], v)
    # MOV A, direct : 0xE5 da
    if op == 0xE5 and p + 2 <= len(img):
        d = img[p + 1]
        if d in SFR:
            return 3, '(read) A <= %s' % SFR[d]
    # ANL direct, A : 0x52 da
    if op == 0x52 and p + 2 <= len(img):
        d = img[p + 1]
        if d in SFR:
            return 3, '<=> %s &= A' % SFR[d]
    # ANL direct, #imm : 0x53 da imm
    if op == 0x53 and p + 3 <= len(img):
        d, v = img[p + 1], img[p + 2]
        if d in SFR:
            return 4, '<=> %s &= #%02X' % (SFR[d], v)
    # ORL direct, A : 0x42 da
    if op == 0x42 and p + 2 <= len(img):
        d = img[p + 1]
        if d in SFR:
            return 3, '<=> %s |= A' % SFR[d]
    # ORL direct, #imm : 0x43 da imm
    if op == 0x43 and p + 3 <= len(img):
        d, v = img[p + 1], img[p + 2]
        if d in SFR:
            return 4, '<=> %s |= #%02X' % (SFR[d], v)
    # XRL direct, #imm : 0x63 da imm
    if op == 0x63 and p + 3 <= len(img):
        d, v = img[p + 1], img[p + 2]
        if d in SFR:
            return 4, '<=> %s ^= #%02X' % (SFR[d], v)
    # SETB bit : 0xD2 b
    if op == 0xD2 and p + 2 <= len(img):
        nm = bitname(img[p + 1])
        if nm:
            return 3, '<=> SETB %s' % nm
    # CLR bit : 0xC2 b
    if op == 0xC2 and p + 2 <= len(img):
        nm = bitname(img[p + 1])
        if nm:
            return 3, '<=> CLR %s' % nm
    # MOV C,bit / MOV bit,C : 0xA2/0x92 b
    if op in (0xA2, 0x92) and p + 2 <= len(img):
        nm = bitname(img[p + 1])
        if nm:
            return 3, ('<=> C <- %s' % nm) if op == 0xA2 else ('<=> %s <- C' % nm)
    return None


def dis(start, end):
    p = start
    while p < end:
        m, ln = D.disasm_one(img, p)
        raw = ' '.join('%02X' % img[p + k] for k in range(ln))
        note = ''
        # 先试端口注解 (可能比指令更短/更长, 但只在指令边界安全)
        a = annotate(p)
        if a and a[0] == ln:
            note = '   ; ' + a[1]
        print("  p%05X: %-14s %s%s" % (p, raw, m, note))
        p += ln
        if ln <= 0:
            p += 1


def scan():
    """全镜像扫描端口写点(独立于指令边界), 输出候选位置"""
    hits = []
    p = 0
    N = len(img)
    while p < N:
        op = img[p]
        d = None
        if op in (0xF5, 0x52, 0x42, 0xE5) and p + 1 < N:
            d = img[p + 1]
        elif op in (0x75, 0x53, 0x43, 0x63) and p + 1 < N:
            d = img[p + 1]
        if d in PORTREGS:
            hits.append(p)
        p += 1
    print("端口写点候选 %d 个:" % len(hits))
    for h in hits:
        # 打印该点后 8 字节
        seg = img[h:h + 6]
        print("  p%05X (bank%d off %04X): %s" % (h, h // 0x8000, h % 0x8000,
                                                 ' '.join('%02X' % b for b in seg)))
    return hits


if __name__ == '__main__':
    if sys.argv[1] == 'dis':
        dis(int(sys.argv[2], 16), int(sys.argv[3], 16))
    elif sys.argv[1] == 'scan':
        scan()
