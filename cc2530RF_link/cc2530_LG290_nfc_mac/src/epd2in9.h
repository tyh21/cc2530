/**
 * @file    epd2in9.h
 * @brief   2.9 寸 EPD 驱动 (SSD1680, WFT0290CZ10, LG 电子价签板 CC2530F256)
 *          bit-bang SPI, 刷新序列 = 微雪 EPD_2in9d (V2.0, 实测完美点亮)
 *
 * 驱动序列来源:
 *   全刷/局刷/休眠: Waveshare EPD_2in9d.cpp (ws_drivers/)
 *   引脚/电源:      原厂固件逆向 (yuanma_LG_290, 256KB 全镜像)
 *
 * 电源控制 (P0.7 -> EPD 供电 MOS, 高电平有效):
 *   原固件 0x11735: 上电(SETB P0.7) 后立即硬件复位脉冲
 *   原固件 0x11766: 深度休眠 CLR P0.7 断电
 *   原固件 0x11B3D: 初始化前检查 P0.7 电平, 为 0 则报电源错误
 */
#ifndef __EPD2IN9_H__
#define __EPD2IN9_H__

#include <ioCC2530.h>
#include "fonts.h"

#ifndef _UINT8_T_DEFINED
#define _UINT8_T_DEFINED
typedef unsigned char  uint8;
#endif
#ifndef _UINT16_T_DEFINED
#define _UINT16_T_DEFINED
typedef unsigned short uint16;
#endif

/* 面板物理分辨率 (不可变): 296 条 gate 线 x 128 个 source 位
 * 帧缓冲按面板原生布局: 每行 16 字节 x 296 行 = 4736 字节 */
#define EPD_WIDTH       128      /* 面板 source 方向像素数 */
#define EPD_HEIGHT      296      /* 面板 gate 线数 */
#define EPD_IMG_BYTES   (EPD_WIDTH / 8 * EPD_HEIGHT)   /* 4736 字节 */
#define EPD_LINE_BYTES  (EPD_WIDTH / 8)                /* 16 字节/行 */

/* 横屏逻辑分辨率 (软件旋转): 宽 296 x 高 128 */
#define EPD_LAND_W      296
#define EPD_LAND_H      128

/* 旋转方向: 1 = 顺时针 90° (默认), 0 = 逆时针 90°
 * 如果横屏后发现内容是倒的 (转了 180°), 把这里改成 0 */
#define EPD_ROT_CW      1

/* LG 价签实测引脚映射 (万用表实测 + 原厂固件逆向双重验证)
 * 注: 固件中 RST=P1.1 / BUSY=P1.2, 与实测表 (BUSY=P1.1/RST=P1.2) 对调;
 *     CC2530 脚 8=P1.2 与脚 9=P1.1 相邻, 实测时极易数错, 以固件为准。
 *     如果屏始终不 init, 把下面 EPD_BUSY/EPD_RST 互换再试。
 */
#define EPD_BUSY        P1_2     /* 输入, 空闲=高, 忙=低 (固件 0x11771/0x11B6A 等待拉高) */
#define EPD_RST         P1_1     /* 硬复位 (固件 0x11735 复位脉冲) */
#define EPD_DC          P0_2     /* 0=命令 1=数据 */
#define EPD_CS          P0_4     /* 片选, 低有效 (固件 0x116EC/0x1171F) */
#define EPD_SCLK        P0_5     /* 原固件走 USART0 SPI 硬件时钟 (Alt1 SCK) */
#define EPD_SDI         P0_3     /* 原固件走 USART0 SPI 硬件 MOSI (Alt1 MOSI) */

/* ---------------- 电源控制: P0.7 -> EPD 供电 MOS, 高电平=上电 ----------------
 * 原固件: 0x11735 SETB P0.7 上电 / 0x11766 CLR P0.7 断电 / 0x11B3D 检查电平
 */
#define EPD_PWR             P0_7
#define EPD_PWR_ON_LVL      1
#define EPD_PWR_OFF_LVL     0

/* ---------------- 全刷 API ---------------- */
void EPD_GpioInit(void);
void EPD_PowerOn(void);                /* P0.7 上电 */
void EPD_PowerOff(void);               /* P0.7 断电 */
void EPD_Init(void);                    /* 上电初始化 (微雪 2in9d 序列) */
void EPD_Fill(uint8 color);             /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img);/* 4736 字节图, 全刷 */
void EPD_Display(void);                 /* 把已写入 RAM 的内容全刷出来 */
void EPD_Sleep(void);                   /* 深度睡眠 + 断电 (P0.7 拉低) */

/* ---------------- 局刷 API (微雪 2in9d 序列) ----------------
 * 用法:
 *   1. EPD_Init();                 全刷初始化
 *   2. EPD_DisplayBase(白底图);     打底: 全刷一次
 *   3. 循环 EPD_DisplayPartial(帧); 局刷 LUT + 部分窗, 无闪烁
 *
 * 进阶: EPD_DisplayPartialD(old,new) 差分局刷, old/new 都写
 */
void EPD_DisplayBase(const uint8 *img);   /* 全刷打底 (内部先退出局刷模式) */
void EPD_DisplayPartial(const uint8 *img);/* 局刷一帧: 装 2in9d 局刷 LUT + 0x13 + 0x12 */
void EPD_DisplayPartialD(const uint8 *old_img, const uint8 *new_img); /* 差分局刷 */

/* ---------------- 横屏帧缓冲 + 画图 API ----------------
 * EPD_Frame 为面板原生布局, 画图用横屏坐标 (x:0-295, y:0-127)
 * 旋转方向由 EPD_ROT_CW 宏决定, 内容倒了改宏即可
 */
extern __xdata uint8 EPD_Frame[EPD_IMG_BYTES];   /* 帧缓冲 (面板原生布局) */
void EPD_FrameClear(uint8 color);                  /* 帧清为 color (0xFF=白) */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black); /* 横屏画点: x0-295, y0-127, black:1=黑 */
void EPD_ShowFrame(void);                          /* 局刷显示 EPD_Frame (0x0C) */
/* 画图 API (横屏坐标) */
void EPD_DrawLine(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawFilledRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawCircle(int xc, int yc, int r, uint8 black);
void EPD_DrawFilledCircle(int xc, int yc, int r, uint8 black);
/* 字符绘制 (横屏坐标) */
void EPD_DrawChar(uint16 x, uint16 y, char ch, const sFONT *font, uint8 black);
void EPD_DrawString(uint16 x, uint16 y, const char *text, const sFONT *font, uint8 black);

#endif /* __EPD2IN9_H__ */

