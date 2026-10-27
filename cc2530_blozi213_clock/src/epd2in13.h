/**
 * @file    epd2in13.h
 * @brief   blozi 213 电子价签 - 2.13 寸 EPD 驱动 (104x212) CC2530F256 / IAR
 *
 * ============================================================================
 * [2026-10-25 混合底座]
 *   全刷 = cc2530_RF_link 版原厂路线 (照 yuanma_Blozi_213_cc2530.hex 反汇编,
 *          实机验证黑色正常): 不装外部 LUT, 0x2B(VCOM)+0x0C(booster)+0x18(温度),
 *          0x22=0xC7 走 OTP 出厂波形, 全刷 ~3s。
 *          旧 Waveshare 路线 (0x2C=0x50 VCOM, 无 0x0C booster, 外部 LUT) 在本板
 *          实测全刷偏淡、局刷黑推不满 —— 电压配置与本板 IC 不匹配。
 *   局刷 = EPD_PART_MODE=1 + EPD_PART_OTP=1 (默认): 芯片寄存器保持上电原厂
 *          状态不动, 0x22=0xC0 上电 -> 0x24=帧 / 0x26 见 EPD_PART_OLD_INV
 *          -> 0x22=0x04, 波形走 OTP (与已验证的全刷同一电压体系):
 *          最坏等效 ~3s 全刷 (黑保证正常), 最好 ~1-2s 快刷。
 *          EPD_PART_OTP=0 则走 Waveshare 局刷 LUT + Waveshare 电压
 *          (0x2C/0x03/0x04 补齐) —— 实测两档 WB_B 白转黑都不驱动, 已弃用。
 *   0x26 双重身份: OTP 三色波形下 = 红层 (写 0xFF 整片红, 不显红写 0x00);
 *                  局刷波形下可能是 OLD RAM (DA14585 验证, 写 ~帧)。
 *
 * 引脚: BUSY=P0.4  RST=P0.5  DC=P0.6  CS=P0.7  SCLK=P1.0  SDI=P1.1  PWR=P0.3
 * ============================================================================
 */
#ifndef __EPD2IN13_H__
#define __EPD2IN13_H__

#include <ioCC2530.h>
#include "hal_types.h"          /* uint8 / uint16 / uint32 */

/* ---------------- 面板物理分辨率 (104 x 212) ---------------- */
#define EPD_WIDTH       104      /* 面板 source 方向像素数 */
#define EPD_HEIGHT      212      /* 面板 gate 线数 */
#define EPD_IMG_BYTES   (EPD_WIDTH / 8 * EPD_HEIGHT)   /* 2756 字节 */
#define EPD_LINE_BYTES  (EPD_WIDTH / 8)                /* 13 字节/行 */

/* 横屏逻辑分辨率: 宽 212 x 高 104 (= 系统统一图源尺寸) */
#define EPD_LAND_W      212
#define EPD_LAND_H      104

/* ---------------- 数据入口 (复刻原厂/与帧布局配对) ----------------
 * 1 = 0x11=0x01 (X增 Y减), Y 从 211 递减写入
 *     -> 帧 = 横屏列 x 主序: frame[x*13 + (y>>3)], 帧第 j 组 = 屏列 j
 *     (与 pl_proto.c 的直接索引、clockface 画图 API 全部一致)
 * 0 = 0x11=0x03 (X增 Y增), Y 从 0 递增 (RF 版默认; 本工程不用)
 */
#define EPD_RAM_Y_DESC  1

/* ---------------- 全刷波形来源 ----------------
 * 1 = 外部全刷 LUT (照抄 DA14585 EPD_2IN13_V2_lut_full_update, 触发 0xC4)
 *     三段振荡波形 + repeat=2, 实测 DA14585 上全刷约 1s, 比 OTP 快很多
 * 0 = 芯片内置 OTP 波形 (原厂路线, 触发 0xC7, ~3s, 黑饱满已验证)
 * 注意: 外部 LUT 需配 0x2C=0x50 + 0x03=0x15 + 0x04=41,A8,32 电压轨
 */
#define EPD_FULL_USE_LUT    0

/* ---------------- 刷新模式 ----------------
 * 0 = 每分钟全刷 (~3s, OTP 波形): 黑色/对比度实机验证正常
 * 1 = 局刷 (具体走哪套看 EPD_PART_OTP)
 */
#define EPD_PART_MODE       1

