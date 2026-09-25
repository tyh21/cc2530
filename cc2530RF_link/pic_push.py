#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pic_push.py - PC 端图片推送工具 (三板 CC2530 价签组网)

用法:
    python pic_push.py <图片路径> <COM口> [波特率]
    python pic_push.py photo.jpg COM5
    python pic_push.py logo.png COM5 115200

链路:
    PC --USB-TTL 115200--> LG213 网关 --BasicRF--> LG290 / blozi290 节点
    三块屏同时显示同一张图 (213 全刷 ~15s, 290 全刷 ~3s)

图像处理:
    任意图片 -> 等比缩放居中裁剪到 212x104 -> Floyd-Steinberg 抖动二值
    -> 行优先位流 (MSB first, bit=1 黑) -> 2756 字节

协议 (与固件 pic_link 一致):
    UART 帧: A5 5A | LEN16 | TYPE | PAYLOAD | CRC16(0x1021/0xFFFF)
    流程:    START -> DATA x29 (每帧停等 ACK) -> END
    可靠性:  每帧 CRC + 每数据块内层 CRC + 超时重发 x3
             任何失败整图重传; START 超时较长 (网关可能在刷上一张图)

依赖: pip install pillow pyserial
"""

import sys
import time

try:
    from PIL import Image, ImageOps
except ImportError:
    sys.exit("需要 Pillow: pip install pillow")

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

# ---------------- 协议常量 (与固件 pl_proto.h 一致) ----------------
IMG_W, IMG_H = 212, 104
IMG_BYTES = IMG_W * IMG_H // 8          # 2756
CHUNK = 96
N_CHUNKS = (IMG_BYTES + CHUNK - 1) // CHUNK   # 29
LAST_LEN = IMG_BYTES - (N_CHUNKS - 1) * CHUNK # 68

U_SYNC_H, U_SYNC_L = 0xA5, 0x5A
U_START, U_DATA, U_END = 0x01, 0x02, 0x03
U_ACK, U_NAK = 0x80, 0x81

RETRIES = 3
ACK_TIMEOUT = 0.5                        # DATA/END 停等超时
START_TIMEOUT = 20.0                     # START 超时 (网关可能在刷上一张图 ~15s)


def crc16(data: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF), 与固件 pl_crc16 一致"""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if (c & 0x8000) else (c << 1)
            c &= 0xFFFF
    return c


def make_frame(ftype: int, payload: bytes) -> bytes:
    """UART 帧: A5 5A | LEN16 | TYPE | PAYLOAD | CRC16(TYPE+PAYLOAD)"""
    body = bytes([ftype]) + payload
    c = crc16(body)
    return bytes([U_SYNC_H, U_SYNC_L,
                  (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) \
        + body + bytes([(c >> 8) & 0xFF, c & 0xFF])


def image_to_bits(path: str) -> bytes:
    """任意图片 -> 2756 字节黑白位流 (bit=1 黑, MSB first, 行优先)"""
    img = Image.open(path).convert('L')
    img = ImageOps.fit(img, (IMG_W, IMG_H), Image.LANCZOS)  # 等比缩放+居中裁剪

    try:                                                    # Floyd-Steinberg 抖动
        mono = img.convert('1', dither=Image.Dither.FLOYDSTEINBERG)
    except AttributeError:                                  # 旧版 Pillow 兼容
        mono = img.convert('1', dither=Image.FLOYDSTEINBERG)

    px = list(mono.getdata())                               # 0=黑, 255=白
    out = bytearray(IMG_BYTES)
    for i in range(IMG_BYTES):
        b = 0
        for bit in range(8):
            k = i * 8 + bit
            if k >= IMG_W * IMG_H:
                break
            if px[k] == 0:                                  # 黑 -> bit=1
                b |= 0x80 >> bit
        out[i] = b
    return bytes(out)


class Gateway:
    def __init__(self, port: str, baud: int = 115200):
        self.ser = serial.Serial(port, baud, timeout=ACK_TIMEOUT)

    def _read_frame(self, timeout: float):
        """读一帧, 返回 (type, payload) 或 None"""
        deadline = time.time() + timeout
        buf = bytearray()
        prev = 0
        while time.time() < deadline:
            self.ser.timeout = max(0.05, deadline - time.time())
            b = self.ser.read(1)
            if not b:
                continue
            b = b[0]
            if prev == U_SYNC_H and b == U_SYNC_L:
                hdr = self.ser.read(3)                     # LEN_H LEN_L TYPE
                if len(hdr) < 3:
                    prev = 0
                    continue
                ln = (hdr[0] << 8) | hdr[1]
                rest = self.ser.read(ln + 2)               # PAYLOAD + CRC
                if len(rest) < ln + 2:
                    prev = 0
                    continue
                pay = rest[:ln]
                crc_rx = (rest[ln] << 8) | rest[ln + 1]
                if crc16(bytes([hdr[2]]) + pay) == crc_rx:
                    return hdr[2], pay
                prev = 0
                continue
            prev = b
        return None

    def send_wait(self, ftype: int, payload: bytes, timeout: float) -> bool:
        """发一帧并等 ACK (NAK/超时返回 False)"""
        frame = make_frame(ftype, payload)
        for _ in range(RETRIES):
            self.ser.reset_input_buffer()
            self.ser.write(frame)
            resp = self._read_frame(timeout)
            if resp and resp[0] == U_ACK:
                return True
            print("    重发...")
        return False

    def push(self, path: str):
        print(f"[1/3] 图像处理: {path}")
        img = image_to_bits(path)
        print(f"      212x104 位流 {len(img)} 字节, 整图 CRC=0x{crc16(img):04X}")

        print("[2/3] 发送 START (网关忙时可能等待较久)...")
        if not self.send_wait(U_START, len(img).to_bytes(2, 'big'), START_TIMEOUT):
            print("失败: START 无 ACK, 检查网关接线和串口")
            return False

        for seq in range(N_CHUNKS):
            data = img[seq * CHUNK:(seq + 1) * CHUNK]
            blk = bytes([seq >> 8, seq & 0xFF, len(data)]) + data
            c = crc16(blk)
            payload = blk + bytes([(c >> 8) & 0xFF, c & 0xFF])
            if not self.send_wait(U_DATA, payload, ACK_TIMEOUT):
                print(f"失败: DATA 块 {seq} 无 ACK (整图需重传)")
                return False
            print(f"\r[3/3] RF 转发: {seq + 1}/{N_CHUNKS} 块", end='', flush=True)
        print()

        if not self.send_wait(U_END, b'', ACK_TIMEOUT):
            print("失败: END 无 ACK")
            return False

        print("完成! 等待三块屏刷出图片 (213 ~15s, 290 ~3s)")
        return True


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    path, port = sys.argv[1], sys.argv[2]
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

    try:
        gw = Gateway(port, baud)
    except serial.SerialException as e:
        sys.exit(f"打开串口失败: {e}\n提示: 关闭占用串口的程序, 或检查 USB-TTL 是否插好")

    ok = gw.push(path)
    sys.exit(0 if ok else 2)


if __name__ == '__main__':
    main()
