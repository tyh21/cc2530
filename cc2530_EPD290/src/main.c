/**
 * @file    main.c
 * @brief   blozi_290 EPD 图形+文字演示 (移植自 CC2530_EPaperModule 项目)
 *
 * 效果:
 *   1. 显示 IMAGE_DATA 全屏图片 (2 秒)
 *   2. 清屏 (2 秒)
 *   3. 画几何图形: 矩形/圆/对角线
 *   4. 循环显示时钟文字 "MM:SS" + "Relay: OFF/ON"
 *
 * 板载 RGB LED: R=P0.1  G=P0.2  B=P0.0 (低电平点亮)
 * EPD 引脚    : BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1
 *               PWR=P0.3 (MOS 管电源开关, 低有效)
 */
#include <ioCC2530.h>
#include <stdint.h>
#include "epd2in9.h"
#include "epdpaint.h"
#include "epdtest.h"
#include "imagedata.h"

#define LED_ON   0
#define LED_OFF  1

#define RED_PIN    P0_1
#define GREEN_PIN  P0_2
#define BLUE_PIN   P0_0

/* uint8/uint16 类型已在 epd2in9.h 中定义 */

/* ---------------- 延时 (32MHz 校准) ---------------- */
void DelayMS(uint16 msec)
{
    uint16 i, j;
    for (i = 0; i < msec; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 系统时钟: 32MHz ---------------- */
static void clock_init(void)
{
    CLKCONCMD &= ~0x40;
    while (CLKCONSTA & 0x40);
    CLKCONCMD &= ~0x47;
}

/* ---------------- RGB LED ---------------- */
static void led_init(void)
{
    P0SEL &= ~0x07;
    P0DIR |=  0x07;
    RED_PIN = LED_OFF;
    GREEN_PIN = LED_OFF;
    BLUE_PIN = LED_OFF;
}

static void set_led(uint8 r, uint8 g, uint8 b)
{
    RED_PIN   = r ? LED_ON : LED_OFF;
    GREEN_PIN = g ? LED_ON : LED_OFF;
    BLUE_PIN  = b ? LED_ON : LED_OFF;
}

/* epdpaint 画布缓冲区 (epdtest.c 的 PaintClear/PaintDraw* 操作此内存) */
__xdata static uint8 paint_buf[896];

int main(void)
{
    unsigned long time_now_s = 0;
    uint8 relay_state = 0;
    uint8 frame_cnt = 0;

    clock_init();
    led_init();

    /* 初始化 epdpaint 画布指针 (必须在使用 Paint* 函数之前调用) */
    PaintPaint(paint_buf, 0, 0);

    /* ---- 阶段 1: 全屏图片 ---- */
    set_led(1, 0, 0);
    EPD_Init();
    EpdSetFrameMemory((const uint8 *)IMAGE_DATA);
    EpdDisplayFrame();
    DelayMS(2000);

    /* ---- 阶段 2: 清屏 ---- */
    set_led(0, 0, 1);
    EpdClearFrameMemory(0xFF);
    EpdDisplayFrame();
    DelayMS(2000);

    /* ---- 阶段 3: 几何图形 (全刷) ---- */
    set_led(0, 1, 0);
    EpdtestNotRefresh();
    DelayMS(2000);

    /* ---- 切局刷模式 ---- */
    EPD_InitPartial();

    /* ---- 阶段 4: 循环时钟+继电器状态 (局刷) ---- */
    while (1)
    {
        set_led(0, 0, 1);              /* 蓝: 局刷中 */
        EpdtestRefreshPartial(relay_state, time_now_s);
        set_led(0, 1, 0);              /* 绿: 空闲 1 秒 */
        DelayMS(1000);

        time_now_s++;
        if (time_now_s >= 60)
        {
            time_now_s = 0;
            relay_state = !relay_state;
        }

        /* 每 30 帧全刷去残影 */
        frame_cnt++;
        if (frame_cnt >= 30)
        {
            frame_cnt = 0;
            set_led(1, 0, 0);
            EpdClearFrameMemory(0xFF);
            EpdDisplayFrame();
            EpdtestNotRefresh();
            EPD_InitPartial();
        }
    }
}