/* ---------------- 局刷路径选择 ----------------
 * 实测结论 (多轮实验闭合):
 *   - LUT 0x80 = 白向电压轨, 0x40 = 黑向电压轨 (方向由电压环境决定);
 *   - 原厂电压体系 (0x2B/0x3D 族, 为 OTP 波形调的) 下: 0x80 轨能翻转
 *     黑->白, 0x40 轨只能巩固已黑、翻不动白->黑 ("黑的能推白,白的推不黑");
 *   - DA14585 同一块屏局刷正常 = Waveshare 电压轨 (0x03=0x15/
 *     0x04=41,A8,32) + VCOM 0x2C=0x28, 且芯片里无 0x2B/0x0C/0x3D 残留。
 * EPD_PART_OTP=0 + EPD_PART_DASTYLE=1 (默认): 软复位清电压 -> DA14585
 *   寄存器组原样 + 原版局刷 LUT + 0x04 触发 —— 同屏已验证环境的完整复刻。
 * AALL/WAVEFULL/partial (legacy) 仅作历史对照。
 */
#define EPD_PART_OTP        0

/* ---------------- DA14585 环境复刻开关 ----------------
 * 1 (默认) = 局刷走 DA14585 同款环境 (软复位 + Waveshare 轨 + 原版 LUT)
 * 0 = legacy 实验路径 (AALL/WAVEFULL/WB_B)
 */
#define EPD_PART_DASTYLE    1

/* ---------------- 局刷触发值 (0x22) [A/B 开关 1] ----------------
 * 依据: HINK-E0213A20x 规格书 0x22 表
 *   合法序列值: 80/01(CK) C0/03(CK+Analog) 91/99(Load LUT M1/M2)
 *               B1/B9(Load Temp+Load LUT M1/M2) C7/CF(Display M1/M2)
 *               F7/FF(Load Temp+Display M1/M2)
 *   位语义: bit3 = 显示模式, 0=Mode1(全刷波形) / 1=Mode2(差分波形)
 *   0xC7 / 0xF7 = Mode1               -> 全刷 (对照用)
 *   0x0C / 0xCF / 0xFF = Mode2        -> 差分(局刷)
 *   0x0C 缺 CK/Analog, 故必须先 0x22=0xC0 上电
 *        —— EPD_PartEnter() 已做, 顺序与 Waveshare 局刷一致
 * 历史: 0x04 (原 DA14585 表值) 按规格书 bit3=0 实为 Mode1,
 *       即此前"局刷"跑的是全刷波形 (与实测 1~3s 等效全刷吻合)
 * A/B 顺序建议: 0x0C -> 0xCF -> 0xFF -> 0xC7(对照)
 */
#define EPD_PART_TRIG       0x0C

/* ---------------- 局刷前置: 边框波形 0x3C [A/B 开关 2] ----------------
 * 依据: 规格书 0x3C  A[7:6] 选 VBD 档:
 *         00=GS Transition  01=Fix Level  10=VCOM  11=HiZ[POR]
 *   0x80 = VBD 固定 VCOM -> 局刷时边框不参与闪烁 (Waveshare 局刷同款)
 *   0x01 = 边框跟随 LUT1 -> 全刷时与白像素一起闪 (原厂全刷用值)
 * 1 (本轮) = 局刷触发前补写 0x3C=EPD_PART_BORDER
 * 0        = 不补 (保持原行为, 对照用)
 */
#define EPD_PART_BORDER_EN  1
#define EPD_PART_BORDER     0x80

/* ---------------- 局刷 RAM Ping-Pong (0x37) 写法 [A/B 开关 3] ----------------
 * 依据: 规格书 0x37 为 10 字节:
 *         A | B~E = WS[7:0]~WS[31:24] | F(F[3:0]=WS[35:32] 模式选择,
 *         F[6] = PingPong for Display Mode 2) | G~J = module ID/waveform ver
 * 1 (本轮) = 10 字节写法, 0x40 落在 F 字节 = PingPong enable
 *            (与 Waveshare 2.13_V3 局刷的 0x37 完全一致)
 * 0        = 原 7 字节写法: 0x40 落在 E 字节 = WS[31:24], PingPong 未使能
 * 注: PingPong 仅对 Display Mode 2 有效 (规格书原文:
 *     "RAM Ping-Pong function is not support for Display Mode 1")
 */
#define EPD_PART_PP10       1

/* ---------------- 局刷 TP0 的 A 时长 (DA 表原值) ----------------
 * DA14585 原版 0x0A; 黑翻转不够就加大 (0x14/0x1E/0x28), 有残影就减小。
 * 注: A 相驱动 LUT1(BW 黑->白), 已实测清晰, 一般不用动。
 */
#define EPD_PART_ALEN       0x0A

