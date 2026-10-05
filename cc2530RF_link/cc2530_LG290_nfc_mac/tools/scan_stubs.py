#!/usr/bin/env python3
"""扫描固件中的 0x11F0 跳板 stub，提取 目标地址 + FMAP bank
stub 结构: 12 F1 11 (LCALL 0x11F0) + b0 + b1 + b2
其中 b0|b1<<8 = 目标偏移, b2 = FMAP bank
入口+1 = MOV R5,A 参数口: 7D xx 12 F1 11 ...
"""
import sys

FW = '/Users/tianyaohui/Desktop/cc2530/.temp/fw_LG290_256k.bin'
data = open(FW, 'rb').read()

stubs = []
for i in range(len(data) - 6):
    if data[i] == 0x12 and data[i+1] == 0xF1 and data[i+2] == 0x11:  # LCALL 0x11F1?
        pass
    if data[i] == 0x12 and data[i+1] == 0xF1 and data[i+2] == 0x11:
        pass

# 修正: LCALL 0x11F0 = 12 11 F0 (小端: 高字节在前)
# 8051 LCALL 编码: opcode | addr_hi | addr_lo
hits = []
for i in range(0, min(len(data), 0x20000) - 6):
    if data[i] == 0x12 and data[i+1] == 0x11 and data[i+2] == 0xF0:
        b0, b1, b2 = data[i+3], data[i+4], data[i+5]
        hits.append((i, b0 | (b1 << 8), b2))

print(f'共 {len(hits)} 个 LCALL 0x11F0 stub:')
for addr, target, bank in hits:
    print(f'  stub@0x{addr:05X} -> 目标 0x{target:04X}, FMAP={bank}')

# 统计 bank 分布
from collections import Counter
c = Counter(b for _, _, b in hits)
print('FMAP 分布:', dict(c))
