/**
 * @file    epd2in9.h
 * @brief   blozi 213 电子价签 - 2.13 寸三色 EPD 驱动 (HINK-E0213A20-A2, 212x104)
 *          bit-bang SPI, CC2530F256
 *
 * [2026-10-24 实测迭代记录]
 *   ① 原驱动是从 290 板 (2.9 寸 128x296 单色) 整体照抄的 -> 与本板不符, 不显示
 *   ② 按本板原厂固件 yuanma_Blozi_213_cc2530.hex 重写 (104x212, SSD16xx 系命令:
 *      0x01/0x11/0x44/0x45/0x24/0x26/0x4E/0x4F/0x22/0x20/0x10) -> 实机刷白 OK
 *      灰屏那一次是 FPC 没插好 (命令被吃掉), 不是序列问题
 *   ③ 本板屏是**红黑白三色屏**: 红层(0x26)写 0xFF 会整片变红, 故红层电平
 *      改为 0x00 = 不显示红 (见 EPD_RedNone)。屏幕主控为 SSD16xx 系 (接受上述
 *      命令序列); 佳显 GDEH0213Z19 / UC8151D 那套 0x00/0x04/0x61/0x10/0x13/0x12
 *      只作备用参考, 本工程未启用
 *
 * 面板/引脚:
 *   分辨率 104 x 212 (13B/行 x 212 行 = 2756B), 横屏 212x104
 *   帧缓冲布局与系统统一图源 212x104 逐位一致 (pl_proto.c 的 pl_frame_write_213)
 *   BUSY=P0.4 (高=忙, 等低)  RST=P0.5  DC=P0.6  CS=P0.7
 *   SCLK=P1.0  SDI=P1.1  PWR=P0.3 (MOS 管电源开关, 低有效)
 *   与 290 板 EPD 引脚完全一致 (同款 PCB 家族)
 */
#ifndef __EPD2IN9_H__
#define __EPD2IN9_H__

#include <ioCC2530.h>
#include "hal_types.h"          /* uint8 / uint16 */

/* ---------------- 面板物理分辨率 (104 x 212) ---------------- */
#define EPD_WIDTH       104      /* 面板 source 方向像素数 */
#define EPD_HEIGHT      212      /* 面板 gate 线数 */
#define EPD_IMG_BYTES   (EPD_WIDTH / 8 * EPD_HEIGHT)   /* 2756 字节 */
#define EPD_LINE_BYTES  (EPD_WIDTH / 8)                /* 13 字节/行 */

/* 横屏逻辑分辨率 (软件旋转): 宽 212 x 高 104 (= 系统统一图源尺寸) */
#define EPD_LAND_W      212
#define EPD_LAND_H      104

/* ---------------- 方向微调 (实机若方向不对, 只改这两个开关) ----------------
 * EPD_ROT_CW    : 1 = 顺时针 90° 摆横 (默认, 与 LG213/统一图源一致)
 *                 0 = 逆时针 90° (整屏内容 180° 反过来时改这个)
 * EPD_RAM_Y_DESC: 0 = 帧第 j 行 -> RAM 行 j (默认, 数据入口 X+ Y+)
 *                 1 = 复刻原厂固件顺序 (RAM 从 Y=211 递减, 数据入口 X+ Y-)
 *                 -> 图像左右镜像 (文字左右反了) 时把它改成 1
 */
#define EPD_ROT_CW      1
#define EPD_RAM_Y_DESC  0

/* ---------------- BUSY 极性 ----------------
 * 本板(SSD16xx 系): 高电平 = 忙, 等它拉低 (原厂固件 0x0BBE 同款)
 */
#define EPD_BUSY_HIGH_IS_BUSY   1

/* ---------------- 板上实测引脚映射 (290 板同款硬件) ---------------- */
#define EPD_BUSY        P0_4
#define EPD_RST         P0_5
#define EPD_DC          P0_6     /* 0=命令 1=数据 */
#define EPD_CS          P0_7
#define EPD_SCLK        P1_0
#define EPD_SDI         P1_1

/* ---------------- 电源控制: P0.3 -> MOS 管开关 (低有效) ---------------- */
#define EPD_PWR             P0_3
#define EPD_PWR_ACTIVE_LOW  1

#if EPD_PWR_ACTIVE_LOW
#define EPD_PWR_ON_LVL      0
#define EPD_PWR_OFF_LVL     1
#else
#define EPD_PWR_ON_LVL      1
#define EPD_PWR_OFF_LVL     0
#endif

/* ---------------- 红层(0x26)电平 ----------------
 * 实机实测: 写 0xFF 整屏变红 -> 本板用 0x00 表示"不显示红"
 * 若刷出来整屏红 / 红黑颠倒, 把它改成 0xFF (自检固件可 A/B 对照)
 */
extern uint8 EPD_RedNone;

/* ---------------- API ---------------- */
void EPD_GpioInit(void);                 /* GPIO 方向/空闲电平 (含 P0.0-0.2 LED 脚) */
void EPD_PowerOn(void);                  /* P0.3 上电: 断 500ms -> 通 500ms */
void EPD_PowerOff(void);
void EPD_Init(void);                     /* 上电 + 复位 + 原厂 init 序列 (含清底色) */
void EPD_Fill(uint8 color);              /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img); /* 2756B 黑白图, 全刷 (红层=EPD_RedNone) */
void EPD_Display(void);                  /* 把已写入的 RAM 全刷出来 */
void EPD_Sleep(void);                    /* 深度睡眠 (只发 0x10 0x01, 不断电) */

void EPD_SendCommand(uint8 cmd);         /* 非 static: 供调试扩展 */
void EPD_SendData(uint8 dat);

/* ---------------- 横屏帧缓冲与画图 API ---------------- */
extern __xdata uint8 EPD_Frame[EPD_IMG_BYTES];
void EPD_FrameClear(uint8 color);                  /* 帧清为 color (0xFF=白) */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black); /* 横屏画点: x0-211, y0-103, black:1=黑 */
void EPD_ShowFrame(void);                          /* 全刷显示 EPD_Frame */
/* 画图 API (横屏坐标) */
void EPD_DrawLine(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawFilledRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawCircle(int xc, int yc, int r, uint8 black);
void EPD_DrawFilledCircle(int xc, int yc, int r, uint8 black);

#endif /* __EPD2IN9_H__ */
