/**
 * @file    epd2in9.c
 * @brief   2.9 寸 EPD 驱动实现 (SSD1680, WFT0290CZ10, LG 价签板, bit-bang SPI)
 *
 * 引脚 (原厂固件逆向 + 实测双重验证):
 *   BUSY=P1.2  RST=P1.1  DC=P0.2  CS=P0.4  SCLK=P0.5  SDI=P0.3
 *   PWR=P0.7 (EPD 供电 MOS, 高电平上电)
 *
 * 刷新序列来源: 微雪 EPD_2in9d (ws_drivers/EPD_2in9d.cpp, 用户实测完美点亮)
 *   全刷: 0x00 0x1F (OTP 波形) + 0x10/0x13 双 RAM + 0x12
 *   局刷: SetPartReg (局刷 LUT 0x20-0x24) + 0x91/0x90 部分窗 + 0x13 + 0x12
 *   BUSY: 发 0x71 查询, 引脚高=空闲 (与原固件 0x11771 等待行为一致)
 */
#include "epd2in9.h"
#include "fonts.h"

/* ==================================================================
 * 局刷 LUT (微雪 EPD_2IN9D_SetPartReg, 逐字节照抄)
 * ================================================================== */
static const uint8 __code lut_vcom1[44] = {
    0x00, 0x19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};