/* ---------------- 局刷表用全刷波形还是局刷波形 ----------------
 * 0 (本轮) = Waveshare 局刷表 + TP0.B 补时长 (见 WB_B):
 *            白像素 A10 白向 + 黑像素 B10 黑向, 各一次到位, 快刷。
 * 1 = 全刷波形表 (黑路径混着白向 A 相, 淡; 对照用)
 */
#define EPD_PART_WAVEFULL   0

/* ---------------- 局刷时 0x26 写什么 ----------------
 * 外部 LUT 波形下 0x26 = OLD RAM (DA14585 验证): 必须 ~帧 建 OLD 基线。
 * OTP 波形下 0x26 = 红层 (写 0xFF 发红) —— 仅 EPD_PART_OTP=1 时相关。
 * 1 (默认) = ~帧 (DA14585 局刷语义, 本实验用这个)
 * 0        = EPD_RedNone(0x00)
 */
#define EPD_PART_OLD_INV    1

/* ---------------- 局刷表 WB(白->黑)相时长 ----------------
 * 1 (本轮) = TP0 的 B 相补 0x0A: 黑像素走 B10 黑向脉冲 (B 相=new黑黑向实锤)。
 * 黑不够实就加大 (0x14/0x1E), 有残影就减小。
 * 0 = 原厂原样 (B 相 0) —— 低压时代实测渐白; 原厂电压下未测。
 */
#define EPD_PART_WB_B       1

/* ---------------- 全刷表的 TP repeat (仅 AALL=0/WAVEFULL=1 时有效) ----------------
 * 全刷表原值 0x02 -> 黑路径约 1s, 实测偏淡。repeat 与时长成正比。
 */
#define EPD_WAVE_REPEAT     0x0A

/* ---------------- 等 BUSY 时的心跳灯 ---------------- */
#define EPD_BUSY_HEARTBEAT  1
#define EPD_BUSY_HB_PIN     P0_0     /* 板载 LED (P0.0-0.2) */

/* ---------------- BUSY 极性 ---------------- */
#define EPD_BUSY        P0_4         /* 高 = 忙 */

/* ---------------- 板上引脚 ---------------- */
#define EPD_RST         P0_5
#define EPD_DC          P0_6         /* 0=命令 1=数据 */
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

/* ---------------- 红层/OLD 层 (0x26) 电平 ----------------
 * OTP 全刷路径: 0x26=红层, 写 0x00 不显示红 (写 0xFF 整片红, 实测)
 * 局刷路径:     0x26=OLD RAM, 写 ~帧 (驱动内部处理, 与本值无关) */
extern uint8 EPD_RedNone;

/* ---------------- 延时 ---------------- */
void   EPD_DelayMS(uint16 ms);       /* 毫秒延时 (halMcuWaitMs + 内部记账) */
uint16 EPD_DrainOpMs(void);          /* 取走并清零"驱动内部已花掉的毫秒数" */

/* ---------------- API ---------------- */
void EPD_GpioInit(void);
void EPD_PowerOn(void);
void EPD_PowerOff(void);
void EPD_Init(void);                     /* 上电+复位+原厂 init+OTP 清底 (只跑一次) */
void EPD_Fill(uint8 color);              /* 全屏填充并全刷: 0xFF=白 0x00=黑 */
void EPD_DisplayImage(const uint8 *img); /* 2756B 图, 写 0x24 + 全刷 */
void EPD_Display(void);                  /* 0x22=0xC7 + 0x20 + 等刷完 (~3s) */
void EPD_Sleep(void);                    /* 深睡; 之后首次显示会自动重新 init */

void EPD_SendCommand(uint8 cmd);
void EPD_SendData(uint8 dat);

/* ---------------- 横屏帧缓冲与画图 API ---------------- */
extern __xdata uint8 EPD_Frame[EPD_IMG_BYTES];
void EPD_FrameClear(uint8 color);                  /* 帧清为 color (0xFF=白) */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black); /* 横屏画点: x0-211, y0-103, black:1=黑 */
void EPD_ShowFrame(void);                          /* 全刷显示 EPD_Frame */
void EPD_ShowFrameFull(void);                      /* 写 0x24/0x26 + OTP 全刷 */
void EPD_ShowFramePart(void);                      /* MODE=0 同全刷; MODE=1 局刷实验 */

/* 画图 API (横屏坐标) */
void EPD_DrawLine(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawFilledRect(int x0, int y0, int x1, int y1, uint8 black);
void EPD_DrawCircle(int xc, int yc, int r, uint8 black);
void EPD_DrawFilledCircle(int xc, int yc, int r, uint8 black);

#endif /* __EPD2IN13_H__ */
