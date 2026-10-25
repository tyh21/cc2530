# -*- coding: utf-8 -*-
HEX_PATH = r"C:\\Users\\tyh21\\Desktop\\cc2530\\213资料\\yuanma_Blozi_213_cc2530.hex"
img = bytearray(0x10000); ext_base = 0
for line in open(HEX_PATH, "r"):
    line = line.strip()
    if not line.startswith(":"): continue
    cnt  = int(line[1:3],16); addr = int(line[3:7],16); typ = int(line[7:9],16)
    data = bytes.fromhex(line[9:9+cnt*2])
    if typ == 0:
        a = ext_base+addr; img[a:a+cnt] = data
    elif typ == 4:
        ext_base = ((data[0]<<8)|data[1])<<16
    elif typ == 1: break
img = bytes(img)

# 找所有 SendCmd: 79 xx 12 0B C3
sites = []
for pc in range(0x7FFE):
    if img[pc]==0x79 and img[pc+2]==0x12 and img[pc+3]==0x0B and img[pc+4]==0xC3:
        sites.append((pc, img[pc+1]))

print("=== 所有 0x32 (LUT) SendCmd 点 ===")
for pc, cmd in sites:
    if cmd == 0x32:
        print("  SendCmd(0x32) @%04X" % pc)
        # 后面应该跟 70 个 SendData: 79 xx 12 0B EE
        p = pc + 5
        bytes_out = []
        for _ in range(70):
            # 找下一个 79 xx 12 0B EE
            found = False
            for skip in range(0, 12):
                q = p + skip
                if q+4 < len(img) and img[q]==0x79 and img[q+2]==0x12 and img[q+3]==0x0B and img[q+4]==0xEE:
                    bytes_out.append(img[q+1])
                    p = q + 5
                    found = True
                    break
            if not found:
                break
        print("   提取到 %d 字节:" % len(bytes_out))
        for i in range(0, len(bytes_out), 7):
            print("     " + " ".join("%02X" % b for b in bytes_out[i:i+7]))
