/**
 * @file    epd2in9.c
 * @brief   2.9 寸 EPD 驱动实现 (SSD1680 系, bit-bang SPI)
 *
 * 引脚: BUSY=P0.4  RST=P0.5  DC=P0.6  CS=P0.7  SCLK=P1.0  SDI=P1.1
 *       PWR=P0.3  (MOS 管电源开关, 低有效)
 * 全刷序列: blozi_290 价签原厂固件逆向 (yuanma1.hex)
 * 局刷序列: Waveshare epd2in13_V2 (板上实测兼容)
 */
#include "epd2in9.h"

/* ---------------- 局刷 LUT (epd2in13_V2, 70 字节) ----------------
 * LUT0 BB / LUT1 BW / LUT2 WB / LUT3 WW / LUT4 VCOM 各 7 字节,
 * TP0..TP6 各 5 字节 (0x32 只收这 70 字节)
 */
static const uint8 lut_partial[70] = {
    /* LUT0: BB */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* LUT1: BW */
    0x80,0x00,0x00,0x00,0x00,0x00,0x00,
    /* LUT2: WB */
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,
    /* LUT3: WW */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    /* LUT4: VCOM */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    /* TP0..TP6 */
    0x0A,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
};

/* ---------------- 全刷 LUT (epd2in13_V2 full update, 76 字节) ----------------
 * 前 70 字节写 0x32; 后 6 字节 [70..75] 分别给 0x03/0x04(4B)/0x3A/0x3B
 * 局刷模式跑过之后 0x32 里驻留的是局刷 LUT, 全刷前必须用本表覆盖,
 * 否则 0x22=0xC7 仍按局刷 LUT 驱动 (不闪)。
 */
static const uint8 lut_full[76] = {
    /* LUT0: BB */
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    /* LUT1: BW */
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    /* LUT2: WB */
    0x80,0x60,0x40,0x00,0x00,0x00,0x00,
    /* LUT3: WW */
    0x10,0x60,0x20,0x00,0x00,0x00,0x00,
    /* LUT4: VCOM */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,

    /* TP0..TP6 */
    0x03,0x03,0x00,0x00,0x02,
    0x09,0x09,0x00,0x00,0x02,
    0x03,0x03,0x00,0x00,0x02,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,

    /* [70..75]: 0x03 / 0x04 / 0x3A / 0x3B 参数 */
    0x15,0x41,0xA8,0x32,0x30,0x0A,
};

