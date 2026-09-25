/**
 * @file    epd2in9.h
 * @brief   2.9 寸 EPD 驱动 - blozi 板 (GDEH029A1 296x128, SSD1680 系命令集)
 *          bit-bang SPI, CC2530F256
 *
 * [2026-09-25 重写] 本驱动 = cc2530_EPD290/05src_timer 实机点亮版本整体移植,
 * 此前按 "SSD1608 + 0x22=0xB1(OTP)" 逆向重写的版本实机不亮, 弃用。
 * 实机验证结论 (05src_timer 板上跑通):
 *   - 全刷必须 LUT 装载式: 0x32 写 70B 波形 + 0x03/0x04/0x3A/0x3B 参数,
 *     再 0x22=0xC7 + 0x20;  0x22=0xB1(OTP) 仅用于 Init 末尾清底色
 *   - GPIO init 必须先清外设功能: P0SEL &= ~0xF8; P1SEL &= ~0x03
 *   - 初始电平 CS=1 / DC=1 / RST=1 (CS/RST 常低 = 屏永远被复位!)
 *   - 电源 P0.3 低有效: 高500ms(断) -> 低500ms(通) -> Init
 *   - EPD_Sleep 只发 0x10 0x01, 不断电 (下次 EPD_Init 内 PowerOn 重走时序)
 *
 * 面板物理分辨率 (不可变): 296 条 gate 线 x 128 个 source 位
 * 帧缓冲按面板原生布局: 每行 16 字节 x 296 行 = 4736 字节
 */
#ifndef __EPD2IN9_H__
#define __EPD2IN9_H__

#include <ioCC2530.h>
#include "hal_types.h"          /* uint8 / uint16 (blozi290 工程既有依赖) */

/* 面板物理分辨率 */
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

/* 板上实测引脚映射 (CC2530 QFN-40) */
#define EPD_BUSY        P0_4     /* 输入, 高=忙 */
#define EPD_RST         P0_5
#define EPD_DC          P0_6     /* 0=命令 1=数据 */
#define EPD_CS          P0_7
#define EPD_SCLK        P1_0
#define EPD_SDI         P1_1

/* ---------------- 电源控制: P0.3 -> MOS 管开关 ----------------
 * 低电平=上电 (低有效, blozi 板实机验证 + 原厂固件 0x2266 时序):
 * 高 500ms (断电放电) -> 低 500ms (上电稳定)
 */
#define EPD_PWR             P0_3
#define EPD_PWR_ACTIVE_LOW  1

#if EPD_PWR_ACTIVE_LOW
#define EPD_PWR_ON_LVL      0
#define EPD_PWR_OFF_LVL     1
#else
#define EPD_PWR_ON_LVL      1
#define EPD_PWR_OFF_LVL     0
#endif

/* ---------------- 全刷 API ---------------- */
void EPD_GpioInit(void);
void EPD_PowerOn(void);                /* P0.3 上电: 高500ms放电->低500ms稳定 */
void EPD_PowerOff(void);               /* P0.3 断电 */
void EPD_Init(void);                   /* 上电初始化, 内含一次清屏刷新 */
void EPD_Fill(uint8 color);            /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img);/* 4736 字节图, 全刷 */
void EPD_Display(void);                /* 把已写入 0x24 RAM 的内容全刷出来 */
void EPD_Sleep(void);                  /* 深度睡眠 (只发 0x10 0x01, 不断电) */

void EPD_SendCommand(uint8 cmd);       /* 非 static: 供调试扩展 */
void EPD_SendData(uint8 dat);

/* ---------------- 局刷 API (epd2in13_V2 序列, 板上实测兼容) ----------------
 * 用法:
 *   1. EPD_Init();                 全刷初始化
 *   2. EPD_DisplayBase(白底图);     打底: 0x24+0x26 同图 + 全刷一次
 *   3. EPD_InitPartial();          装局刷 LUT
 *   4. 循环 EPD_DisplayPartial(帧); 只写 0x24 + 0x0C 快刷, 无闪烁
 */
void EPD_InitPartial(void);               /* 切局刷模式: 写 LUT/0x37 + 0xC0 装载 */
void EPD_DisplayBase(const uint8 *img);   /* 局刷基底: old(0x26)=new(0x24)+全刷 */
void EPD_DisplayPartial(const uint8 *img);/* 局刷一帧: 写 0x24 + 0x0C 快刷 */
void EPD_DisplayPartialD(const uint8 *old_img, const uint8 *new_img); /* 差分局刷 */

/* ---------------- 横屏帧缓冲 ----------------
 * EPD_Frame 为面板原生布局, 与 LG290 帧缓冲通用 (pl_proto.c 直接可用)
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

#endif /* __EPD2IN9_H__ */
