#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描原厂固件里所有对 P1.0 / P1 的操作，判断 NIRQ 线的用法：
   - 是否被当输出驱动(SETB/CLR/MOV P1)  -> 软件是否拉高拉低 P1.0
   - 是否被当输入读(JB/JNB/MOV C/MOV A P1) -> 软件是否依赖 NIRQ 电平
   - 是否存在"等 NIRQ"的等待循环(写流程关键)
   同时定位主循环对 nfc_power_on 的调用点。
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()
N = len(img)  # 262144 = 8 x 32KB

def code_addr(off):
    bank = off // 0x8000
    return (bank << 15) | (off & 0x7FFF)

# 解 thunk 表 (root bank 0x143A..0x2000)
thunks = {}
a = 0x143A
while a < 0x2000 - 6:
    if img[a] == 0x12 and img[a+1] == 0x11 and img[a+2] == 0xF0:
        thunks[a] = ((img[a+4] << 8) | img[a+3], img[a+5])
        a += 6
    else:
        a += 1

def dis_ctx(off, before=0, after=10):
    """在 file offset 处反汇编 after 条，返回文本。"""
    out = []
    # 往前找一条起点近似(简单: 从 off 往前回溯到对齐较难, 只反汇编 after 条从 off 起)
    code = code_addr(off)
    for ad, m, ln, raw in D.disasm(img, off, off + 64):
        out.append("    %04X: %-14s %s" % (code_addr(ad), raw, m))
        if len(out) >= after:
            break
    return "\n".join(out)

# 目标 opcode 模式 (针对 P1.0 = bit 0x90, P1 = SFR 0x90)
PATTERNS = {
    b'\x20\x90': 'JB  P1.0',
    b'\x30\x90': 'JNB P1.0',
    b'\xa2\x90': 'MOV C,P1.0',
    b'\x92\x90': 'MOV P1.0,C',
    b'\xd2\x90': 'SETB P1.0',
    b'\xc2\x90': 'CLR  P1.0',
    b'\xb2\x90': 'CPL  P1.0',
    b'\xe5\x90': 'MOV A,P1',
    b'\xf5\x90': 'MOV P1,A',
    b'\x53\x90': 'ANL P1,#',
    b'\x43\x90': 'ORL P1,#',
    b'\x63\x90': 'XRL P1,#',
}

print("="*78)
print("一、P1.0 / P1 操作扫描 (file offset -> code addr)")
print("="*78)
hits = []
for off in range(N - 2):
    pair = img[off:off+2]
    if pair in PATTERNS:
        # 还要确认不是数据巧合: 对 0xE5/0xF5/0x53/0x43/0x63 多验证第三字节
        b2 = img[off+2] if off+2 < N else 0
        three = img[off:off+3]
        name = PATTERNS[pair]
        if pair in (b'\xe5\x90', b'\xf5\x90', b'\x53\x90', b'\x43\x90', b'\x63\x90'):
            # 这些也可能碰巧出现在数据; 记录即可, 后面人工看
            pass
        hits.append((off, code_addr(off), name, b2))
# 去重相邻(同一指令多命中不会, 但保险)
for off, ca, name, b2 in hits:
    extra = ""
    if name in ('ANL P1,#','ORL P1,#','XRL P1,#'):
        extra = "  (imm=0x%02X)" % b2
    print("  off=0x%05X code=0x%04X  %s%s" % (off, ca, name, extra))

print()
print("="*78)
print("二、对 P1.0 等待/读取的上下文反汇编 (关键: 是否\"等 NIRQ\")")
print("="*78)
for off, ca, name, b2 in hits:
    if name in ('JB P1.0','JNB P1.0','MOV C,P1.0','MOV A,P1'):
        print("\n--- 命中 %s @ code 0x%04X (off 0x%05X) ---" % (name, ca, off))
        print(dis_ctx(off, after=12))

print()
print("="*78)
print("三、P1.0 方向/功能配置 (P1DIR / P1SEL 对 bit0 的操作)")
print("="*78)
# 搜 P1DIR(0xFE) / P1SEL(0xF4) 对 0x01 的 ORL/ANL
for off in range(N - 3):
    if img[off] in (0x43, 0x53) and img[off+1] == 0xFE and img[off+2] == 0x01:
        print("  off=0x%05X code=0x%04X  ORL/ANL P1DIR,#01  (%s)" % (
            off, code_addr(off), 'ORL' if img[off]==0x43 else 'ANL'))
    if img[off] in (0x43, 0x53) and img[off+1] == 0xF4 and img[off+2] == 0x01:
        print("  off=0x%05X code=0x%04X  ORL/ANL P1SEL,#01  (%s)" % (
            off, code_addr(off), 'ORL' if img[off]==0x43 else 'ANL'))

print()
print("="*78)
print("四、主循环调用 nfc_power_on (0x7D30) 的位置")
print("="*78)
# LCALL 0x7D30 直接
for off in range(N - 3):
    if img[off] == 0x12 and img[off+1] == 0x7D and img[off+2] == 0x30:
        print("  off=0x%05X code=0x%04X  LCALL nfc_power_on" % (off, code_addr(off)))
# 经 thunk: 找指向 0x7D30 的 thunk 条目, 再找调用该 thunk 的位置
target_thunks = [t for t,(tg,bk) in thunks.items() if tg == 0x7D30]
print("  -> thunk entries pointing to 0x7D30:", [hex(t) for t in target_thunks])
for off in range(N - 3):
    for t in target_thunks:
        if img[off] == 0x12 and img[off+1] == (t >> 8) & 0xFF and img[off+2] == t & 0xFF:
            print("  off=0x%05X code=0x%04X  LCALL thunk->nfc_power_on" % (off, code_addr(off)))
