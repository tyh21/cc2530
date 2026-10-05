#!/usr/bin/env python3
"""
完整反汇编 + 注释原厂 NFC I2C 物理层与命令层 0x7B00-0x7D80。
逐条指令输出，标注每个函数入口、I2C 原语、命令层流程。
"""
import sys
sys.path.insert(0, '/Users/tianyaohui/Desktop/cc2530/.temp')
from dis8051_v2 import disasm

FW = '/Users/tianyaohui/Desktop/cc2530/.temp/fw_LG290_256k.bin'
data = open(FW, 'rb').read()

# 函数入口标注
LABELS = {
    0x7B00: '◆ 子函数A: 检查命令码 0x42 分支',
    0x7B48: '◆ set_slv: 写 XRAM 0x1B61 (设 7 位从地址)',
    0x7B5F: '◆ read_sda: 读 SDA 单 bit/字节采样',
    0x7B8F: '◆ write_byte: 写 8 位字节 (MSB 先行)',
    0x7BCC: '◆ START: I2C START 时序',
    0x7BEE: '◆ STOP: I2C STOP 时序',
    0x7C10: '◆◆ read_cmd: 读命令层 (发读地址→读N字节)',
    0x7C3E: '◆ read_loop_body: 读循环体',
    0x7C7F: '◆ read_done: 读完成 STOP',
    0x7CBD: '◆ sda_input: SDA 切输入 (ANL P0DIR,#FE)',
    0x7CC7: '◆◆ write_cmd: 写命令层 (发地址→发命令→发数据)',
    0x7D14: '◆ write_err: 写错误返回',
    0x7D1B: '◆ write_done: 写完成 STOP',
    0x7D25: '◆ init/reset: 复位序列 (VDDEX 脉冲)',
    0x7D30: '◆ power_on: 上电/VDDEX 配置',
    0x7D6D: '◆ power_off: 下电/释放',
    0x7DE2: '◆ init_slv: 初始化从地址为 0x55',
    0x7E5C: '◆◆ batch_write: 批量写 FeRAM',
    0x7ECD: '◆ batch_read: 批量读 FeRAM',
    0x7EED: '◆ verify_cmd: 校验命令',
}

# I2C 关键指令注释
def annotate(a, m, raw):
    notes = []
    # 引脚操作
    if 'P0.0' in m or 'P0.1' in m: notes.append('SDA/SCL')
    if 'P2.3' in m: notes.append('VDDEX')
    if 'P0DIR' in m: notes.append('方向')
    if '#1B61' in m: notes.append('★从地址存储')
    # delay
    if '0x1590' in m or 'LCALL 0x1590' in m: notes.append('delay')
    # stub 调用
    if 'LCALL 0x18' in m:
        stub = int(m.split('0x')[1].split()[0], 16) if '0x' in m else 0
        stubs = {0x18A8:'set_slv',0x18AE:'read_sda',0x18B4:'write_byte',
                 0x18BA:'START',0x18C0:'STOP',0x18C6:'read_cmd',0x18CC:'write_cmd',
                 0x18D2:'reset',0x18D8:'power_on',0x18DE:'power_off',0x18E4:'read_status'}
        if stub in stubs: notes.append(f'→{stubs[stub]}')
    # 立即数
    if '#55' in m: notes.append('★slv=0x55')
    if '#AA' in m: notes.append('8位写地址')
    if '#AB' in m: notes.append('8位读地址')
    if '#01' in m and 'ORL' in m: notes.append('读位')
    note = ' '.join(notes)
    return note

print('=' * 80)
print('原厂 NFC I2C 物理层 + 命令层 完整反汇编 (0x7B00-0x7D80)')
print('=' * 80)
start = 0x7B00
end = 0x7D80
for a, m, ln, raw in disasm(data, start, end):
    label = LABELS.get(a, '')
    note = annotate(a, m, raw)
    prefix = label if label else '  '
    suffix = f'  ; {note}' if note else ''
    print(f'{prefix:<45} 0x{a:05X}: {raw:<18} {m}{suffix}')