static const uint8 __code lut_ww1[42] = {
    0x00, 0x19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8 __code lut_bw1[42] = {
    0x80, 0x19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8 __code lut_wb1[42] = {
    0x40, 0x19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8 __code lut_bb1[42] = {
    0x00, 0x19, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

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
    EPD_PWR = EPD_PWR_ON_LVL;          /* 上电 (原固件 0x11735) */
    EPD_DelayMS(100);                  /* 供电稳定 */
}

void EPD_PowerOff(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;         /* 原固件 0x11766 休眠断电 */
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

/* ---------------- BUSY 等待 (微雪方式: 发 0x71 后查询, 高=空闲) ----------------
 * 原固件 0x11771/0x11B6A 同样是等 P1.2 拉高, 带超时防死等
 */
static uint8 EPD_WaitBusy(uint16 timeout_ms)
{
    uint16 polls = timeout_ms / 20U;
    uint8  busy;

    do
    {
        EPD_SendCommand(0x71);         /* GetStatus */
        busy = EPD_BUSY;
        if (busy)
            break;                     /* 高电平 = 空闲 */
        EPD_DelayMS(20);
    } while (polls--);

    EPD_DelayMS(20);
    return busy ? 0 : 1;               /* 1 = 超时 */
}

/* ---------------- 硬复位 (微雪 2in9d: 4 个脉冲) ---------------- */
static void EPD_Reset(void)
{
    uint8 i;

    EPD_RST = 1;
    EPD_DelayMS(20);
    for (i = 0; i < 3; i++)
    {
        EPD_RST = 0;
        EPD_DelayMS(2);
        EPD_RST = 1;
        EPD_DelayMS(20);
    }
}

/* ---------------- 全刷模式恢复 (退出局刷 + OTP 波形) ----------------
 * 局刷跑过之后 0x00 驻留 0xBF (LUT 寄存器模式+部分窗),
 * 全刷前必须 0x92 退出部分窗 + 0x00 0x1F 切回 OTP 波形
 */
static void EPD_LeavePartial(void)
{
    EPD_SendCommand(0x92);             /* Partial Out */

    EPD_SendCommand(0x00);             /* panel setting: LUT from OTP */
    EPD_SendData(0x1F);

    EPD_SendCommand(0x50);             /* VCOM and data interval */
    EPD_SendData(0x97);
}

/* ---------------- 上电初始化 (微雪 EPD_2IN9D_Init) ---------------- */
void EPD_Init(void)
{
    EPD_GpioInit();
    EPD_PowerOn();
    EPD_Reset();

    EPD_SendCommand(0x04);             /* Power on */
    EPD_WaitBusy(5000);

    EPD_SendCommand(0x00);             /* panel setting: LUT from OTP, BWOTP */
    EPD_SendData(0x1F);

    EPD_SendCommand(0x61);             /* resolution: 128 x 296 */
    EPD_SendData(0x80);
    EPD_SendData(0x01);
    EPD_SendData(0x28);

    EPD_SendCommand(0x50);             /* VCOM and data interval */
    EPD_SendData(0x97);
}

/* ---------------- 全窗指针回零 (混合局刷后保证全屏写入) ---------------- */
static void EPD_SetFullWindow(void)
{
    EPD_SendCommand(0x44);             /* RAM X window: 0 ~ 0x0F (128列) */
    EPD_SendData(0x00);
    EPD_SendData(0x0F);

    EPD_SendCommand(0x45);             /* RAM Y window: 0 ~ 295 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x27);
    EPD_SendData(0x01);

    EPD_SendCommand(0x4E);             /* RAM X pointer = 0 */
    EPD_SendData(0x00);

    EPD_SendCommand(0x4F);             /* RAM Y pointer = 0 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
}

/* ---------------- 全刷: old RAM=0x00 + new RAM=图 + 0x12 (微雪原味) ---------------- */
static void EPD_RefreshFull(const uint8 *img)
{
    uint16 i;

    EPD_LeavePartial();
    EPD_SetFullWindow();

    EPD_SendCommand(0x10);             /* old RAM = 黑 (微雪 Clear/Display 同款) */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(0x00);

    EPD_SendCommand(0x13);             /* new RAM = 目标图 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : 0xFF);

    EPD_SendCommand(0x12);             /* DISPLAY REFRESH */
    EPD_DelayMS(10);                   /* 命令后至少 200us, 留 10ms */
    EPD_WaitBusy(15000);
}

/* ---------------- 装载局刷 LUT + 局刷寄存器 (微雪 EPD_2IN9D_SetPartReg) ---------------- */
static void EPD_SetPartReg(void)
{
    uint8 i;

    EPD_SendCommand(0x01);             /* power setting */
    EPD_SendData(0x03);
    EPD_SendData(0x00);
    EPD_SendData(0x2B);
    EPD_SendData(0x2B);
    EPD_SendData(0x03);

    EPD_SendCommand(0x06);             /* booster soft start */
    EPD_SendData(0x17);
    EPD_SendData(0x17);
    EPD_SendData(0x17);

    EPD_SendCommand(0x04);             /* power on */
    EPD_WaitBusy(5000);

    EPD_SendCommand(0x00);             /* panel setting: LUT from register */
    EPD_SendData(0xBF);

    EPD_SendCommand(0x30);             /* PLL: 100Hz */
    EPD_SendData(0x3A);

    EPD_SendCommand(0x61);             /* resolution 128 x 296 */
    EPD_SendData(0x80);
    EPD_SendData(0x01);
    EPD_SendData(0x28);

    EPD_SendCommand(0x82);             /* vcom_DC */
    EPD_SendData(0x12);

    EPD_SendCommand(0x50);             /* VCOM and data interval */
    EPD_SendData(0x97);

    EPD_SendCommand(0x20);             /* LUT vcom (44) */
    for (i = 0; i < 44; i++)
        EPD_SendData(lut_vcom1[i]);

    EPD_SendCommand(0x21);             /* LUT ww (42) */
    for (i = 0; i < 42; i++)
        EPD_SendData(lut_ww1[i]);

    EPD_SendCommand(0x22);             /* LUT bw (42) */
    for (i = 0; i < 42; i++)
        EPD_SendData(lut_bw1[i]);

    EPD_SendCommand(0x23);             /* LUT wb (42) */
    for (i = 0; i < 42; i++)
        EPD_SendData(lut_wb1[i]);

    EPD_SendCommand(0x24);             /* LUT bb (42) */
    for (i = 0; i < 42; i++)
        EPD_SendData(lut_bb1[i]);
}

/* ---------------- 全刷 API ---------------- */
void EPD_Display(void)
{
    EPD_RefreshFull((const uint8 *)0);
}

void EPD_Fill(uint8 color)             /* 0xFF=白 0x00=黑 */
{
    uint16 i;

    EPD_LeavePartial();
    EPD_SetFullWindow();

    EPD_SendCommand(0x10);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(0x00);

    EPD_SendCommand(0x13);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(color);

    EPD_SendCommand(0x12);
    EPD_DelayMS(10);
    EPD_WaitBusy(15000);
}

void EPD_DisplayImage(const uint8 *img)
{
    EPD_RefreshFull(img);
}

/* ---------------- 局刷基底: 全刷一次 (主循环定期去残影用) ---------------- */
void EPD_DisplayBase(const uint8 *img)
{
    EPD_RefreshFull(img);
}

/* ---------------- 局刷一帧 (微雪 EPD_2IN9D_DisplayPart 原味) ---------------- */
void EPD_DisplayPartial(const uint8 *img)
{
    uint16 i;

    EPD_SetPartReg();

    EPD_SendCommand(0x91);             /* Partial In */
    EPD_SendCommand(0x90);             /* Partial Window */
    EPD_SendData(0x00);                /* x-start */
    EPD_SendData(EPD_WIDTH - 1);       /* x-end */
    EPD_SendData(0x00);                /* y-start hi */
    EPD_SendData(0x00);                /* y-start lo */
    EPD_SendData((EPD_HEIGHT >> 8) & 0xFF);
    EPD_SendData((EPD_HEIGHT & 0xFF) - 1);
    EPD_SendData(0x28);                /* 扫描增量 (微雪原味) */

    EPD_SendCommand(0x13);             /* new RAM */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : 0xFF);

    EPD_SendCommand(0x12);             /* DISPLAY REFRESH */
    EPD_DelayMS(10);
    EPD_WaitBusy(5000);
}

/* ---------------- 差分局刷: old + new 都写 ---------------- */
void EPD_DisplayPartialD(const uint8 *old_img, const uint8 *new_img)
{
    uint16 i;

    EPD_SetPartReg();

    EPD_SendCommand(0x91);
    EPD_SendCommand(0x90);
    EPD_SendData(0x00);
    EPD_SendData(EPD_WIDTH - 1);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData((EPD_HEIGHT >> 8) & 0xFF);
    EPD_SendData((EPD_HEIGHT & 0xFF) - 1);
    EPD_SendData(0x28);

    EPD_SendCommand(0x10);             /* old RAM = 屏幕当前内容 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(old_img ? old_img[i] : 0xFF);

    EPD_SendCommand(0x13);             /* new RAM = 目标内容 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(new_img ? new_img[i] : 0xFF);

    EPD_SendCommand(0x12);
    EPD_DelayMS(10);
    EPD_WaitBusy(5000);
}

/* ---------------- 深度睡眠 + 断电 (微雪 Sleep + 原固件断电) ---------------- */
void EPD_Sleep(void)
{
    EPD_SendCommand(0x50);             /* VCOM: 局刷边框保持 */
    EPD_SendData(0xF7);
    EPD_SendCommand(0x02);             /* power off */
    EPD_WaitBusy(5000);
    EPD_SendCommand(0x07);             /* deep sleep */
    EPD_SendData(0xA5);
    EPD_PowerOff();                    /* P0.7 断电 (原固件 0x11766) */
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

/* 局刷显示 EPD_Frame */
void EPD_ShowFrame(void)
{
    EPD_DisplayPartial(EPD_Frame);
}

/* ---------------- 画图 API (横屏坐标 x:0-295, y:0-127) ---------------- */

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
