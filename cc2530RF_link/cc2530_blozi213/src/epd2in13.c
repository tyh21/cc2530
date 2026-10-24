/**
 * @file    epd2in9.c
 * @brief   blozi 213 电子价签 2.13 寸 EPD 驱动实现 (HINK-E0213A20, 104x212, bit-bang SPI)
 *
 * [2026-10-24 重写] 序列全部照抄本板原厂固件 yuanma_Blozi_213_cc2530.hex,
 * 反汇编对照地址见注释 (工具: .temp/disasm_213.py / flow_213.py)。
 *
 * 与旧的 "290 板移植版" 的本质差异 (旧版不显示的原因):
 *   1. 面板几何: 296 gate x 16B/行 -> 212 gate x 13B/行
 *      (0x01 = 0xD3 = 211, 0x44/0x45 窗口与 0x4E/0x4F 指针全部按 104x212 重设)
 *   2. 数据入口/行序由 EPD_RAM_Y_DESC 选择 (默认与 LG213/统一图源一致)
 *   3. 不再需要 LUT 装载 (0x32/0x03/0x04/0x3A/0x3B), 直接 0x22 0xC7 走 OTP 波形
 *
 * [2026-10-24 实机实测修正] 本板屏是**红黑白三色**:
 *   0x24 = 黑白层 (0=黑 1=白), 0x26 = 红层, 本板上"写 0xFF 会整片红"
 *   -> 不显示红时红层写 0x00 (EPD_RedNone), 不再像原厂那样把同图写进两页
 *   (原厂固件那份 hex 的红层约定与本板实测相反, 已按实测修正)
 *
 * 引脚: BUSY=P0.4  RST=P0.5  DC=P0.6  CS=P0.7  SCLK=P1.0  SDI=P1.1
 *       PWR=P0.3  (MOS 管电源开关, 低有效)
 */
#include "epd2in13.h"

/* ---------------- 内部延时 (32MHz 主频校准) ---------------- */
static void EPD_DelayMS(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 电源控制: P0.3 -> MOS 管开关 (低有效) ----------------
 * 原厂 0x1E23: SETB P0.3 (断) + 延时 -> CLR P0.3 (通) + 延时 -> 再走 0x13CF 初始化
 */
void EPD_PowerOn(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;         /* 断电位置 (放电) */
    EPD_DelayMS(500);
    EPD_PWR = EPD_PWR_ON_LVL;          /* 上电 */
    EPD_DelayMS(500);                  /* 上电稳定 */
}

void EPD_PowerOff(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;
}

/* ---------------- GPIO 初始化 ----------------
 * 原厂 0x1E00: P0SEL &= ~0xEF / P0DIR |= 0xEF (P0.0-0.3, P0.5-0.7 全输出,
 * P0.4 保持输入) / P0INP = 0x00 / P2INP &= ~0xDF (P0 上拉) / P1SEL &= ~0x03 /
 * P1DIR |= 0x03。注意 P0.0-0.2 就是板载 RGB LED, 这里一并配成输出。
 */
void EPD_GpioInit(void)
{
    /* [关键] 先清外设功能: 不通用 IO 则位操作无效 */
    P0SEL &= ~0xEF;
    P1SEL &= ~0x03;

    /* P0.3/5/6/7 输出, P0.0-0.2 输出(LED), P0.4 输入(BUSY) */
    P0DIR |=  0xEF;
    P0DIR &= ~0x10;

    /* P1.0/P1.1 输出 (SCLK/SDI) */
    P1DIR |=  0x03;

    /* P0 上拉/下拉使能 + 选上拉 (原厂 P0INP=0x00, P2INP.5=0) */
    P0INP  =  0x00;
    P2INP &= ~0x20;

    /* [关键] 初始电平: CS=1/DC=1/RST=1 (CS/RST 常低 = 屏永远被复位) */
    EPD_CS   = 1;
    EPD_DC   = 1;
    EPD_SCLK = 0;
    EPD_SDI  = 0;
    EPD_RST  = 1;
    EPD_PWR  = EPD_PWR_OFF_LVL;        /* 先保持断电, 由 EPD_PowerOn() 上电 */
}

/* ---------------- bit-bang SPI (原厂 0x0BC3/0x0BEE 同款: MSB 先出, 上升沿锁存) ---------------- */
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

/* ---------------- BUSY 等待 (P0.4 高=忙, 等低, 带超时) ----------------
 * 原厂 0x0BB4: MOV C,P0.4 / JC 回跳 = 高电平期间一直等
 */
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

/* ---------------- 硬复位 (原厂 0x0BA1: CLR P0.5 -> 延时 -> SETB P0.5) ---------------- */
static void EPD_Reset(void)
{
    EPD_RST = 1;
    EPD_DelayMS(10);
    EPD_RST = 0;
    EPD_DelayMS(10);
    EPD_RST = 1;
    EPD_DelayMS(10);
}

/* ---------------- 上电初始化 ----------------
 * 流程 = 原厂 0x13CF (0x13D6 复位 -> 0x13D9 软复位 0x12 -> 0x13E1 init 0x0C38)
 */
void EPD_Init(void)
{
    EPD_GpioInit();
    EPD_PowerOn();                    /* P0.3 上电 (原厂时序) */

    EPD_Reset();                      /* 0x13D6 */
    EPD_WaitBusy(3000);

    EPD_SendCommand(0x12);            /* 0x13D9 软复位 */
    EPD_WaitBusy(3000);

    /* ---- 以下为原厂 0x0C38 段, 逐条照抄 ---- */
    EPD_SendCommand(0x74);            /* 温度传感器写 */
    EPD_SendData(0x54);

    EPD_SendCommand(0x7E);            /* 外部温度传感器读 */
    EPD_SendData(0x3B);

    EPD_SendCommand(0x2B);            /* VCOM 设置 */
    EPD_SendData(0x04);
    EPD_SendData(0x63);

    EPD_SendCommand(0x0C);            /* booster 软启动 */
    EPD_SendData(0x8B);
    EPD_SendData(0x9C);
    EPD_SendData(0x96);
    EPD_SendData(0x0F);

    EPD_SendCommand(0x01);            /* 驱动输出控制: 212 条 gate */
    EPD_SendData(0xD3);               /* 0xD3 = 211 = 212-1 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x11);            /* 数据入口: EPD_RAM_Y_DESC ? (X增 Y减) : (X增 Y增) */
#if EPD_RAM_Y_DESC
    EPD_SendData(0x01);
#else
    EPD_SendData(0x03);
#endif

    EPD_SendCommand(0x44);            /* RAM X 窗口: 0 ~ 12 (13 字节 = 104 列) */
    EPD_SendData(0x00);
    EPD_SendData(0x0C);

    EPD_SendCommand(0x45);            /* RAM Y 窗口: 212 行 */
#if EPD_RAM_Y_DESC
    EPD_SendData(0xD3);               /* 211 -> 0 (原厂顺序) */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
#else
    EPD_SendData(0x00);               /* 0 -> 211 */
    EPD_SendData(0x00);
    EPD_SendData(0xD3);
    EPD_SendData(0x00);
#endif

    EPD_SendCommand(0x3C);            /* 边框波形 */
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);            /* 内置温度传感器 */
    EPD_SendData(0x80);

    EPD_SendCommand(0x22);            /* 刷新: OTP 波形, 清一次底色 */
    EPD_SendData(0xB1);
    EPD_SendCommand(0x20);            /* master activation */
    EPD_WaitBusy(8000);
}

