/**
 * @file    epd2in9.c
 * @brief   2.9 寸 EPD 驱动实现 (SSD1680 系, bit-bang SPI)
 *
 * 引脚: BUSY=P0.4  RST=P0.5  DC=P0.6  CS=P0.7  SCLK=P1.0  SDI=P1.1
 * 命令序列来源: blozi_290 价签原厂固件逆向 (yuanma1.hex)
 */
#include "epd2in9.h"

/* ---------------- 内部延时 (32MHz 主频校准) ---------------- */
static void EPD_DelayMS(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- GPIO 初始化 ---------------- */
void EPD_GpioInit(void)
{
    /* P0.4-P0.7 设为通用 IO */
    P0SEL &= ~0xF0;
    /* P0.5/6/7 输出, P0.4 输入(BUSY) */
    P0DIR |=  0xE0;
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

/* ---------------- 上电初始化 ----------------
 * 序列照抄原固件 0x1142 处, 仅 data entry 由 0x01(原) 改 0x03,
 * 使图片数组方向与常规工具(Waveshare/Image2Lcd)一致: 从(0,0)逐行
 */
void EPD_Init(void)
{
    EPD_GpioInit();

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

    EPD_SendCommand(0x22);           /* Display update seq: 全刷(OTP波形) */
    EPD_SendData(0xB1);
    EPD_SendCommand(0x20);           /* Master activate */
    EPD_WaitBusy(5000);              /* 这次全刷把屏清成统一底色 */
}

/* ---------------- 写 4736 字节到 0x24 RAM ---------------- */
static void EPD_WriteRAMBytes(const uint8 *img)
{
    uint16 i;

    EPD_SendCommand(0x4E);           /* RAM X pointer = 0 */
    EPD_SendData(0x00);

    EPD_SendCommand(0x4F);           /* RAM Y pointer = 0 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);

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

/* ---------------- 刷新 (原固件 0x1100 序列) ---------------- */
void EPD_Display(void)
{
    EPD_SendCommand(0x21);           /* 原固件带此命令, 保留 */
    EPD_SendData(0x40);

    EPD_SendCommand(0x22);
    EPD_SendData(0xC7);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(8000);
}

/* ---------------- 全屏填充并发刷 ---------------- */
void EPD_Fill(uint8 color)           /* 0xFF=白  0x00=黑 */
{
    uint16 i;

    EPD_SendCommand(0x4E);
    EPD_SendData(0x00);
    EPD_SendCommand(0x4F);
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x24);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(color);

    EPD_Display();
}

/* ---------------- 显示一幅图 ---------------- */
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
