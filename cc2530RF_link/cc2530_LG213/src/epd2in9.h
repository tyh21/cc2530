/**
 * @file    epd2in9.h
 * @brief   2.13 寸三色 EPD 驱动 (UC8276 系控制器, WFT0213CZ16 裸屏
 *          = 微雪 "2.13inch e-Paper (B)", LG 电子价签板 CC2530F256)
 *          bit-bang SPI, 刷新序列 = 微雪 EPD_2in13b_V3 (实测可点亮)
 *
 * 注: 文件名沿用工程清单里的 epd2in9.c/h, 实际驱动对象为 2.13 寸三色屏
 *
 * 驱动序列来源:
 *   全刷/休眠: 微雪官方 EPD_2in13b_V3.c (jsDelivr, 逐字节照抄)
 *   引脚/电源: 原厂固件逆向 (yuanma_LG213.hex) + 万用表实测
 *
 * 三色屏特性 (与 2.9 寸黑白屏的差异):
 *   - 无局刷 (官方 b_V3 驱动无 partial API), 每次刷新都是全刷, 耗时 ~15s
 *   - 双 RAM: 0x10 = 黑层, 0x13 = 红层; 0xFF=白, 0x00=黑/红
 *   - BUSY 极性与 SSD1680 相同: 空闲=高 (官方 ReadBusy 发 0x71 后等拉高)
 *   - 红层全 0xFF = 不显示红色, 帧缓冲只存黑层即可 (省 2.7KB RAM)
 *
 * 电源控制 (P0.7 -> EPD 供电 MOS, 高电平有效, 与 290 板同款硬件):
 *   LG213 固件为另一 SDK 构建, P0.7 无位操作痕迹,
 *   电源控制依据: 同族 290 板固件铁证 (0x11735/0x11766/0x11B3D) + 用户实测
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

/* 面板物理分辨率 (不可变): 212 条 gate 线 x 104 个 source 位
 * 帧缓冲按面板原生布局: 每行 13 字节 x 212 行 = 2756 字节
 * (官方 0x61 分辨率参数: 0x68, 0x00, 0xD4 = 104 x 212) */
#define EPD_WIDTH       104      /* 面板 source 方向像素数 */
#define EPD_HEIGHT      212      /* 面板 gate 线数 */
#define EPD_IMG_BYTES   (EPD_WIDTH / 8 * EPD_HEIGHT)   /* 2756 字节 */
#define EPD_LINE_BYTES  (EPD_WIDTH / 8)                /* 13 字节/行 */

/* 横屏逻辑分辨率 (软件旋转): 宽 212 x 高 104
 * 映射公式与微雪官方示例 Paint 旋转 270° 等价 */
#define EPD_LAND_W      212
#define EPD_LAND_H      104

/* 旋转方向: 1 = 顺时针 90° (默认, 同官方 270° 摆横), 0 = 逆时针 90°
 * 如果横屏后发现内容是倒的 (转了 180°), 把这里改成 0 */
#define EPD_ROT_CW      1

/* LG 价签实测引脚映射 (万用表实测 + 213/290 固件逆向验证)
 * 注: 固件中 RST=P1.1 / BUSY=P1.2 (213 固件 0x02B908 复位脉冲簇确认 RST=P1.1),
 *     与实测表 (BUSY=P1.1/RST=P1.2) 对调; CC2530 脚 8=P1.2 与脚 9=P1.1
 *     相邻易数错, 以固件为准 (与 290 板结论一致)。
 *     如果屏始终不 init, 把下面 EPD_BUSY/EPD_RST 互换再试。
 */
#define EPD_BUSY        P1_2     /* 输入, 空闲=高, 忙=低 (官方 ReadBusy 等拉高, 同 SSD1680 极性) */
#define EPD_RST         P1_1     /* 硬复位 (213 固件 0x02B908 复位脉冲) */
#define EPD_DC          P0_2     /* 0=命令 1=数据 */
#define EPD_CS          P0_4     /* 片选, 低有效 */
#define EPD_SCLK        P0_5     /* 290 板原固件走 USART0 硬件 SPI Alt1 SCK, bit-bang 同引脚等价 */
#define EPD_SDI         P0_3     /* 290 板原固件走 USART0 硬件 SPI Alt1 MOSI */

/* ---------------- 电源控制: P0.7 -> EPD 供电 MOS, 高电平=上电 ---------------- */
#define EPD_PWR             P0_7
#define EPD_PWR_ON_LVL      1
#define EPD_PWR_OFF_LVL     0

/* ---------------- 全刷 API (三色屏无局刷) ---------------- */
void EPD_GpioInit(void);
void EPD_PowerOn(void);                /* P0.7 上电 */
void EPD_PowerOff(void);               /* P0.7 断电 */
void EPD_Init(void);                    /* 上电初始化 (微雪 2in13b_V3 序列) */
void EPD_Fill(uint8 color);             /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img);/* 2756 字节黑层图, 全刷 (红层不显示) */
void EPD_DisplayImageRY(const uint8 *black, const uint8 *red); /* 黑+红双层全刷, 传 NULL 该层=白 */
void EPD_Sleep(void);                   /* 深度睡眠 + 断电 (P0.7 拉低) */

/* ---------------- 横屏帧缓冲 + 画图 API ----------------
 * EPD_Frame 为面板原生布局 (黑层), 画图用横屏坐标 (x:0-211, y:0-103)
 * 旋转方向由 EPD_ROT_CW 宏决定, 内容倒了改宏即可
 * 红色内容: 把红层图 (0x00=红) 通过 EPD_DisplayImageRY() 的 red 参数送显
 */
extern __xdata uint8 EPD_Frame[EPD_IMG_BYTES];   /* 帧缓冲 (面板原生布局) */
void EPD_FrameClear(uint8 color);                  /* 帧清为 color (0xFF=白) */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black); /* 横屏画点: x0-211, y0-103, black:1=黑 */
void EPD_ShowFrame(void);                          /* 全刷显示 EPD_Frame */
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
