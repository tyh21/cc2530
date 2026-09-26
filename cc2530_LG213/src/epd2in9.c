/**
 * @file    epd2in9.c
 * @brief   2.13 寸三色 EPD 驱动实现 (UC8276 系, WFT0213CZ16, LG 价签板, bit-bang SPI)
 *
 * 引脚 (实测 + 固件逆向双重验证, 与 290 板同款硬件):
 *   BUSY=P1.2  RST=P1.1  DC=P0.2  CS=P0.4  SCLK=P0.5  SDI=P0.3
 *   PWR=P0.7 (EPD 供电 MOS, 高电平上电)
 *
 * 刷新序列来源: 微雪官方 EPD_2in13b_V3.c (V1.0, 2020-04-13, 用户实测可点亮)
 *   Reset: 高 200ms -> 低 1ms -> 高 200ms (单脉冲)
 *   Init:  0x04+等忙 -> 0x00 [0x0F,0x89] -> 0x61 [0x68,0x00,0xD4] -> 0x50 [0x77]
 *   刷新:  0x10 黑层 + 0x13 红层 + 0x12 + 等 busy (双 RAM, 0xFF=白)
 *   BUSY:  发 0x71 后查询, 引脚高=空闲 (官方 ReadBusy 同款, 与 SSD1680 极性一致)
 *   休眠:  0x50 0xF7 -> 0x02+等忙 -> 0x07 0xA5 (追加 P0.7 断电)
 *
 * 三色屏无局刷, 删除 2.9 寸版的局刷 LUT/部分窗代码; 全刷一次 ~15s
 */
#include "epd2in9.h"
#include "fonts.h"

