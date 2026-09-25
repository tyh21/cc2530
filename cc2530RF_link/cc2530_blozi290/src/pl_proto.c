/**
 * @file    pl_proto.c
 * @brief   pic_link 公共协议实现: CRC16 + 统一图源到各屏面板帧的变换
 *
 * 变换公式 (与各板 epd2in9.c 的 EPD_SetPixel 映射一致, EPD_ROT_CW=1):
 *   横屏像素 (px,py)  ->  面板 gate 行 g, source 位 s
 *   帧字节 idx = g * EPD_LINE_BYTES + (s>>3), 位掩码 = 0x80>>(s&7)
 *   0=黑, 1=白 (帧缓冲中)
 *
 * LG213  (212x104 横屏): s = py,          g = 211 - px           (13B/行 x 212 行)
 * LG290  (296x128 横屏): 图居中嵌入 gx=px+42, sy=py+12,
 *                        s = sy,          g = 295 - gx = 253 - px (16B/行 x 296 行)
 * blozi290 同 LG290 面板布局 (GDEH029A1 128x296), 4736B 帧缓冲通用
 */
#include "pl_proto.h"
#include "epd2in9.h"

/* ==================== CRC16-CCITT ==================== */
uint16 pl_crc16(const uint8 *p, uint16 len)
{
    uint16 c = 0xFFFF;
    while (len--) {
        uint8 i;
        c ^= (uint16)((uint16)(*p++) << 8);
        for (i = 0; i < 8; i++) {
            if (c & 0x8000)
                c = (uint16)((c << 1) ^ 0x1021);
            else
                c = (uint16)(c << 1);
        }
    }
    return c;
}

/* ==================== LG213 变换 ==================== */
void pl_frame_write_213(uint16 off, const uint8 __xdata *data, uint16 len)
{
    uint16 k = (uint16)(off * 8);
    uint16 px = k % PL_IMG_W;
    uint16 py = k / PL_IMG_W;
    uint16 i;

    if (py >= PL_IMG_H)
        return;

    for (i = 0; i < len; i++) {
        uint8 d = data[i];
        uint8 b;
        for (b = 0; b < 8; b++) {
            if (px >= PL_IMG_W) { px = 0; py++; }
            if (py >= PL_IMG_H)
                return;
            if (d & (0x80 >> b)) {                    /* 1=黑 */
                uint16 idx = (uint16)(211 - px) * EPD_LINE_BYTES + (py >> 3);
                EPD_Frame[idx] &= (uint8)~(0x80 >> (py & 7));
            }
            px++;
        }
    }
}

void pl_to_frame_213(const uint8 __xdata *img)
{
    EPD_FrameClear(0xFF);
    pl_frame_write_213(0, img, PL_IMG_BYTES);
}

/* ==================== LG290 / blozi290 变换 ==================== */
void pl_frame_write_290(uint16 off, const uint8 __xdata *data, uint16 len)
{
    uint16 k = (uint16)(off * 8);
    uint16 px = k % PL_IMG_W;
    uint16 py = k / PL_IMG_W;
    uint16 i;

    if (py >= PL_IMG_H)
        return;

    for (i = 0; i < len; i++) {
        uint8 d = data[i];
        uint8 b;
        for (b = 0; b < 8; b++) {
            if (px >= PL_IMG_W) { px = 0; py++; }
            if (py >= PL_IMG_H)
                return;
            if (d & (0x80 >> b)) {                    /* 1=黑 */
                uint8  sy  = (uint8)(py + 12);        /* 垂直居中: 12+104+12=128 */
                uint16 gx  = px + 42;                 /* 水平居中: 42+212+42=296 */
                uint16 idx = (uint16)(295 - gx) * EPD_LINE_BYTES + (sy >> 3);
                EPD_Frame[idx] &= (uint8)~(0x80 >> (sy & 7));
            }
            px++;
        }
    }
}

void pl_to_frame_290(const uint8 __xdata *img)
{
    EPD_FrameClear(0xFF);
    pl_frame_write_290(0, img, PL_IMG_BYTES);
}
