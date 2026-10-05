#!/usr/bin/env python3
"""
精准反汇编 NFC 命令层与上层 batch 函数，确认：
1. write_cmd (0x7CC7) / read_cmd (0x7C10) 的确切入口参数传递
2. batch_read (0x7DAD) / batch_write (0x7E5C) / 0x7EB9 / 0x7ECD / 0x7EED 功能
3. 各函数内部对 stub 的调用关系
"""
import sys
sys.path.insert(0, '/Users/tianyaohui/Desktop/cc2530/.temp')
from dis8051_v2 import disasm

FW = '/Users/tianyaohui/Desktop/cc2530/.temp/fw_LG290_256k.bin'
data = open(FW, 'rb').read()

STUBS = {
    0x18A8:'set_slv', 0x18AE:'read_sda', 0x18B4:'write_byte',
    0x18BA:'START', 0x18C0:'STOP', 0x18C6:'read_cmd',
    0x18CC:'write_cmd', 0x18D2:'reset', 0x18D8:'power_on',
    0x18DE:'power_off', 0x18E4:'?7DAD', 0x18EA:'?7E5C',
    0x18F0:'?7EB9', 0x18F6:'?7ECD', 0x18FC:'?7EED',
    0x1902:'?7F27', 0x1590:'delay', 0x1DFA:'write_2byte',
}

LABELS = {
    0x7B48:'set_slv', 0x7B5F:'read_sda', 0x7B8F:'write_byte',
    0x7BCC:'START', 0x7BEE:'STOP',
    0x7C10:'read_cmd', 0x7CC7:'write_cmd',
    0x7D25:'reset', 0x7D30:'power_on', 0x7D6D:'power_off',
    0x7DE2:'init_slv',
    0x7DAD:'fn_7DAD(0x18E4)',
    0x7E5C:'fn_7E5C(0x18EA)',
    0x7EB9:'fn_7EB9(0x18F0)',
    0x7ECD:'fn_7ECD(0x18F6)',
    0x7EED:'fn_7EED(0x18FC)',
    0x7F27:'fn_7F27(0x1902)',
    0xFD1A:'write_2byte(0x1DFA)',
}

def disasm_range(start, end, title):
    print('='*90)
    print(title)
    print('='*90)
    for a, m, ln, raw in disasm(data, start, end):
        label = LABELS.get(a, '')
        note = ''
        # 识别 LCALL stub
        if m.startswith('LCALL'):
            parts = m.split()
            if len(parts) >= 2:
                try:
                    tgt = int(parts[1], 16)
                    if tgt in STUBS:
                        note = f'  ; -> {STUBS[tgt]}'
                    elif tgt in LABELS:
                        note = f'  ; -> {LABELS[tgt]}'
                except: pass
        # 识别 XRAM 0x1B61
        if '1B61' in m: note += '  ; ★从地址'
        if '#55' in m: note += '  ; ★slv=0x55'
        if '#AA' in m: note += '  ; ★写地址0xAA'
        if 'RLC' in m or 'RL A' in m: note += '  ; ★左移'
        pre = label if label else '  '
        print(f'{pre:<28} 0x{a:05X}: {raw:<16} {m}{note}')
    print()

# 1. write_cmd + read_cmd 详尽反汇编（重点看入口参数怎么取）
disasm_range(0x7C10, 0x7D80, '### read_cmd (0x7C10) + write_cmd (0x7CC7) 完整反汇编')

# 2. 五个 batch 函数
disasm_range(0x7DAD, 0x7E00, '### fn_7DAD (0x18E4)')
disasm_range(0x7E5C, 0x7EC0, '### fn_7E5C (0x18EA) batch_write?')
disasm_range(0x7EB9, 0x7ED0, '### fn_7EB9 (0x18F0)')
disasm_range(0x7ECD, 0x7EF0, '### fn_7ECD (0x18F6)')
disasm_range(0x7EED, 0x7F30, '### fn_7EED (0x18FC) verify?')
