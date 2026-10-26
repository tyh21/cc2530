/**
 * @file    clockface.c
 * @brief   墨水屏时钟实现 (写死起点 12:34, 硬件计时走时)
 *
 * 布局 (横屏 212 x 104):
 *   y=2  :  "2026-10-25" (11x16 ASCII)  +  "星期X" (16x16 汉字)
 *   y=22 :  "HH:MM"      (40x80 大号数字)
 *
 * 刷新节奏 (同原 DA14585 工程, 解决"每次刷新都闪"的问题):
 *   上电 / 整点 -> 全刷一次 (会闪几下, 顺带清残影)
 *   平时分钟更新 -> 局刷 (0x22=0x04, 快, 不闪)
 *
 * 走时基准: Timer1 硬件计时 (32MHz/128=250kHz), 精确; 初版用软件空转延时
 * 实测偏快 2.4 倍, 已弃用。
 *
 * 整屏方向/刷新方式都由 epd2in13.c 按 DA14585 原工程逐字节对齐实现;
 * 若实机方向不对, 改 epd2in13.c 里 EPD_SetPixel 的帧排布 (与原工程一致)。
 */
#include "clockface.h"
#include "epd2in13.h"
#include "epd_paint.h"
#include "clockfont.h"
#include "hal_mcu.h"
#if CLOCK_USE_RF
#include "pl_time.h"
#endif

/* ---------------- 走时状态 ---------------- */
uint8  clock_hour = CLOCK_START_HOUR;
uint8  clock_min  = CLOCK_START_MIN;
uint8  clock_sec  = CLOCK_START_SEC;
uint16 clock_year = CLOCK_START_YEAR;
uint8  clock_mon  = CLOCK_START_MON;
uint8  clock_day  = CLOCK_START_DAY;

/* ---------------- 中文"星期"字串 (UTF-8 字节, 避开源码编码问题) ----------------
 * 字模 EPD_FontUTF8_16x16 里含 星 期 一 二 三 四 五 六 日 */
static __code const uint8 CLK_XQ[6]  = {0xE6, 0x98, 0x9F, 0xE6, 0x9C, 0x9F}; /* "星期" */
static __code const uint8 CLK_WEEK[7][3] = {
    {0xE6, 0x97, 0xA5},   /* 日 */
    {0xE4, 0xB8, 0x80},   /* 一 */
    {0xE4, 0xBA, 0x8C},   /* 二 */
    {0xE4, 0xB8, 0x89},   /* 三 */
    {0xE5, 0x9B, 0x9B},   /* 四 */
    {0xE4, 0xBA, 0x94},   /* 五 */
    {0xE5, 0x85, 0xAD}    /* 六 */
};