/* ---------------- 内部延时 (32MHz 主频校准) ---------------- */
static void EPD_DelayMS(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 电源控制: P0.7 -> MOS 管开关 (高电平上电) ---------------- */
void EPD_PowerOn(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;         /* 先确保断电 */
    EPD_DelayMS(100);
    EPD_PWR = EPD_PWR_ON_LVL;          /* 上电 */
    EPD_DelayMS(100);                  /* 供电稳定 */
}

void EPD_PowerOff(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;
}

/* ---------------- GPIO 初始化 ---------------- */
void EPD_GpioInit(void)
{
    /* P0.2/3/4/5 设为通用 IO (DC/SDI/CS/SCLK), P0.7 = PWR */
    P0SEL &= ~0xBC;
    P0DIR |=  0xBC;                    /* P0.2,3,4,5,7 输出 */

    /* P1.1 输出 (RST), P1.2 输入 (BUSY) */
    P1SEL &= ~0x06;
    P1DIR |=  0x02;
    P1DIR &= ~0x04;

    /* 初始电平: 信号空闲态, 电源先关 */
    EPD_CS   = 1;
    EPD_DC   = 1;
    EPD_SCLK = 0;
    EPD_SDI  = 0;
    EPD_RST  = 1;
    EPD_PWR  = EPD_PWR_OFF_LVL;        /* 由 EPD_PowerOn() 上电 */
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

/* ---------------- BUSY 等待 (官方 ReadBusy 同款: 发 0x71 后查询) ----------------
 * 官方 do { 0x71; busy = !pin; } while (busy) => 等引脚拉高, 高=空闲;
 * 与 SSD1680 极性一致。加超时防死等
 */
static uint8 EPD_WaitBusy(uint16 timeout_ms)
{
    uint16 polls = timeout_ms / 20U;
    uint8  busy;

    do
    {
        EPD_SendCommand(0x71);         /* 官方 ReadBusy 同款 */
        busy = EPD_BUSY;
        if (busy)
            break;                     /* 高电平 = 空闲 */
        EPD_DelayMS(20);
    } while (polls--);

    EPD_DelayMS(20);
    return busy ? 0 : 1;               /* 1 = 超时 */
}

/* ---------------- 硬复位 (官方: 高200ms -> 低1ms -> 高200ms, 单脉冲) ---------------- */
static void EPD_Reset(void)
{
    EPD_RST = 1;
    EPD_DelayMS(200);
    EPD_RST = 0;
    EPD_DelayMS(1);
    EPD_RST = 1;
    EPD_DelayMS(200);
}

/* ---------------- 上电初始化 (微雪 EPD_2IN13B_V3_Init 逐字节照抄) ---------------- */
void EPD_Init(void)
{
    EPD_GpioInit();
    EPD_PowerOn();
    EPD_Reset();
    EPD_DelayMS(10);                   /* 官方 Reset 后延时 10ms */

    EPD_SendCommand(0x04);             /* Power on */
    EPD_WaitBusy(5000);

    EPD_SendCommand(0x00);             /* panel setting */
    EPD_SendData(0x0F);                /* LUT from OTP */
    EPD_SendData(0x89);                /* 温度传感器 / boost 等时序 */

    EPD_SendCommand(0x61);             /* resolution: 104 x 212 */
    EPD_SendData(0x68);
    EPD_SendData(0x00);
    EPD_SendData(0xD4);

    EPD_SendCommand(0x50);             /* VCOM and data interval */
    EPD_SendData(0x77);
}

/* ---------------- 全刷: 黑层 0x10 + 红层 0x13 + 0x12 (官方 Display/Clear 同款) ----------------
 * black/red 任一指针为 NULL 时该层发全 0xFF (白/不显示红)
 * 三色全刷耗时较长 (~15s), 超时留足余量
 */
static void EPD_RefreshFull(const uint8 *black, const uint8 *red)
{
    uint16 i;

    EPD_SendCommand(0x10);             /* B/W RAM: 0xFF=白 0x00=黑 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(black ? black[i] : 0xFF);

    EPD_SendCommand(0x13);             /* RED RAM: 0xFF=不显示红 0x00=红 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(red ? red[i] : 0xFF);

    EPD_SendCommand(0x12);             /* DISPLAY REFRESH */
    EPD_DelayMS(100);                  /* 官方 TurnOnDisplay: 0x12 后等 100ms 再查忙 */
    EPD_WaitBusy(25000);
}

/* ---------------- 全刷 API ---------------- */
void EPD_Fill(uint8 color)             /* 0xFF=白 0x00=黑 */
{
    uint16 i;

    EPD_SendCommand(0x10);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(color);

    EPD_SendCommand(0x13);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(0xFF);            /* 红层不显示 */

    EPD_SendCommand(0x12);
    EPD_DelayMS(100);
    EPD_WaitBusy(25000);
}

void EPD_DisplayImage(const uint8 *img)
{
    EPD_RefreshFull(img, (const uint8 *)0);
}

void EPD_DisplayImageRY(const uint8 *black, const uint8 *red)
{
    EPD_RefreshFull(black, red);
}

/* ---------------- 深度睡眠 + 断电 (官方 Sleep + P0.7 断电) ---------------- */
void EPD_Sleep(void)
{
    EPD_SendCommand(0x50);             /* VCOM: 休眠边框设置 */
    EPD_SendData(0xF7);
    EPD_SendCommand(0x02);             /* power off */
    EPD_WaitBusy(5000);
    EPD_SendCommand(0x07);             /* deep sleep */
    EPD_SendData(0xA5);
    EPD_DelayMS(100);
    EPD_PowerOff();                    /* P0.7 断电 */
}

/* ==================================================================
 * 横屏帧缓冲 + 画图 API
 * 帧缓冲 EPD_Frame 为面板原生布局 (13B x 212 行, 黑层),
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
 * 输入: x = 0..211 (横向), y = 0..103 (纵向), black = 1 画黑
 * 映射 (EPD_ROT_CW=1, 屏幕顺时针转 90° 摆横, 等价官方示例 270°):
 *   横屏 (x, y) -> 面板 (src, gate) = (y, EPD_LAND_W-1-x)
 *   面板布局: 帧[gate] 的 bit(7-src%8), 字节 = frame[gate*13 + src/8]
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

/* 全刷显示 EPD_Frame (三色屏无局刷) */
void EPD_ShowFrame(void)
{
    EPD_DisplayImage((const uint8 *)EPD_Frame);
}

/* ---------------- 画图 API (横屏坐标 x:0-211, y:0-103) ---------------- */

/* 画线 (Bresenham) */
void EPD_DrawLine(int x0, int y0, int x1, int y1, uint8 black)
{
    int dx = x1 - x0 >= 0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 - y0 <= 0 ? y1 - y0 : y0 - y1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1)
    {
        EPD_SetPixel((uint16)x0, (uint8)y0, black);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

/* 画矩形边框 */
void EPD_DrawRect(int x0, int y0, int x1, int y1, uint8 black)
{
    EPD_DrawLine(x0, y0, x1, y0, black);
    EPD_DrawLine(x0, y1, x1, y1, black);
    EPD_DrawLine(x0, y0, x0, y1, black);
    EPD_DrawLine(x1, y0, x1, y1, black);
}

/* 画填充矩形 */
void EPD_DrawFilledRect(int x0, int y0, int x1, int y1, uint8 black)
{
    int x, y;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            EPD_SetPixel((uint16)x, (uint8)y, black);
}

/* 画圆 (Bresenham 中点圆) */
void EPD_DrawCircle(int xc, int yc, int r, uint8 black)
{
    int x = -r, y = 0, err = 2 - 2 * r, e2;
    do {
        EPD_SetPixel((uint16)(xc - x), (uint8)(yc + y), black);
        EPD_SetPixel((uint16)(xc + x), (uint8)(yc + y), black);
        EPD_SetPixel((uint16)(xc + x), (uint8)(yc - y), black);
        EPD_SetPixel((uint16)(xc - x), (uint8)(yc - y), black);
        e2 = err;
        if (e2 <= y) { err += ++y * 2 + 1; if (-x == y && e2 <= x) e2 = 0; }
        if (e2 > x)  { err += ++x * 2 + 1; }
    } while (x <= 0);
}

/* 画填充圆 */
void EPD_DrawFilledCircle(int xc, int yc, int r, uint8 black)
{
    int x = -r, y = 0, err = 2 - 2 * r, e2;
    do {
        EPD_DrawLine(xc + x, yc + y, xc - x, yc + y, black);
        EPD_DrawLine(xc + x, yc - y, xc - x, yc - y, black);
        e2 = err;
        if (e2 <= y) { err += ++y * 2 + 1; if (-x == y && e2 <= x) e2 = 0; }
        if (e2 > x)  { err += ++x * 2 + 1; }
    } while (x <= 0);
}

/* ---------------- 字符绘制 (横屏坐标, 用 __code 字模) ---------------- */
void EPD_DrawChar(uint16 x, uint16 y, char ch, const sFONT *font, uint8 black)
{
    uint16 i, j;
    uint16 offset = (uint16)(ch - ' ') * font->Height * (font->Width / 8 + (font->Width % 8 ? 1 : 0));
    const uint8 __code *ptr = (const uint8 __code *)font->table + offset;

    for (j = 0; j < font->Height; j++)
    {
        for (i = 0; i < font->Width; i++)
        {
            if (*ptr & (0x80 >> (i % 8)))
                EPD_SetPixel(x + i, y + j, black);
            if (i % 8 == 7)
                ptr++;
        }
        if (font->Width % 8 != 0)
            ptr++;
    }
}

void EPD_DrawString(uint16 x, uint16 y, const char *text, const sFONT *font, uint8 black)
{
    uint16 w = font->Width;
    while (*text)
    {
        EPD_DrawChar(x, y, *text, font, black);
        x = (uint16)(x + w);
        text++;
    }
}
