#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
带 thunk 解析的原厂固件反汇编 + 数据表 dump
用法:
  python flow2.py <start_hex> <end_hex>     # 反汇编范围内代码, 自动解析 thunk
  python flow2.py dump <start_hex> <len>    # hex dump 数据
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()
BANK = 0x8000

# --- 解 thunk 表 ---
thunks = {}
a = 0x143A
while a < 0x2000 - 6:
    if img[a] == 0x12 and img[a+1] == 0x11 and img[a+2] == 0xF0:
        thunks[a] = ((img[a+4] << 8) | img[a+3], img[a+5])
        a += 6
    else:
        a += 1

# --- 已知函数名 ---
NAMES = {
    0x11F0: 'TRAMPOLINE', 0x0E24: 'TODO_0E24', 0x1188: 'memcpy_c2x',
    0x1419: 'ld4(R2R3R4R5=*DPTR)', 0x1430: 'ptr(base+A)',
    0x1220: 'ptr_add(A)', 0x1282: 'ptr_sub(A)', 0x126F: 'fn_126F',
    0x7B48: 'i2c_set_slv', 0x7B5F: 'i2c_read_sda', 0x7B8F: 'i2c_write_byte',
    0x7BCC: 'i2c_seq_A', 0x7BEE: 'i2c_seq_B', 0x7C10: 'i2c_read_cmd',
    0x7CC7: 'i2c_write_cmd', 0x7D25: 'nfc_reset', 0x7D30: 'nfc_power_on',
    0x7D6D: 'nfc_power_off', 0x7DAD: 'nfc_batch_read', 0x7E5C: 'nfc_batch_write',
    0x7EB9: 'fn_7EB9', 0x7ECD: 'fn_7ECD', 0x7EED: 'fn_7EED_16B', 0x7F27: 'fn_7F27',
}

def nm(t):
    if t in thunks:
        tgt, bk = thunks[t]
        base = NAMES.get(tgt, 'sub_%04X' % tgt)
        if bk:
            base += '@bank%d' % bk
        return 'thunk->' + base
    return NAMES.get(t, '')


def dis(start, end):
    for ad, m, ln, raw in D.disasm(img, start, end):
        note = ''
        if m.startswith(('LCALL', 'LJMP', 'AJMP', 'ACALL')):
            try:
                tg = int(m.split()[-1], 16)
                n = nm(tg)
                if n:
                    note = '   ; ' + n
            except Exception:
                pass
        print("  %04X: %-16s %s%s" % (ad, raw, m, note))


def dump(start, n):
    for r in range(0, n, 16):
        chunk = img[start + r: start + r + 16]
        hx = ' '.join('%02X' % b for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print("  %04X: %-47s  %s" % (start + r, hx, asc))


if __name__ == '__main__':
    if sys.argv[1] == 'dump':
        dump(int(sys.argv[2], 16), int(sys.argv[3], 16))
    else:
        dis(int(sys.argv[1], 16), int(sys.argv[2], 16))