/* ---------------- 内部延时 (32MHz 主频校准) ---------------- */
static void EPD_DelayMS(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 电源控制: P0.3 -> MOS 管开关 ---------------- */
void EPD_PowerOn(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;         /* 断电位置 */
    EPD_DelayMS(500);                  /* 放电等待 */
    EPD_PWR = EPD_PWR_ON_LVL;          /* 上电 */
    EPD_DelayMS(500);                  /* 上电稳定 */
}

void EPD_PowerOff(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;
}

/* ---------------- GPIO 初始化 ---------------- */
void EPD_GpioInit(void)
{
    /* P0.3-P0.7 设为通用 IO */
    P0SEL &= ~0xF8;
    /* P0.5/6/7 输出, P0.4 输入(BUSY), P0.3 输出(PWR) */
    P0DIR |=  0xE8;
    P0DIR &= ~0x10;

    /* P1.0/P1.1 设为通用 IO 输出 (SCLK/SDI) */
    P1SEL &= ~0x03;
    P1DIR |=  0x03;

    /* 初始电平 */
    EPD_CS   = 1;
    EPD_DC   = 1;
    EPD_SCLK = 0;
    EPD_SDI  = 0;
    EPD_RST  = 1;
    EPD_PWR  = EPD_PWR_OFF_LVL;        /* 先保持断电, 由 EPD_PowerOn() 上电 */
}

/* ---------------- bit-bang SPI ---------------- */
static void EPD_SendByte(uint8 b)
{
    uint8 i;
    for (i = 0; i < 8; i++)
    {
        EPD_SCLK = 0;
        EPD_SDI  = (b & 0x80) ? 1 : 0;
        b <<= 1;
        EPD_SCLK = 1;      /* 上升沿锁存 */
    }
    EPD_SCLK = 0;
}

void EPD_SendCommand(uint8 cmd)
{
    EPD_DC = 0;
    EPD_CS = 0;
    EPD_SendByte(cmd);
    EPD_CS = 1;
    EPD_DC = 1;
}

void EPD_SendData(uint8 dat)
{
    EPD_DC = 1;
    EPD_CS = 0;
    EPD_SendByte(dat);
    EPD_CS = 1;
}

/* ---------------- BUSY 等待 (P0.4 高=忙, 带超时) ---------------- */
static uint8 EPD_WaitBusy(uint16 timeout_ms)
{
    while (EPD_BUSY)
    {
        if (timeout_ms == 0)
            return 1;               /* 超时 */
        EPD_DelayMS(10);
        if (timeout_ms >= 10)
            timeout_ms -= 10;
        else
            timeout_ms = 0;
    }
    EPD_DelayMS(10);
    return 0;
}

/* ---------------- 硬复位 ---------------- */
static void EPD_Reset(void)
{
    EPD_RST = 1;
    EPD_DelayMS(20);
    EPD_RST = 0;
    EPD_DelayMS(10);
    EPD_RST = 1;
    EPD_DelayMS(20);
}

/* ---------------- 全屏指针回零 ---------------- */
static void EPD_SetPtr0(void)
{
    EPD_SendCommand(0x4E);           /* RAM X pointer = 0 */
    EPD_SendData(0x00);

    EPD_SendCommand(0x4F);           /* RAM Y pointer = 0 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
}

/* ---------------- 上电初始化 (OTP 全刷波形) ----------------
 * 序列照抄原固件 0x1142 处, 仅 data entry 由 0x01(原) 改 0x03,
 * 使图片数组方向与常规工具(Waveshare/Image2Lcd)一致: 从(0,0)逐行
 */
void EPD_Init(void)
{
    EPD_GpioInit();
    EPD_PowerOn();                    /* P0.3 上电 (原固件时序) */
    EPD_Reset();
    EPD_WaitBusy(3000);

    EPD_SendCommand(0x12);           /* SWRESET */
    EPD_WaitBusy(3000);

    EPD_SendCommand(0x74);           /* 内部温度传感器写 */
    EPD_SendData(0x54);

    EPD_SendCommand(0x7E);           /* 外部温度传感器读 */
    EPD_SendData(0x3B);

    EPD_SendCommand(0x01);           /* Driver output control: 296 行 */
    EPD_SendData(0x27);
    EPD_SendData(0x01);
    EPD_SendData(0x00);

    EPD_SendCommand(0x11);           /* Data entry mode: X增 Y增 */
    EPD_SendData(0x03);

    EPD_SendCommand(0x44);           /* RAM X window: 0 ~ 0x0F (128列) */
    EPD_SendData(0x00);
    EPD_SendData(0x0F);

    EPD_SendCommand(0x45);           /* RAM Y window: 0 ~ 295 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x27);
    EPD_SendData(0x01);

    EPD_SendCommand(0x3C);           /* Border waveform */
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);           /* RTC control (原固件参数) */
    EPD_SendData(0x80);

    EPD_SendCommand(0x22);           /* Display update seq: 全刷(OTP) */
    EPD_SendData(0xB1);
    EPD_SendCommand(0x20);           /* Master activate */
    EPD_WaitBusy(5000);              /* 这次全刷把屏清成统一底色 */
}

/* ---------------- 写 4736 字节到 0x24 RAM ---------------- */
static void EPD_WriteRAMBytes(const uint8 *img)
{
    uint16 i;

    EPD_SetPtr0();
    EPD_SendCommand(0x24);           /* 写 B/W RAM */
    if (img == (void *)0)
    {
        for (i = 0; i < EPD_IMG_BYTES; i++)
            EPD_SendData(0xFF);
    }
    else
    {
        for (i = 0; i < EPD_IMG_BYTES; i++)
            EPD_SendData(img[i]);
    }
}

/* ---------------- 装载全刷 LUT (epd2in13_V2 FULL 分支) ----------------
 * 0x32 写 70 字节全刷波形; 0x03/0x04/0x3A/0x3B 写额外 6 字节参数
 * 必须在对屏做 0x22=0xC7 全刷之前调用, 否则驻留的局刷 LUT 会让全刷不闪
 */
static void EPD_LoadFullLUT(void)
{
    uint8 i;

    EPD_SendCommand(0x03);
    EPD_SendData(lut_full[70]);

    EPD_SendCommand(0x04);           /* 4 字节 */
    EPD_SendData(lut_full[71]);
    EPD_SendData(lut_full[72]);
    EPD_SendData(lut_full[73]);

    EPD_SendCommand(0x3A);           /* Dummy line */
    EPD_SendData(lut_full[74]);

    EPD_SendCommand(0x3B);           /* Gate time */
    EPD_SendData(lut_full[75]);

    EPD_SendCommand(0x32);           /* 70 字节波形表 */
    for (i = 0; i < 70; i++)
        EPD_SendData(lut_full[i]);
}

/* ---------------- 全刷刷新 (原固件 0x1100 序列 + 全刷 LUT) ---------------- */
void EPD_Display(void)
{
    EPD_LoadFullLUT();

    EPD_SendCommand(0x21);           /* 原固件带此命令, 保留 */
    EPD_SendData(0x40);

    EPD_SendCommand(0x22);
    EPD_SendData(0xC7);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(8000);
}

/* ---------------- 全屏填充并全刷 ---------------- */
void EPD_Fill(uint8 color)           /* 0xFF=白 0x00=黑 */
{
    uint16 i;

    EPD_SetPtr0();
    EPD_SendCommand(0x24);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(color);

    EPD_Display();
}

/* ---------------- 显示一幅图 (全刷) ---------------- */
void EPD_DisplayImage(const uint8 *img)
{
    EPD_WriteRAMBytes(img);
    EPD_Display();
}

/* ---------------- 深度睡眠 ---------------- */
void EPD_Sleep(void)
{
    EPD_SendCommand(0x10);
    EPD_SendData(0x01);
}

/* ==================================================================
 * 局刷 (epd2in13_V2 序列, 板上实测兼容)
 * ================================================================== */

/* ---------------- 装载局刷 LUT (epd2in13_V2 PART 分支) ----------------
 * 切回局刷模式: 写 70 字节局刷 LUT + 部分窗 + 0xC0 装载 + 边界波形
 * 全刷(EPD_Display/EPD_DisplayBase)后会再调用本函数, 恢复局刷 LUT
 */
static void EPD_LoadPartialLUT(void)
{
    uint8 i;

    /* VCOM (epd2in13_V2 PART 分支) */
    EPD_SendCommand(0x2C);
    EPD_SendData(0x26);
    EPD_WaitBusy(3000);

    /* 写局刷 LUT: 70 字节 */
    EPD_SendCommand(0x32);
    for (i = 0; i < 70; i++)
        EPD_SendData(lut_partial[i]);

    /* 部分窗口寄存器 (全屏范围) */
    EPD_SendCommand(0x37);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x40);             /* gate 扫描使能 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    /* 装载 LUT 并激活 */
    EPD_SendCommand(0x22);
    EPD_SendData(0xC0);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);

    /* 边界波形: 局刷时边框保持 */
    EPD_SendCommand(0x3C);
    EPD_SendData(0x01);
}

