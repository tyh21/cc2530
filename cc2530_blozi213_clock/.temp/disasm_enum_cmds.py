# -*- coding: utf-8 -*-
"""枚举原厂固件全部 EPD 命令调用点:
模式 A: 79 xx 12 0B C3  (MOV R1,#cmd ; LCALL SendCmd)
对每个 0x22 命令点, 再看紧随其后的 SendData 立即数 (0x22 的取值)"""
HEX_PATH = r"C:\Users\tyh21\Desktop\cc2530\213资料\yuanma_Blozi_213_cc2530.hex"

img = bytearray(0x10000)
ext_base = 0
for line in open(HEX_PATH, "r"):
    line = line.strip()
    if not line.startswith(":"):
        continue
    cnt  = int(line[1:3], 16)
    addr = int(line[3:7], 16)
    typ  = int(line[7:9], 16)
    data = bytes.fromhex(line[9:9 + cnt * 2])
    if typ == 0:
        a = ext_base + addr
        img[a:a + cnt] = data
    elif typ == 4:
        ext_base = ((data[0] << 8) | data[1]) << 16
    elif typ == 1:
        break
img = bytes(img)

CMDNAMES = {
    0x01:"DRIVER_OUTPUT", 0x0C:"BOOSTER_SOFTSTART", 0x10:"DEEP_SLEEP",
    0x11:"DATA_ENTRY", 0x12:"SOFT_RESET", 0x18:"TEMP_SENSOR",
    0x20:"MASTER_ACT", 0x22:"DISPLAY_UPDATE_CTRL", 0x24:"RAM_NEW",
    0x26:"RAM_OLD/RED", 0x2B:"VCOM_2B", 0x2C:"VCOM_DC", 0x32:"LUT",
    0x37:"BORDER_VCOM_PWR", 0x3A:"DUMMY_LINE", 0x3B:"GATE_TIME",
    0x3C:"BORDER_WF", 0x44:"RAM_X_WIN", 0x45:"RAM_Y_WIN",
    0x4E:"RAM_X_CNT", 0x4F:"RAM_Y_CNT", 0x74:"TEMP_WR", 0x7E:"TEMP_RD",
}

# 找所有 SendCmd 调用: 79 xx 12 0B C3
sites = []
for pc in range(0x7FFE):
    if img[pc] == 0x79 and img[pc + 2] == 0x12 and img[pc + 3] == 0x0B and img[pc + 4] == 0xC3:
        sites.append((pc, img[pc + 1]))

print("=== 全部 SendCmd 调用点 (MOV R1,#cmd ; LCALL 0x0BC3) ===")
for pc, cmd in sites:
    name = CMDNAMES.get(cmd, "?")
    print("  %04X: cmd=0x%02X  %s" % (pc, cmd, name))

print("\n=== 命令种类统计 ===")
from collections import Counter
c = Counter(cmd for _, cmd in sites)
for cmd, n in sorted(c.items()):
    print("  0x%02X %-20s x%d" % (cmd, CMDNAMES.get(cmd, "?"), n))

# 0x22 的取值: 对每个 0x22 SendCmd 点, 找紧随其后的 MOV R1,#imm ; LCALL 0x0BEE
print("\n=== 0x22 的数据取值 (SendCmd(0x22) 之后的第一个 SendData) ===")
vals = []
for pc, cmd in sites:
    if cmd != 0x22:
        continue
    p = pc + 5
    found = None
    for _ in range(8):                    # 允许隔几条指令
        if img[p] == 0x79 and img[p + 2] == 0x12 and img[p + 3] == 0x0B and img[p + 4] == 0xEE:
            found = img[p + 1]
            break
        ln = 1 if img[p] not in (0x75, 0x90, 0x12, 0x02, 0xB4) else (3 if img[p] != 0x75 else 3)
        # 粗略步进: 用常见长度
        op = img[p]
        ln = 3 if op in (0x12, 0x02, 0x90, 0x75) else 2 if op in (0x74, 0x79, 0x7A, 0x7B, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0x43, 0x53, 0x85, 0xB4, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0xC2, 0xD2, 0xA2, 0x92) else 1
        p += ln
    vals.append((pc, found))
    print("  SendCmd(0x22) @%04X -> data=%s" % (pc, "0x%02X" % found if found is not None else "??"))

# 反汇编每个 0x22 相关函数的完整上下文
src = open(r"C:\Users\tyh21\Desktop\cc2530\.temp\dis8051.py", encoding="utf-8").read()
ns = {}
head = src.split("# 常用 8051")[0].replace(
    'BIN_PATH = r"C:\\Users\\tyh21\\Desktop\\cc2530\\.temp\\yuanma1.bin"\nwith open(BIN_PATH, "rb") as f:\n    img = f.read()', 'img = b""')
exec(head, ns)
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
            elif 0x78 <= op <= 0x7F: s = "MOV R%d,#0x%02X" % (op - 0x78, raw[1])
            bytes_hex = " ".join("%02X" % x for x in raw)
            out.append("  %04X: %-12s %s" % (pc, bytes_hex, s))
            pc += ln
        else:
            out.append("  %04X: %02X           ???" % (pc, op)); pc += 1
    return "\n".join(out)

# 反汇编每个 0x22 发送点前 24 字节后 32 字节
out = ["===== 0x22 发送点完整上下文 ====="]
for pc, found in vals:
    out.append("---- 0x%04X (data=%s) ----" % (pc, "0x%02X" % found if found is not None else "??"))
    out.append(disasm(max(0, pc - 24), pc + 40))
    out.append("")
open(r".temp\disasm_22_full.txt", "w", encoding="utf-8").write("\n".join(out))
print("\n上下文已写入 .temp/disasm_22_full.txt")
