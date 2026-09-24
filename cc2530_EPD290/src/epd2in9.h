/**
 * @file    epd2in9.h
 * @brief   2.9 寸 EPD 驱动 (GDEH029A1 296x128, SSD1680 系命令集)
 *          针对 blozi_290 电子价签板 (CC2530F256) 的 bit-bang SPI
 *
 * 命令序列来源:
 *   全刷: blozi_290 价签原厂固件逆向 (yuanma1.hex @0x1142/0x1100)
 *   局刷: Waveshare epd2in13_V2 序列 (板上实测兼容)
 *
 * 电源控制 (P0.3 -> MOS 管开关, 低有效):
 *   原固件 0x2266 时序: 高 500ms (断电放电) -> 低 500ms (上电稳定)
 */
#ifndef __EPD2IN9_H__
#define __EPD2IN9_H__

#include <ioCC2530.h>

/* 默认关闭横屏帧缓冲 (省 4736B XDATA), 需要时在 epd2in9.c 里 #define USE_EPD_FRAME 1 */
#ifndef USE_EPD_FRAME
#define USE_EPD_FRAME 0
#endif

typedef unsigned char  uint8;
typedef unsigned short uint16;

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

/* 板上实测引脚映射 (CC2530 QFN-40) */
#define EPD_BUSY        P0_4
#define EPD_RST         P0_5
#define EPD_DC          P0_6
#define EPD_CS          P0_7
#define EPD_SCLK        P1_0
#define EPD_SDI         P1_1

/* ---------------- 电源控制: P0.3 -> MOS 管开关 ----------------
 * EPD_PWR_ACTIVE_LOW = 1: 低电平=上电 (默认, 照原固件时序)
 * EPD_PWR_ACTIVE_LOW = 0: 高电平=上电 (如果第一种方向屏仍不亮, 改这里再试)
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
void EPD_Init(void);                    /* 上电初始化(OTP 全刷波形), 内含一次清屏 */
void EPD_Fill(uint8 color);             /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img);/* 4736 字节图, 全刷 */
void EPD_Display(void);                 /* 把已写入 0x24 RAM 的内容全刷出来 */
void EPD_Sleep(void);                   /* 深度睡眠 (RST 硬复位唤醒) */

/* ---------------- 局刷 API (epd2in13_V2 序列) ----------------
 * 用法:
 *   1. EPD_Init();                 全刷初始化
 *   2. EPD_DisplayBase(白底图);     打底: 0x24+0x26 同图 + 全刷一次
 *   3. EPD_InitPartial();           装局刷 LUT
 *   4. 循环 EPD_DisplayPartial(帧); 只写 0x24 + 0x0C 快刷, 无闪烁
 *
 * 进阶: EPD_DisplayPartialD(old,new) 差分局刷, old/new 都写,
 *       双向翻转都有驱动, 擦除内容不残影 (帧图建议放 CODE 区)
 */
void EPD_InitPartial(void);               /* 切局刷模式: 写 LUT/0x37 + 0xC0 装载 */
void EPD_DisplayBase(const uint8 *img);   /* 局刷基底: old(0x26)=new(0x24)+全刷 */
void EPD_DisplayPartial(const uint8 *img);/* 局刷一帧: 写 0x24 + 0x0C 快刷 */
void EPD_DisplayPartialD(const uint8 *old_img, const uint8 *new_img); /* 差分局刷 */

/* ---------------- 横屏帧缓冲 + 画图 API ----------------
 * 需要时定义 USE_EPD_FRAME=1 启用, 默认关闭以节省 4736B XDATA
 * EPD_Frame 为面板原生布局, 画图用横屏坐标 (x:0-295, y:0-127)
 * 旋转方向由 EPD_ROT_CW 宏决定, 内容倒了改宏即可
 */
#if USE_EPD_FRAME
extern __xdata uint8 EPD_Frame[EPD_IMG_BYTES];   /* 帧缓冲 (面板原生布局) */
void EPD_FrameClear(uint8 color);                  /* 帧清为 color (0xFF=白) */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black); /* 横屏画点: x0-295, y0-127, black:1=黑 */
void EPD_ShowFrame(void);                          /* 局刷显示 EPD_Frame (0x0C) */
#endif

/* ---------------- 兼容 API (epdtest.c / epdpaint.c 调用) ---------------- */
void EpdSendCommand(uint8 cmd);
void EpdSendData(uint8 dat);
void WaitUntilIdle(void);
void Reset(void);
void DelayMs(uint16 ms);
void EpdSetMemoryArea(int x_start, int y_start, int x_end, int y_end);
void EpdSetMemoryPointer(int x, int y);
void EpdClearFrameMemory(uint8 color);
void EpdSetFrameMemory(const uint8 *image_buffer);
void EpdSetFrameMemoryXY(const uint8 *image_buffer, int x, int y, int image_width, int image_height);
void EpdDisplayFrame(void);
void EpdDisplayFramePartial(void);                       /* 局刷: 0x22 0x0C + 0x20 */
void EpdSetFrameMemoryXYOld(const uint8 *image_buffer, int x, int y, int image_width, int image_height); /* 写 0x26 old RAM */
void EpdSetLut(const uint8 *lut);

#endif /* __EPD2IN9_H__ */
