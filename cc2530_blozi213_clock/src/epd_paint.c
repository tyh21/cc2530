/**
 * @file    epd_paint.c
 * @brief   EPD 画布层实现 (横屏 212x104) + UTF-8 文本绘制
 *
 * 移植自 DA14585 日历工程的 GUI_Paint.c (Waveshare 风格), 主要改动:
 *   1. 字模/文本指针改成 IAR 8051 的 __code 指针
 *   2. Paint_SetPixel 直接落到 epd2in13.c 的画点 (复用已验证的横屏映射)
 *   3. 去掉旋转/镜像/灰度, 只保留本屏要用的黑白
 */
#include "epd_paint.h"

void Paint_Clear(uint8 color)
{
    EPD_FrameClear(color == PAINT_BLACK ? 0x00 : 0xFF);
}

void Paint_SetPixel(uint16 x, uint8 y, uint8 color)
{
    /* EPD_SetPixel 的 black 参数: 1 = 黑 */
    EPD_SetPixel(x, y, (color == PAINT_BLACK) ? 1 : 0);
}

/* 把一个字模 (w x h, 每行 ceil(w/8) 字节, MSB 先) 画到 (qx, qy)
 * 与 CANVAS 边界裁剪由 Paint_SetPixel/EPD_SetPixel 负责 */
static void Paint_DrawGlyph(uint16 qx, uint8 qy, const uint8 __code *bits,
                            uint8 w, uint8 h, uint8 fg, uint8 bg)
{
    uint8 row, col, bytes;
    uint16 x;
    uint8  b;

    bytes = (uint8)((w + 7) >> 3);

    for (row = 0; row < h; row++)
    {
        uint16 base = (uint16)row * bytes;
        for (col = 0; col < w; col++)
        {
            b = bits[base + (col >> 3)];
            x = (uint16)(qx + col);
            if (b & (0x80 >> (col & 7)))
                Paint_SetPixel(x, (uint8)(qy + row), fg);
            else if (bg != PAINT_WHITE)          /* 背景为白时省掉, 加快速度 */
                Paint_SetPixel(x, (uint8)(qy + row), bg);
        }
    }
}

void EPD_DrawUTF8(uint16 x, uint8 y, uint8 gap, const char *str,
                  const uint8 __code *ascii_font,
                  const uint8 __code *utf8_font,
                  uint8 fg, uint8 bg)
{
    uint16 x_count = 0;
    uint8  i;
    uint8  ch;

    while (*str != '\0')
    {
        ch = (uint8)(*str);

        if ((ch & 0x80) == 0x00)                 /* 普通 ASCII */
        {
            if (ascii_font != 0)
            {
                uint8  first = ascii_font[0];
                uint8  w     = ascii_font[1];
                uint8  h     = ascii_font[2];
                uint8  cnt   = ascii_font[3];
                uint16 fsize = (uint16)(((w + 7) >> 3)) * h;

                if (ch >= first && (uint16)(ch - first) < cnt)
                {
                    const uint8 __code *base =
                        ascii_font + 4 + (uint16)(ch - first) * fsize;
                    Paint_DrawGlyph(x + x_count, y, base, w, h, fg, bg);
                }
                x_count += (uint16)w + gap;
            }
            else if (ch == ' ' && utf8_font != 0)   /* 无 ASCII 字体时空格按半宽 */
            {
                x_count += (uint16)(utf8_font[1] / 2) + gap;
            }
        }
        else if (utf8_font != 0)                 /* UTF-8 汉字 (2/3/4 字节) */
        {
            uint32 uni = 0;
            uint8  n = 0;

            for (i = 0; i < 5; i++)
            {
                if (ch & (0x80 >> i))
                    n++;
                else
                    break;
            }

            switch (n)
            {
            case 2:
                uni  = (uint32)(ch & 0x1F) << 6;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F);
                break;
            case 3:
                uni  = (uint32)(ch & 0x0F) << 12;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F) << 6;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F);
                break;
            case 4:
                uni  = (uint32)(ch & 0x07) << 18;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F) << 12;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F) << 6;
                str++;
                uni |= (uint32)((uint8)(*str) & 0x3F);
                break;
            default:
                break;
            }

            if (uni != 0)
            {
                uint8  w   = utf8_font[1];
                uint8  h   = utf8_font[2];
                uint8  cnt = utf8_font[3];
                uint16 fsize  = (uint16)(((w + 7) >> 3)) * h;
                uint16 stride = (uint16)(fsize + 3);

                for (i = 0; i < cnt; i++)
                {
                    uint32 t = ((uint32)utf8_font[4 + stride * i] << 16) |
                               ((uint32)utf8_font[5 + stride * i] << 8)  |
                               ((uint32)utf8_font[6 + stride * i]);
                    if (t == uni)
                    {
                        Paint_DrawGlyph(x + x_count, y,
                                        utf8_font + 7 + stride * i,
                                        w, h, fg, bg);
                        break;
                    }
                }
            }

            x_count += (uint16)utf8_font[1] + gap;
        }

        str++;
    }
}