/* ---------------- 切局刷模式: 装 LUT + 部分窗 + 装载 ---------------- */
void EPD_InitPartial(void)
{
    EPD_LoadPartialLUT();
}

/* ---------------- 局刷基底: old=新图 + 全刷一次 ----------------
 * 0x26 是 old 数据 RAM, 局刷波形按 new(0x24) 与 old(0x26) 差异驱动
 */
void EPD_DisplayBase(const uint8 *img)
{
    uint16 i;

    EPD_SetPtr0();
    EPD_SendCommand(0x24);           /* new RAM */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : 0xFF);

    EPD_SetPtr0();
    EPD_SendCommand(0x26);           /* old RAM (局刷波形参考) */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : 0xFF);

    EPD_LoadFullLUT();               /* 装全刷 LUT (会闪) */
    EPD_SendCommand(0x22);           /* 全刷一次 */
    EPD_SendData(0xC7);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(8000);

    EPD_LoadPartialLUT();            /* 切回局刷 LUT, 供后续局刷 */
}

/* ---------------- 局刷一帧 (Waveshare 原味: 只写 new) ----------------
 * 适合只加黑不擦除的内容; 擦除旧内容会有残影, 需定期全刷
 */
void EPD_DisplayPartial(const uint8 *img)
{
    uint16 i;

    EPD_SetPtr0();
    EPD_SendCommand(0x24);           /* new RAM */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : 0xFF);

    EPD_SendCommand(0x22);           /* 局刷快刷 (无闪烁) */
    EPD_SendData(0x0C);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);
}

