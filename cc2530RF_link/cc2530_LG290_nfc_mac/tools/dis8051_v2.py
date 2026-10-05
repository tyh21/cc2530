#!/usr/bin/env python3
"""CC2530 (8051) firmware disassembler v2 - 完整 256 操作码表驱动实现

修复 dis8051.py 的全部已知 bug:
  1. `elif 0x22:` 恒真，吞掉之后 ~60 个指令分支
  2. AJMP/ACALL 编码公式错误
  3. 0xB5 被按 4 字节处理
  4. SFR 表错误 (P0DIR=0xFD 非 0xF5; 0xF5=P2SEL)
SFR 名与地址取自 IAR ioCC2530.h 权威定义。
用法: dis8051_v2.py <fw.bin> <start_hex> <end_hex>
"""
import sys

# ---- CC2530 SFR (来自 IAR ioCC2530.h) ----
SFR = {
    0x80:'P0', 0x81:'SP', 0x82:'DPL0', 0x83:'DPH0', 0x84:'DPL1', 0x85:'DPH1',
    0x86:'U0CSR', 0x87:'PCON', 0x88:'TCON', 0x89:'P0IFG', 0x8A:'P1IFG', 0x8B:'P2IFG',
    0x8C:'PICTL', 0x8D:'P1IEN', 0x8F:'P0INP',
    0x90:'P1', 0x91:'RFIRQF1', 0x92:'DPS', 0x93:'MPAGE', 0x94:'T2CTRL',
    0x95:'ST0', 0x96:'ST1', 0x97:'ST2', 0x9A:'IEN2', 0x9B:'S1CON',
    0x9C:'T2CSPCFG', 0x9D:'SLEEPSTA', 0x9E:'CLKCONSTA', 0x9F:'FMAP',
    0xA0:'P2', 0xA1:'T2IRQF', 0xA2:'T2M0', 0xA3:'T2M1',
    0xA4:'T2MOVF0', 0xA5:'T2MOVF1', 0xA6:'T2MOVF2', 0xA7:'T2IRQM',
    0xA8:'IEN0', 0xA9:'IP0', 0xAB:'P0IEN', 0xAC:'P2IEN',
    0xAD:'STLOAD', 0xAE:'PMUX', 0xAF:'T1STAT', 0xB0:'P3',
    0xB4:'ADCCON1', 0xB5:'ADCCON2', 0xB6:'ADCCON3',
    0xB9:'IP1', 0xBA:'ADCL', 0xBB:'ADCH',
    0xBC:'RNDL', 0xBD:'RNDH', 0xBE:'SLEEPCMD', 0xBF:'RFERRF',
    0xC0:'IRCON', 0xC1:'U0DBUF', 0xC2:'U0BAUD', 0xC3:'T2MSEL', 0xC4:'U0UCR',
    0xC5:'U0GCR', 0xC6:'CLKCONCMD', 0xC7:'MEMCTR', 0xC9:'WDCTL',
    0xCA:'T3CNT', 0xCB:'T3CTL', 0xCC:'T3CCTL0', 0xCD:'T3CC0', 0xCE:'T3CCTL1',
    0xCF:'T3CC1', 0xD0:'PSW', 0xD1:'DMAIRQ', 0xD2:'DMA1CFGL', 0xD3:'DMA1CFGH',
    0xD4:'DMA0CFGL', 0xD5:'DMA0CFGH', 0xD6:'DMAARM', 0xD7:'DMAREQ',
    0xD8:'SFRD8', 0xD9:'RFD', 0xDA:'T1CC0L', 0xDB:'T1CC0H', 0xDC:'T1CC1L',
    0xDD:'T1CC1H', 0xDE:'T1CC2L', 0xDF:'T1CC2H',
    0xE0:'ACC', 0xE1:'RFST', 0xE2:'T1CNTL', 0xE3:'T1CNTH', 0xE4:'T1CTL',
    0xE5:'T1CCTL0', 0xE6:'T1CCTL1', 0xE7:'T1CCTL2',
    0xE8:'IRCON2', 0xE9:'RFIRQF0', 0xEA:'T4CNT', 0xEB:'T4CTL', 0xEC:'T4CCTL0',
    0xED:'T4CC0', 0xEE:'T4CCTL1', 0xEF:'T4CC1',
    0xF0:'B', 0xF1:'PERCFG', 0xF2:'ADCCFG', 0xF3:'P0SEL', 0xF4:'P1SEL',
    0xF5:'P2SEL', 0xF6:'P1INP', 0xF7:'P2INP', 0xF9:'U1DBUF',
    0xFA:'U1BAUD', 0xFB:'U1UCR', 0xFC:'U1GCR', 0xFD:'P0DIR', 0xFE:'P1DIR', 0xFF:'P2DIR',
}

