/**
 * @file    pl_proto.c
 * @brief   pic_link 公共协议实现: CRC16 + 统一图源到各屏面板帧的变换
 *
 * 变换公式 (与 epd2in13.c 的 EPD_SetPixel 帧排布一致):
 *   横屏像素 (px,py)  ->  帧字节 idx = px * EPD_LINE_BYTES + (py>>3),
 *                        位掩码 = 0x80>>(py&7), 1=白 0=黑
 *   (该排布逐字节等价 DA14585 原工程 Paint: 旋转270 + 垂直镜像)
 *
 * LG213  (212x104 横屏): idx = px*13 + (py>>3)                (13B/行 x 212 行)
 * LG290  (296x128 横屏): 图居中嵌入 gx=px+42, sy=py+12,
 *                        idx = gx*16 + (sy>>3)                 (16B/行 x 296 行)
 * blozi290 同 LG290 面板布局 (GDEH029A1 128x296), 4736B 帧缓冲通用
 * blozi213 同 LG213 面板布局, 2756B 帧缓冲, 用 *_213
 */
#include "pl_proto.h"
#include "epd2in13.h"

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
                uint16 idx = (uint16)px * EPD_LINE_BYTES + (py >> 3);
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
