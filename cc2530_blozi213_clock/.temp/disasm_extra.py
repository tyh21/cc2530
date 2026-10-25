# -*- coding: utf-8 -*-
src = open(r"C:\Users\tyh21\Desktop\cc2530\.temp\dis8051.py", encoding="utf-8").read()
ns = {}
head = src.split("# 常用 8051")[0].replace(
    'BIN_PATH = r"C:\\Users\\tyh21\\Desktop\\cc2530\\.temp\\yuanma1.bin"\nwith open(BIN_PATH, "rb") as f:\n    img = f.read()', 'img = b""')
exec(head, ns)
body = src[src.index("OPS = {"):src.index("BITS = {")]
exec(body, ns)
OPS = ns["OPS"]

HEX_PATH = r"C:\Users\tyh21\Desktop\cc2530\213资料\yuanma_Blozi_213_cc2530.hex"
img = bytearray(0x10000); ext_base = 0
for line in open(HEX_PATH, "r"):
    line = line.strip()
    if not line.startswith(":"): continue
    cnt = int(line[1:3],16); addr = int(line[3:7],16); typ = int(line[7:9],16)
    data = bytes.fromhex(line[9:9+cnt*2])
    if typ == 0: a = ext_base+addr; img[a:a+cnt] = data
    elif typ == 4: ext_base = ((data[0]<<8)|data[1])<<16
    elif typ == 1: break
img = bytes(img)

def disasm(start, end):
    pc = start; out = []
    while pc < end:
        op = img[pc]
        if op in OPS:
            mn, ln = OPS[op]; raw = img[pc:pc+ln]; s = mn
            if op == 0x12: s = "LCALL 0x%04X" % ((raw[1]<<8)|raw[2])
            elif op == 0x02: s = "LJMP 0x%04X" % ((raw[1]<<8)|raw[2])
            elif op == 0x75: s = "MOV 0x%02X,#0x%02X" % (raw[1], raw[2])
            elif op == 0x74: s = "MOV A,#0x%02X" % raw[1]
            elif 0x78 <= op <= 0x7F: s = "MOV R%d,#0x%02X" % (op-0x78, raw[1])
            bytes_hex = " ".join("%02X" % x for x in raw)
            out.append("  %04X: %-12s %s" % (pc, bytes_hex, s)); pc += ln
        else:
            out.append("  %04X: %02X           ???" % (pc, op)); pc += 1
    return "\n".join(out)

print("===== init 尾部 0x0CCE-0x0D60 (0xB1清底 / 0x1B / 0x3D/3E/3F / 0x72 / 深睡) =====")
print(disasm(0x0CCE, 0x0D60))
print()
print("===== 0x1830-0x18A0 (第二个软复位上下文) =====")
print(disasm(0x1830, 0x18A0))