/* ---------------- 日历小工具 ---------------- */
static __code const uint8 CLK_MDAYS[12] =
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static __code const uint8 CLK_ZELLER[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

static uint8 clock_is_leap(uint16 y)
{
    if (y % 100 == 0)
        return (uint8)(y % 400 == 0);
    return (uint8)(y % 4 == 0);
}

static uint8 clock_days_in_month(uint16 y, uint8 m)
{
    if (m == 2 && clock_is_leap(y))
        return 29;
    return CLK_MDAYS[m - 1];
}

/* Sakamoto 星期算法: 0=周日, 1=周一 ... 6=周六 */
static uint8 clock_weekday(uint16 y, uint8 m, uint8 d)
{
    if (m < 3)
        y = (uint16)(y - 1);
    return (uint8)((y + y / 4 - y / 100 + y / 400 + CLK_ZELLER[m - 1] + d) % 7);
}

/* ---------------- 演示用自检图 (开机先给个方向参照, 想关就把宏改 0) ----------------
 * 白底 + 黑框 + 左上黑块 + 左下->右上对角线; 判读见 epd2in13.h */
#define CLOCK_SELFTEST  0

/* ---------------- 走时 ---------------- */
void clockface_set_time(uint8 hh, uint8 mm, uint8 ss)
{
    clock_hour = (uint8)(hh % 24);
    clock_min  = (uint8)(mm % 60);
    clock_sec  = (uint8)(ss % 60);
}

void clockface_set_date(uint16 y, uint8 m, uint8 d)
{
    clock_year = y;
    clock_mon  = (m >= 1 && m <= 12) ? m : 1;
    clock_day  = (d >= 1) ? d : 1;
}

uint8 clockface_tick_1s(void)
{
    if (++clock_sec < 60)
        return 0;

    clock_sec = 0;
    if (++clock_min < 60)
        return 1;                        /* 分钟变了 -> 该重绘 */

    clock_min = 0;
    if (++clock_hour >= 24)
    {
        clock_hour = 0;
        if (++clock_day > clock_days_in_month(clock_year, clock_mon))
        {
            clock_day = 1;
            if (++clock_mon > 12)
            {
                clock_mon = 1;
                clock_year++;
            }
        }
    }
    return 1;
}

/* ---------------- 走时用的时间基准 (TI halMcuWaitMs, 按 32MHz 校准) ----------------
 * 驱动内部(上电/复位/等 BUSY/全刷)花掉的毫秒由 EPD_DrainOpMs() 记账取走,
 * 在 clockface_delay_ms() 里抵扣, 这样十几秒的全刷时间不会被"漏掉"。
 */
static uint32 s_credit_ms = 0;       /* 已经流逝、但还没抵扣掉的毫秒 */

void clockface_delay_ms(uint16 ms)
{
    s_credit_ms += EPD_DrainOpMs();  /* 驱动内部花掉的时间算作已流逝 */

    if (s_credit_ms >= (uint32)ms)
    {
        s_credit_ms -= ms;
        return;
    }

    ms = (uint16)((uint32)ms - s_credit_ms);
    s_credit_ms = 0;
    halMcuWaitMs(ms);
}

/* ---------------- 整屏绘制 (part: 0=全刷, 1=局刷) ---------------- */
static void clockface_render(uint8 part)
{
    char  buf[12];
    char  wbuf[10];
    uint8 k;
    uint8 wd = clock_weekday(clock_year, clock_mon, clock_day);

    Paint_Clear(PAINT_WHITE);

    /* --- 顶部左: 日期 YYYY-MM-DD (11x16) --- */
    buf[0] = (char)('0' + (uint8)(clock_year / 1000));
    buf[1] = (char)('0' + (uint8)((clock_year / 100) % 10));
    buf[2] = (char)('0' + (uint8)((clock_year / 10) % 10));
    buf[3] = (char)('0' + (uint8)(clock_year % 10));
    buf[4] = '-';
    buf[5] = (char)('0' + (uint8)(clock_mon / 10));
    buf[6] = (char)('0' + (uint8)(clock_mon % 10));
    buf[7] = '-';
    buf[8] = (char)('0' + (uint8)(clock_day / 10));
    buf[9] = (char)('0' + (uint8)(clock_day % 10));
    buf[10] = '\0';
    EPD_DrawUTF8(4, 2, 0, buf, EPD_ASCII_11X16, 0, PAINT_BLACK, PAINT_WHITE);

    /* --- 顶部右: 星期X (16x16 汉字) --- */
    for (k = 0; k < 6; k++)
        wbuf[k] = (char)CLK_XQ[k];
    wbuf[6] = (char)CLK_WEEK[wd][0];
    wbuf[7] = (char)CLK_WEEK[wd][1];
    wbuf[8] = (char)CLK_WEEK[wd][2];
    wbuf[9] = '\0';
    EPD_DrawUTF8(124, 2, 0, wbuf, 0, EPD_FontUTF8_16x16, PAINT_BLACK, PAINT_WHITE);

    /* --- 中部: HH:MM (40x80 大号数字, 5 格 x 42px) --- */
    buf[0] = (char)('0' + (uint8)(clock_hour / 10));
    buf[1] = (char)('0' + (uint8)(clock_hour % 10));
    buf[2] = ':';
    buf[3] = (char)('0' + (uint8)(clock_min / 10));
    buf[4] = (char)('0' + (uint8)(clock_min % 10));
    buf[5] = '\0';
    EPD_DrawUTF8(2, 22, 2, buf, EPD_40X80_TABLE, 0, PAINT_BLACK, PAINT_WHITE);

    /* --- 刷新 --- */
    if (part)
        EPD_ShowFramePart();
    else
        EPD_ShowFrameFull();
}

void clockface_draw(void)
{
    clockface_render(0);
}

void clockface_draw_part(void)
{
    clockface_render(1);
}

#if CLOCK_SELFTEST
static void clock_selftest(void)
{
    uint8 stage;
    for (;;)
    {
        for (stage = 0; stage < 2; stage++)
        {
            EPD_RedNone = stage ? 0x00 : 0xFF;   /* A/B: 红层电平 */
            EPD_Init();
            Paint_Clear(PAINT_WHITE);
            EPD_DrawRect(2, 2, 209, 101, 1);          /* 四边黑框 */
            EPD_DrawFilledRect(5, 5, 46, 30, 1);      /* 左上角黑方块 (方向参照) */
            EPD_DrawLine(5, 98, 206, 5, 1);           /* 左下->右上 对角线 */
            EPD_ShowFrameFull();
            EPD_Sleep();
            clockface_delay_ms(1000);
            clockface_delay_ms(1000);
            clockface_delay_ms(1000);
            clockface_delay_ms(1000);
            clockface_delay_ms(1000);
            clockface_delay_ms(1000);
        }
    }
}
#endif

/* ---------------- 主循环 ---------------- */
void clockface_run(void)
{
#if CLOCK_SELFTEST
    clock_selftest();                    /* 永不返回 */
#endif

    s_credit_ms = 0;

    clockface_render(0);                 /* 内部自己走 复位+全刷 init */
#if CLOCK_SLEEP_AFTER_DRAW
    EPD_Sleep();
#endif

    for (;;)
    {
        clockface_delay_ms(CLOCK_SECOND_MS);

#if CLOCK_USE_RF
        /* 每秒轮询一次网关校时包: 收到即已校时并整屏重画, 本轮重新计秒 */
        if (pl_time_poll())
            continue;
#endif

        if (!clockface_tick_1s())
            continue;                    /* 还没到下一分钟 */

        if (CLOCK_USE_PARTIAL && clock_min != 0)
        {
            clockface_render(1);         /* 分钟更新: EPD_PART_MODE 决定局刷/全刷 */
        }
        else
        {
            clockface_render(0);         /* 整点: 全刷一次, 清残影 */
        }
#if CLOCK_SLEEP_AFTER_DRAW
        EPD_Sleep();
#endif
    }
}