/* ---------------- 写 2756 字节到面板 RAM ----------------
 * 原厂 0x1316 (new 0x24) / 0x1377 (old 0x26) 同款:
 *   0x4E 00      -> X 地址计数器 = 0
 *   0x4F D3 00   -> Y 地址计数器 = 211 (低字节在前), 默认设置下改为 0 (见 EPD_RAM_Y_DESC)
 *   然后按数据入口方向顺序写入, 与 EPD_Frame 的字节序一致
 */
static void EPD_SetPtrStart(void)
{
    EPD_SendCommand(0x4E);           /* RAM X 指针 = 0 */
    EPD_SendData(0x00);
    EPD_SendCommand(0x4F);           /* RAM Y 指针 */
#if EPD_RAM_Y_DESC
    EPD_SendData(0xD3);              /* 211 */
    EPD_SendData(0x00);
#else
    EPD_SendData(0x00);              /* 0 */
    EPD_SendData(0x00);
#endif
}

static void EPD_WriteRAM(uint8 cmd, const uint8 *img, uint8 fill)
{
    uint16 i;

    EPD_SetPtrStart();
    EPD_SendCommand(cmd);            /* 0x24 = new RAM(黑白层), 0x26 = 红层 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img ? img[i] : fill);
}

/* 红层"不显示红"的电平: 本板实机 0xFF 会整屏变红 -> 默认 0x00
 * (自检固件会 A/B 切换它做对照; 正式固件保持实测值) */
uint8 EPD_RedNone = 0x00;

/* ---------------- 全刷刷新 (原厂 0x0D60: 0x22 C7 + 0x20) ---------------- */
void EPD_Display(void)
{
    EPD_SendCommand(0x22);
    EPD_SendData(0xC7);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(20000);             /* 2.13 寸全刷 ~3s, 留足余量 */
}

/* ---------------- 全屏填充并全刷 ---------------- */
void EPD_Fill(uint8 color)           /* 0xFF=白 0x00=黑 */
{
    EPD_WriteRAM(0x24, (const uint8 *)0, color);          /* 黑白层 */
    EPD_WriteRAM(0x26, (const uint8 *)0, EPD_RedNone);    /* 红层: 不显示红 */
    EPD_Display();
}

/* ---------------- 显示一幅图 (全刷) ----------------
 * 黑白图写 0x24, 红层固定写 EPD_RedNone (本工程图源只有黑白)
 */
void EPD_DisplayImage(const uint8 *img)
{
    EPD_WriteRAM(0x24, img, 0);
    EPD_WriteRAM(0x26, (const uint8 *)0, EPD_RedNone);
    EPD_Display();
}

/* ---------------- 深度睡眠 ----------------
 * 原厂 0x0D40: 0x10 0x01。只发命令不断电, RST 硬复位即可唤醒,
 * 下次 EPD_Init 内 PowerOn 重走完整电源时序
 */
void EPD_Sleep(void)
{
    EPD_SendCommand(0x10);
    EPD_SendData(0x01);
}

/* ==================================================================
 * 横屏帧缓冲 (面板原生布局 13B x 212 行), 与统一图源 212x104 通用
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
 * 映射 (EPD_ROT_CW=1, 屏幕顺时针转 90° 摆横), 与 pl_frame_write_213() 一致:
 *   横屏 (x, y) -> 面板 (src, gate) = (y, 211-x)
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

/* 全刷显示 EPD_Frame */
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
