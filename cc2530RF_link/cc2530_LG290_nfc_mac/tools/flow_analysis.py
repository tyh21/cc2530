#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
原厂固件 fw_LG290_256k.bin 主流程分析 (CC2530F256, 8 x 32KB bank)

目标: 回答用户问题 ——
  "原厂程序是不是只 '读数据模块 -> 写 EPD' ? 到底有没有 NFC 读写流程? 数据模块是什么?"

步骤:
  1. 解跨 bank thunk 表 (root bank CODE 0x143A 起, 每条 6 字节 12 11 F0 LO HI FMAP)
  2. 全镜像 (4 个 64KB 区段/8 bank) 扫描谁调用了 NFC thunk
  3. 完整线性反汇编 root bank NFC 应用层 0x7200-0x7B60
  4. 定位 EPD 发送函数 (位操作直方图 + 特征码)
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis8051_v2 as D

IMG = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG213\lg290_flat.bin"
img = open(IMG, "rb").read()
print("# image %d bytes = %d banks(32KB)" % (len(img), len(img)//0x8000))

BANK = 0x8000

def mem_at(code_addr, bank):
    """CODE 地址 -> 物理偏移 (bank0: 原样; bank N: N*0x8000 + (addr-0x8000))"""
    if code_addr < 0x8000:
        return code_addr
    return bank * BANK + (code_addr - 0x8000)

def rd(a, bank=0):
    p = mem_at(a, bank)
    return img[p] if 0 <= p < len(img) else 0xFF

# ---------- 1. thunk 表 ----------
print("\n" + "=" * 78)
print("1) 跨 bank thunk 表 (root bank)")
print("=" * 78)
thunks = {}
a = 0x143A
while a < 0x2000 - 6:
    if rd(a) == 0x12 and rd(a+1) == 0x11 and rd(a+2) == 0xF0:
        lo, hi, bank = rd(a+3), rd(a+4), rd(a+5)
        thunks[a] = ((hi << 8) | lo, bank)
        a += 6
    else:
        a += 1
print("共解出 %d 条 thunk" % len(thunks))

NFC_THUNKS = {
    0x18A8: 'set_slv      -> 0x7B48',
    0x18AE: 'read_sda     -> 0x7B5F',
    0x18B4: 'write_byte   -> 0x7B8F',
    0x18BA: 'START(=STOP) -> 0x7BCC',
    0x18C0: 'STOP(=START) -> 0x7BEE',
    0x18C6: 'read_cmd     -> 0x7C10',
    0x18CC: 'write_cmd    -> 0x7CC7',
    0x18D2: 'reset        -> 0x7D25',
    0x18D8: 'power_on     -> 0x7D30',
    0x18DE: 'power_off    -> 0x7D6D',
    0x18E4: 'batch_read   -> 0x7DAD',
    0x18EA: 'batch_write  -> 0x7E5C',
    0x18F0: 'fn_7EB9',
    0x18F6: 'fn_7ECD',
    0x18FC: 'fn_7EED',
    0x1902: 'fn_7F27',
}
for t, nm in NFC_THUNKS.items():
    tgt, bk = thunks.get(t, (None, None))
    print("  thunk 0x%04X  %-22s 实际目标 0x%04X bank=%s" % (t, nm, tgt or 0, bk))

# ---------- 2. 全镜像扫描调用点 ----------
print("\n" + "=" * 78)
print("2) 谁调用了 NFC thunk (全 256KB 扫描, LCALL/ACALL/跳转都算)")
print("=" * 78)
# 线性扫机器码 12 hi lo (LCALL) 和 11/31/... (ACALL) 指向 thunk 地址
hits = collections.defaultdict(list)
for i in range(len(img) - 3):
    op = img[i]
    if op == 0x12:
        t = (img[i+1] << 8) | img[i+2]
        if t in NFC_THUNKS:
            hits[t].append(i)
    elif (op & 0x1F) == 0x11:            # ACALL page
        t = (((op & 0xE0) >> 5) << 8) | img[i+1]
        t |= (i + 2) & 0xF800
        if t in NFC_THUNKS:
            hits[t].append(i)

def where(off):
    """物理偏移 -> 人话 (哪个 64KB'区段'/哪段 CODE)"""
    seg = off // 0x10000
    if off < BANK:
        return "root(CODE %04X)" % off
    bank = off // BANK
    return "bank%d(CODE %04X)" % (bank, 0x8000 + (off - bank*BANK))

for t, nm in NFC_THUNKS.items():
    ls = hits.get(t, [])
    if not ls:
        continue
    print("\n  [%s]  被调 %d 处:" % (nm, len(ls)))
    for off in ls:
        print("      %s  @phys 0x%05X" % (where(off), off))

# ---------- 3. NFC 应用层反汇编 ----------
def dis_range(start, end, title, bank=0):
    print("\n" + "=" * 78)
    print(title)
    print("=" * 78)
    for ad, m, ln, raw in D.disasm(img, start, end):
        note = ''
        if m.startswith('LCALL') or m.startswith('LJMP'):
            try:
                tg = int(m.split()[-1], 16)
                if tg in NFC_THUNKS:
                    note = '   ; >>> ' + NFC_THUNKS[tg].split('->')[0].strip()
            except Exception:
                pass
        if '0x18CC' in m: note = '   ; >>> write_cmd'
        if '0x1B61' in m: note += '   ; ★从地址存储'
        print("  %04X: %-16s %s%s" % (ad, raw, m, note))

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'flow':
        # NFC 应用层主流程
        dis_range(0x7247, 0x7360, '### NFC 应用层 A: 探测/初始化 (0x7247-0x7360)')
        dis_range(0x7360, 0x73E0, '### NFC 应用层 B: 写 CC/格式化 (0x7360-0x73E0)')
        dis_range(0x7860, 0x7920, '### NFC 应用层 C: read 调用点 (0x7860-0x7920)')
        dis_range(0x7A20, 0x7B60, '### NFC 应用层 D: 主分发 (0x7A20-0x7B60)')
