# -*- coding: utf-8 -*-
"""探针: 扫描原厂固件 yuanma_Blozi_213 里所有 0x22(display update) 发送点,
找局刷流程 (0x22 取值 != 0xC7/0xB1 的调用, 尤其 0x04/0x0C/0x2C/0xC0)。"""
import re, sys

HEX_PATH = r"C:\Users\tyh21\Desktop\cc2530\213资料\yuanma_Blozi_213_cc2530.hex"
DIS_SRC  = r"C:\Users\tyh21\Desktop\cc2530\.temp\dis8051.py"

# ---------- 1. Intel HEX -> flat bin ----------
img = bytearray(0x10000)
ext_base = 0
nrec = 0
for line in open(HEX_PATH, "r"):
    line = line.strip()
    if not line.startswith(":"):
        continue
    cnt  = int(line[1:3], 16)
    addr = int(line[3:7], 16)
    typ  = int(line[7:9], 16)
    data = bytes.fromhex(line[9:9 + cnt * 2])
    nrec += 1
    if typ == 0:
        a = ext_base + addr
        img[a:a + cnt] = data
    elif typ == 4:
        ext_base = ((data[0] << 8) | data[1]) << 16
    elif typ == 1:
        break
img = bytes(img)
print("hex records=%d, size=%d bytes" % (nrec, len(img)))

# ---------- 2. 借用 dis8051.py 的 OPS 表 ----------
src = open(DIS_SRC, encoding="utf-8").read()
ns = {}
head = src.split("# 常用 8051")[0]
head = head.replace(
    'BIN_PATH = r"C:\\Users\\tyh21\\Desktop\\cc2530\\.temp\\yuanma1.bin"\nwith open(BIN_PATH, "rb") as f:\n    img = f.read()',
    'img = b""')
exec(head, ns)                       # 只取 import sys
body = src[src.index("OPS = {"):src.index("BITS = {")]
exec(body, ns)
OPS = ns["OPS"]

def disasm(start, end):
    pc = start; out = []
    while pc < end:
        op = img[pc]
        if op in OPS:
            mn, ln = OPS[op]
            raw = img[pc:pc + ln]
            s = mn
            if op == 0x12: s = "LCALL 0x%04X" % ((raw[1] << 8) | raw[2])
            elif op == 0x02: s = "LJMP 0x%04X" % ((raw[1] << 8) | raw[2])
            elif 0x01 <= op <= 0x21 and (op & 0x1F) == 0x01:
                s = "AJMP 0x%04X" % (((pc + 2) & 0xF800) | ((op >> 5) << 8) | raw[1])
            elif 0x11 <= op <= 0xF1 and (op & 0x1F) == 0x11:
                s = "ACALL 0x%04X" % (((pc + 2) & 0xF800) | ((op >> 5) << 8) | raw[1])
            elif op in (0x80, 0x60, 0x70, 0x40, 0x50, 0x30, 0x20, 0x10):
                rel = raw[-1] if raw[-1] < 128 else raw[-1] - 256
                s = "%s 0x%04X" % (mn.split()[0], (pc + ln + rel) & 0xFFFF)
            elif op == 0x75: s = "MOV 0x%02X,#0x%02X" % (raw[1], raw[2])
            elif op == 0x74: s = "MOV A,#0x%02X" % raw[1]
            elif op == 0xB4: s = "CJNE A,#0x%02X,0x%04X" % (raw[1], (pc + 3 + (raw[2] - 256 if raw[2] > 127 else raw[2])) & 0xFFFF)
            elif 0x78 <= op <= 0x7F: s = "MOV R%d,#0x%02X" % (op - 0x78, raw[1])
            bytes_hex = " ".join("%02X" % x for x in raw)
            out.append("  %04X: %-12s %s" % (pc, bytes_hex, s))
            pc += ln
        else:
            out.append("  %04X: %02X           ???" % (pc, op)); pc += 1
    return "\n".join(out)

# ---------- 3. 已知锚点: 确认 sendcmd/senddata ----------
out = []
out.append("===== 0x0D60 全刷流程 (RF 注释: 0x22 C7 + 0x20) =====")
out.append(disasm(0x0D60, 0x0DB0))
out.append("")
out.append("===== 0x1316 写 new(0x24) / 0x1377 写 old(0x26) =====")
out.append(disasm(0x1300, 0x13D0))
txt = "\n".join(out)
open(r".temp\disasm_anchor.txt", "w", encoding="utf-8").write(txt)
print(txt)

# ---------- 4. 全局扫描 MOV A,#0x22 + (L)CALL ----------
hits = []
for pc in range(0x0000, 0x7FFE):
    if img[pc] == 0x74 and img[pc + 1] == 0x22:
        op2 = img[pc + 2]
        if op2 == 0x12:
            tgt = (img[pc + 3] << 8) | img[pc + 4]
            hits.append((pc, tgt, "LCALL"))
        elif (op2 & 0x1F) == 0x11:
            tgt = ((pc + 2) & 0xF800) | ((op2 >> 5) << 8) | img[pc + 3]
            hits.append((pc, tgt, "ACALL"))
print("\n=== MOV A,#0x22 + CALL 发送点 ===")
for pc, tgt, kind in hits:
    print("  %04X: MOV A,#0x22 ; %s 0x%04X" % (pc, kind, tgt))

raw22 = [pc for pc in range(0x7FFF) if img[pc] == 0x74 and img[pc + 1] == 0x22]
print("\n全部 74 22 出现位置:", " ".join("%04X" % p for p in raw22))

# ---------- 5. 每个 0x22 发送点后的上下文 (找 0x22 的数据值) ----------
out2 = ["===== 每个 0x22 发送点上下文 ====="]
for pc, tgt, kind in hits:
    s = pc + 5
    out2.append("---- 发送点 0x%04X (CALL 0x%04X) ----" % (pc, tgt))
    out2.append(disasm(s, s + 40))
    out2.append("")
txt2 = "\n".join(out2)
open(r".temp\disasm_22ctx.txt", "w", encoding="utf-8").write(txt2)
print("\n" + txt2)