# 位空间 SFR 基址 -> 名称 (位地址 base..base+7 = 寄存器位 7..0)
BITBASE = {
    0x80:'P0', 0x88:'TCON', 0x90:'P1', 0x98:'SCON', 0xA0:'P2', 0xA8:'IEN0',
    0xB0:'P3', 0xB8:'IENC', 0xC0:'IRCON', 0xC8:'T3CC0B', 0xD0:'PSW',
    0xD8:'SFRD8B', 0xE0:'ACC', 0xE8:'IRCON2B', 0xF0:'B',
}

def sfrname(b):
    return SFR.get(b, f'dir{b:02X}')

def bitname(b):
    if b < 0x80:
        return f'{0x20 + (b >> 3):02X}.{b & 7}'      # IRAM 0x20-0x2F 的位
    base = b & 0xF8
    name = BITBASE.get(base)
    if name:
        return f'{name}.{b - base}'        # 位地址 = 基址+位号 (P2.3=0xA3)
    return f'bit{b:02X}'

def rel(addr, off):
    if off & 0x80: off -= 256
    return (addr + 2 + off) & 0xFFFF

def a11(pc, opcode, code):
    """AJMP/ACALL 目标 = (PC+2) 高 5 位 | opcode[2:0]<<8 | code"""
    return ((pc + 2) & 0xF800) | ((opcode & 0x07) << 8) | code

def a16(b1, b2):
    return (b1 << 8) | b2

AJMP  = {0x01,0x21,0x41,0x61,0x81,0xA1,0xC1,0xE1}
ACALL = {0x11,0x31,0x51,0x71,0x91,0xB1,0xD1,0xF1}

