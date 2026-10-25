/**
 * @file    epd_paint.h
 * @brief   EPD 画布层: 面向 "横屏 212x104" 的着色接口 + EPD_DrawUTF8 文本绘制
 *
 *   把 DA14585 日历工程 (da14585_213_104x212_calendar) 的 GUI_Paint / EPD_DrawUTF8
 *   移植到 CC2530 (IAR EW8051): 字模放 __code (ROM), 逐位写进 EPD_Frame。
 *   颜色约定与面板 RAM 位一致: 0 = 黑, 1 = 白。
 */
#ifndef __EPD_PAINT_H__
#define __EPD_PAINT_H__

#include "epd2in13.h"

/* 颜色 (与 EPD_Frame 位一致) */
#define PAINT_BLACK     0
#define PAINT_WHITE     1

/* 清屏 (整屏填色) */
void Paint_Clear(uint8 color);

/* 画点: x 0..211, y 0..103 */
void Paint_SetPixel(uint16 x, uint8 y, uint8 color);

/* ------------------------------------------------------------------
 * 绘制 UTF-8 字符串 (ASCII 走 ascii_font, 汉字走 utf8_font)
 *   x, y   : 左上角 (横屏坐标)
 *   gap    : 每个字符后额外间距
 *   ascii_font / utf8_font: 字模表 (__code), 传 0 表示该类字不画
 *   fg/bg  : 前景/背景色 (PAINT_BLACK / PAINT_WHITE)
 * 字模表格式 (与原工程一致):
 *   ASCII: [0]=首字符, [1]=宽, [2]=高, [3]=字符数, 之后每字 ceil(w/8)*h 字节
 *   UTF-8: [1]=宽, [2]=高, [3]=字符数, 之后每字 (3B 大端 UNICODE + ceil(w/8)*h 字节)
 * ------------------------------------------------------------------ */
void EPD_DrawUTF8(uint16 x, uint8 y, uint8 gap, const char *str,
                  const uint8 __code *ascii_font,
                  const uint8 __code *utf8_font,
                  uint8 fg, uint8 bg);

#endif /* __EPD_PAINT_H__ */
