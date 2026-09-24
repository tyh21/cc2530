/**
 * @file    epd2in9.h
 * @brief   2.9 寸 EPD 驱动 (GDEH029A1 296x128, SSD1680 系命令集)
 *          针对 blozi_290 电子价签板 (CC2530F256) 的 bit-bang SPI
 *
 * 命令序列照抄该板原厂固件 (yuanma1.hex 逆向 @0x1142/0x1100/0x17C3/0x1832):
 *   Init : 0x12 / 0x74 54 / 0x7E 3B / 0x01 27 01 00 / 0x11 / 0x44 / 0x45 /
 *          0x3C 01 / 0x18 80 / 0x22 B1 / 0x20
 *   写RAM: 0x4E / 0x4F / 0x24 (4736 字节)
 *   刷新 : 0x21 40 / 0x22 C7 / 0x20
 *   睡眠 : 0x10 01
 *   BUSY : P0.4, 高电平=忙 (原固件 0x10DF: MOV C,P0.4; JC loop)
 */
#ifndef __EPD2IN9_H__
#define __EPD2IN9_H__

#include <ioCC2530.h>

typedef unsigned char  uint8;
typedef unsigned short uint16;

/* 分辨率: 296 行(gate) x 128 列(source), 每 8 列一个字节 */
#define EPD_WIDTH       128
#define EPD_HEIGHT      296
#define EPD_IMG_BYTES   (EPD_WIDTH / 8 * EPD_HEIGHT)   /* 4736 字节 */

/* 板上实测引脚映射 (CC2530 QFN-40) */
#define EPD_BUSY        P0_4
#define EPD_RST         P0_5
#define EPD_DC          P0_6
#define EPD_CS          P0_7
#define EPD_SCLK        P1_0
#define EPD_SDI         P1_1

void EPD_GpioInit(void);
void EPD_Init(void);                    /* 上电初始化, 内含一次全刷清屏 */
void EPD_Fill(uint8 color);             /* 全屏填充并发刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img);/* 4736 字节图, 从左上(0,0)逐行排列 */
void EPD_Display(void);                 /* 把已写入 0x24 RAM 的内容刷出来 */
void EPD_Sleep(void);                   /* 深度睡眠 (RST 硬复位唤醒) */

#endif /* __EPD2IN9_H__ */