/* ---------------- 局刷一帧 (差分: old+new 都写) ----------------
 * old = 屏幕当前内容, new = 目标内容, 波形只驱动变化像素,
 * 双向翻转都有驱动, 无残影; 图放 CODE 区不占 RAM
 */
void EPD_DisplayPartialD(const uint8 *old_img, const uint8 *new_img)
{
    uint16 i;

    EPD_SetPtr0();
    EPD_SendCommand(0x26);           /* old RAM = 屏幕当前内容 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(old_img ? old_img[i] : 0xFF);

    EPD_SetPtr0();
    EPD_SendCommand(0x24);           /* new RAM = 目标内容 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(new_img ? new_img[i] : 0xFF);

    EPD_SendCommand(0x22);           /* 局刷快刷 (无闪烁) */
    EPD_SendData(0x0C);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);
}

/* ==================================================================
 * 横屏帧缓冲 + 画图 API
 * 帧缓冲 EPD_Frame 为面板原生布局 (16B x 296 行),
 * 画图用横屏坐标, 内部做 90° 旋转映射
 * ================================================================== */
__xdata uint8 EPD_Frame[EPD_IMG_BYTES];

void EPD_FrameClear(uint8 color)
{
    uint16 i;
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_Frame[i] = color;
}

/* 横屏画点
 * 输入: x = 0..295 (横向), y = 0..127 (纵向), black = 1 画黑
 * 映射 (EPD_ROT_CW=1, 屏幕顺时针转 90° 摆横):
 *   横屏 (x, y) -> 面板 (src, gate) = (y, EPD_LAND_W-1-x)
 *   面板布局: 帧[gate] 的 bit(7-src%8), 字节 = frame[gate*16 + src/8]
 */
void EPD_SetPixel(uint16 x, uint8 y, uint8 black)
{
    uint16 gate;
    uint8  src;
    uint16 idx;
    uint8  mask;

    if (x >= EPD_LAND_W || y >= EPD_LAND_H)
        return;

#if EPD_ROT_CW
    src  = y;
    gate = (uint16)(EPD_LAND_W - 1 - x);
#else
    src  = (uint8)(EPD_LAND_H - 1 - y);
    gate = x;
#endif

    idx = (uint16)(gate * EPD_LINE_BYTES) + (src >> 3);
    mask = (uint8)(0x80 >> (src & 0x07));
    if (black)
        EPD_Frame[idx] &= (uint8)~mask;   /* 0=黑 */
    else
        EPD_Frame[idx] |= mask;           /* 1=白 */
}

/* 局刷显示 EPD_Frame (0x0C 快刷) */
void EPD_ShowFrame(void)
{
    EPD_DisplayPartial(EPD_Frame);
}