def disasm_one(data, i):
    """解码 data[i] 处一条指令 -> (助记符, 长度)"""
    b0 = data[i]
    b1 = data[i+1] if i+1 < len(data) else 0
    b2 = data[i+2] if i+2 < len(data) else 0
    lo = b0 & 0x0F
    reg = f'R{lo - 8}'
    ind = f'@R{lo & 1}'          # 0x06/0x07 -> @R0/@R1 (lo-6)
    ind = f'@R{lo - 6}'
    m, ln = '???', 1

    if b0 in AJMP:  m, ln = f'AJMP 0x{a11(i,b0,b1):04X}', 2
    elif b0 in ACALL: m, ln = f'ACALL 0x{a11(i,b0,b1):04X}', 2
    elif b0 == 0x00: m = 'NOP'
    elif b0 == 0x02: m, ln = f'LJMP 0x{a16(b1,b2):04X}', 3
    elif b0 == 0x12: m, ln = f'LCALL 0x{a16(b1,b2):04X}', 3
    elif b0 == 0x03: m = 'RR A'
    elif b0 == 0x13: m = 'RRC A'
    elif b0 == 0x23: m = 'RL A'
    elif b0 == 0x33: m = 'RLC A'
    elif b0 == 0x22: m = 'RET'
    elif b0 == 0x32: m = 'RETI'
    elif b0 == 0x04: m = 'INC A'
    elif b0 == 0x14: m = 'DEC A'
    elif b0 == 0x05: m, ln = f'INC {sfrname(b1)}', 2
    elif b0 == 0x15: m, ln = f'DEC {sfrname(b1)}', 2
    elif b0 in (0x06,0x07): m = f'INC {ind}'
    elif b0 in (0x16,0x17): m = f'DEC {ind}'
    elif 0x08 <= b0 <= 0x0F: m = f'INC {reg}'
    elif 0x18 <= b0 <= 0x1F: m = f'DEC {reg}'
    elif b0 == 0x10: m, ln = f'JBC {bitname(b1)}, 0x{rel(i,b2):04X}', 3
    elif b0 == 0x20: m, ln = f'JB {bitname(b1)}, 0x{rel(i,b2):04X}', 3
    elif b0 == 0x30: m, ln = f'JNB {bitname(b1)}, 0x{rel(i,b2):04X}', 3
    elif b0 == 0x40: m, ln = f'JC 0x{rel(i,b1):04X}', 2
    elif b0 == 0x50: m, ln = f'JNC 0x{rel(i,b1):04X}', 2
    elif b0 == 0x60: m, ln = f'JZ 0x{rel(i,b1):04X}', 2
    elif b0 == 0x70: m, ln = f'JNZ 0x{rel(i,b1):04X}', 2
    elif b0 == 0x80: m, ln = f'SJMP 0x{rel(i,b1):04X}', 2
    elif 0x28 <= b0 <= 0x2F: m = f'ADD A, {reg}'
    elif 0x38 <= b0 <= 0x3F: m = f'ADDC A, {reg}'
    elif 0x58 <= b0 <= 0x5F: m = f'ANL A, {reg}'
    elif 0x48 <= b0 <= 0x4F: m = f'ORL A, {reg}'
    elif 0x68 <= b0 <= 0x6F: m = f'XRL A, {reg}'
    elif 0x88 <= b0 <= 0x8F: m, ln = f'MOV {sfrname(b1)}, {reg}', 2
    elif 0x98 <= b0 <= 0x9F: m = f'SUBB A, {reg}'
    elif b0 == 0x94: m, ln = f'SUBB A, #{b1:02X}', 2
    elif b0 == 0x95: m, ln = f'SUBB A, {sfrname(b1)}', 2
    elif b0 in (0x96,0x97): m = f'SUBB A, {ind}'
    elif 0xA8 <= b0 <= 0xAF: m, ln = f'MOV {reg}, {sfrname(b1)}', 2
    elif 0xB8 <= b0 <= 0xBF: m, ln = f'CJNE {reg}, #{b1:02X}, 0x{rel(i,b2):04X}', 3
    elif 0xC8 <= b0 <= 0xCF: m = f'XCH A, {reg}'
    elif 0xD8 <= b0 <= 0xDF: m, ln = f'DJNZ {reg}, 0x{rel(i,b1):04X}', 2
    elif 0xE8 <= b0 <= 0xEF: m = f'MOV A, {reg}'
    elif 0xF8 <= b0 <= 0xFF: m = f'MOV {reg}, A'
    elif b0 == 0x24: m, ln = f'ADD A, #{b1:02X}', 2
    elif b0 == 0x25: m, ln = f'ADD A, {sfrname(b1)}', 2
    elif b0 in (0x26,0x27): m = f'ADD A, {ind}'
    elif b0 == 0x34: m, ln = f'ADDC A, #{b1:02X}', 2
    elif b0 == 0x35: m, ln = f'ADDC A, {sfrname(b1)}', 2
    elif b0 in (0x36,0x37): m = f'ADDC A, {ind}'
    elif b0 == 0x42: m, ln = f'ORL {sfrname(b1)}, A', 2
    elif b0 == 0x43: m, ln = f'ORL {sfrname(b1)}, #{b2:02X}', 3
    elif b0 == 0x44: m, ln = f'ORL A, #{b1:02X}', 2
    elif b0 == 0x45: m, ln = f'ORL A, {sfrname(b1)}', 2
    elif b0 in (0x46,0x47): m = f'ORL A, {ind}'
    elif b0 == 0x52: m, ln = f'ANL {sfrname(b1)}, A', 2
    elif b0 == 0x53: m, ln = f'ANL {sfrname(b1)}, #{b2:02X}', 3
    elif b0 == 0x54: m, ln = f'ANL A, #{b1:02X}', 2
    elif b0 == 0x55: m, ln = f'ANL A, {sfrname(b1)}', 2
    elif b0 in (0x56,0x57): m = f'ANL A, {ind}'
    elif b0 == 0x62: m, ln = f'XRL {sfrname(b1)}, A', 2
    elif b0 == 0x63: m, ln = f'XRL {sfrname(b1)}, #{b2:02X}', 3
    elif b0 == 0x64: m, ln = f'XRL A, #{b1:02X}', 2
    elif b0 == 0x65: m, ln = f'XRL A, {sfrname(b1)}', 2
    elif b0 in (0x66,0x67): m = f'XRL A, {ind}'
    elif b0 == 0x72: m, ln = f'ORL C, {bitname(b1)}', 2
    elif b0 == 0x82: m, ln = f'ANL C, {bitname(b1)}', 2
    elif b0 == 0x92: m, ln = f'MOV {bitname(b1)}, C', 2
    elif b0 == 0xA2: m, ln = f'MOV C, {bitname(b1)}', 2
    elif b0 == 0xA0: m, ln = f'ORL C, /{bitname(b1)}', 2
    elif b0 == 0xB0: m, ln = f'ANL C, /{bitname(b1)}', 2
    elif b0 == 0xB2: m, ln = f'CPL {bitname(b1)}', 2
    elif b0 == 0xB3: m = 'CPL C'
    elif b0 == 0xA3: m = 'INC DPTR'
    elif b0 == 0xC0: m, ln = f'PUSH {sfrname(b1)}', 2
    elif b0 == 0xD0: m, ln = f'POP {sfrname(b1)}', 2
    elif b0 == 0xC2: m, ln = f'CLR {bitname(b1)}', 2
    elif b0 == 0xC3: m = 'CLR C'
    elif b0 == 0xC4: m = 'SWAP A'
    elif b0 == 0xC5: m, ln = f'XCH A, {sfrname(b1)}', 2
    elif b0 in (0xC6,0xC7): m = f'XCH A, {ind}'
    elif b0 == 0xD2: m, ln = f'SETB {bitname(b1)}', 2
    elif b0 == 0xD3: m = 'SETB C'
    elif b0 == 0xD4: m = 'DA A'
    elif b0 == 0xD5: m, ln = f'DJNZ {sfrname(b1)}, 0x{rel(i,b2):04X}', 3
    elif b0 == 0xD6: m = 'XCHD A, @R0'
    elif b0 == 0xD7: m = 'XCHD A, @R1'
    elif b0 == 0xE0: m = 'MOVX A, @DPTR'
    elif b0 in (0xE2,0xE3): m = f'MOVX A, {ind}'
    elif b0 == 0xF0: m = 'MOVX @DPTR, A'
    elif b0 in (0xF2,0xF3): m = f'MOVX {ind}, A'
    elif b0 == 0xE4: m = 'CLR A'
    elif b0 == 0xF4: m = 'CPL A'
    elif b0 == 0xE5: m, ln = f'MOV A, {sfrname(b1)}', 2
    elif b0 == 0xF5: m, ln = f'MOV {sfrname(b1)}, A', 2
    elif b0 in (0xE6,0xE7): m = f'MOV A, {ind}'
    elif b0 in (0xF6,0xF7): m = f'MOV {ind}, A'
    elif b0 == 0x84: m = 'DIV AB'
    elif b0 == 0xA4: m = 'MUL AB'
    elif b0 == 0x93: m = 'MOVC A, @A+DPTR'
    elif b0 == 0x83: m = 'MOVC A, @A+PC'
    elif b0 == 0x73: m = 'JMP @A+DPTR'
    elif b0 == 0x90: m, ln = f'MOV DPTR, #{a16(b1,b2):04X}', 3
    elif b0 == 0x74: m, ln = f'MOV A, #{b1:02X}', 2
    elif b0 == 0x75: m, ln = f'MOV {sfrname(b1)}, #{b2:02X}', 3
    elif b0 in (0x76,0x77): m, ln = f'MOV {ind}, #{b1:02X}', 2
    elif 0x78 <= b0 <= 0x7F: m, ln = f'MOV {reg}, #{b1:02X}', 2
    elif b0 == 0x85: m, ln = f'MOV {sfrname(b2)}, {sfrname(b1)}', 3
    elif b0 in (0x86,0x87): m, ln = f'MOV {sfrname(b1)}, {ind}', 2
    elif b0 in (0xA6,0xA7): m, ln = f'MOV {ind}, {sfrname(b1)}', 2
    elif b0 == 0xB4: m, ln = f'CJNE A, #{b1:02X}, 0x{rel(i,b2):04X}', 3
    elif b0 == 0xB5: m, ln = f'CJNE A, {sfrname(b1)}, 0x{rel(i,b2):04X}', 3
    elif b0 in (0xB6,0xB7): m, ln = f'CJNE {ind}, #{b1:02X}, 0x{rel(i,b2):04X}', 3
    else:
        m = f'?? 0x{b0:02X}'
    return m, ln

def disasm(data, addr, end):
    out = []
    i = addr
    while i < end and i < len(data):
        m, ln = disasm_one(data, i)
        raw = ' '.join(f'{data[i+k]:02X}' for k in range(min(ln, 6)))
        out.append((i, m, ln, raw))
        i += ln
    return out

if __name__ == '__main__':
    fw = open(sys.argv[1], 'rb').read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else min(len(fw), 0x20000)
    for a, m, ln, raw in disasm(fw, start, end):
        print(f'{a:06X}: {raw:<18} {m}')
