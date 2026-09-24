/**
 * @file    main.c
 * @brief   blozi_290 电子价签 EPD 局刷测试 (竖屏版)
 *
 * 流程: 全刷白底打底 -> 切局刷模式 -> 8x16 计数器每秒局刷跳数 (无闪烁)
 * RGB LED 指示: 红=初始化/全刷  蓝=局刷中  绿=空闲
 *
 * 竖屏: 面板原生方向, 128 像素宽 x 296 像素高
 *
 * 板载 RGB LED: R=P0.1  G=P0.2  B=P0.0 (低电平点亮)
 * EPD 引脚    : BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1
 *               PWR=P0.3 (MOS 管电源开关, 低有效)
 */
#include <ioCC2530.h>
#include <stdint.h>
#include "epd2in9.h"

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
    CLKCONCMD &= ~0x40;            /* 选 32MHz 晶振 */
    while (CLKCONSTA & 0x40);      /* 等稳定 */
    CLKCONCMD &= ~0x47;            /* 分频 1, 主频 32MHz */
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

/* ==================================================================
 * 8x16 数字字模 (0-9)
 * 逐行式: 每字节是 8 个像素一行, MSB=最左列
 * EPD 语义: 0=黑, 1=白 (= 0x24 RAM 直接写入值)
 * 字模来源: 标准 ASCII 8x16, 已取反适配 EPD (0=黑)
 * ================================================================== */
static const uint8 font8x16[10][16] = {
    /* 0 */ {0xFF,0xFF,0xFF,0xC1,0xBD,0xBD,0xBD,0xBD,0xBD,0xBD,0xBD,0xBD,0xC1,0xFF,0xFF,0xFF},
    /* 1 */ {0xFF,0xFF,0xFF,0xF3,0xE3,0xC3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xF3,0xFF,0xFF,0xFF},
    /* 2 */ {0xFF,0xFF,0xFF,0xC1,0xBD,0xBD,0xFD,0xFB,0xF7,0xEF,0xDF,0xBF,0x81,0xFF,0xFF,0xFF},
    /* 3 */ {0xFF,0xFF,0xFF,0xC1,0xBD,0xBD,0xFD,0xE1,0xFD,0xFD,0xBD,0xBD,0xC1,0xFF,0xFF,0xFF},
    /* 4 */ {0xFF,0xFF,0xFF,0xFB,0xF7,0xEF,0xDF,0xBF,0x81,0xFB,0xFB,0xFB,0xFB,0xFF,0xFF,0xFF},
    /* 5 */ {0xFF,0xFF,0xFF,0x81,0xBF,0xBF,0xBF,0xC1,0xFD,0xFD,0xFD,0xBD,0xC1,0xFF,0xFF,0xFF},
    /* 6 */ {0xFF,0xFF,0xFF,0xE1,0xDF,0xBF,0xBF,0xC1,0xBD,0xBD,0xBD,0xBD,0xC1,0xFF,0xFF,0xFF},
    /* 7 */ {0xFF,0xFF,0xFF,0x81,0xFD,0xFD,0xFB,0xF7,0xEF,0xEF,0xEF,0xEF,0xEF,0xFF,0xFF,0xFF},
    /* 8 */ {0xFF,0xFF,0xFF,0xC1,0xBD,0xBD,0xBD,0xC1,0xBD,0xBD,0xBD,0xBD,0xC1,0xFF,0xFF,0xFF},
    /* 9 */ {0xFF,0xFF,0xFF,0xC1,0xBD,0xBD,0xBD,0xBD,0xC1,0xFD,0xFD,0xFB,0xE1,0xFF,0xFF,0xFF},
};

/* ---------------- 帧缓冲 (单帧, XDATA) ----------------
 * 竖屏: 面板原生布局, 每行 16 字节 x 296 行 = 4736 字节
 */
static __xdata uint8 frame[EPD_IMG_BYTES];

/* 清帧为白 */
static void frame_clear(void)
{
    uint16 i;
    for (i = 0; i < EPD_IMG_BYTES; i++)
        frame[i] = 0xFF;
}

/* 在帧缓冲上画一个 8x16 数字 (竖屏, 面板原生坐标)
 * px_x: 数字左上角像素列 (0-120, 需 8 对齐)
 * px_y: 数字左上角像素行 (0-280)
 * 直接拷贝 16 字节到帧缓冲对应位置 (每字节=8像素=1字节列)
 */
static void draw_digit(uint8 d, uint8 px_x, uint16 px_y)
{
    uint16 base = (uint16)(px_y * EPD_LINE_BYTES) + (px_x / 8);
    uint8  y;
    for (y = 0; y < 16; y++)
    {
        uint16 off = base + (uint16)(y * EPD_LINE_BYTES);
        if (off < EPD_IMG_BYTES)
            frame[off] = font8x16[d][y];
    }
}

int main(void)
{
    uint16 counter = 0;
    uint8  digit;

    clock_init();
    led_init();

    set_led(1, 0, 0);              /* 红: 初始化 */
    EPD_Init();

    /* 全刷白底打底 (old RAM 也写白, 局刷参考) */
    set_led(1, 0, 0);
    EPD_DisplayBase((void *)0);    /* NULL = 全白 */

    /* 切局刷模式 */
    EPD_InitPartial();

    while (1)
    {
        /* 生成新帧: 白底 + 4 位计数器 (竖屏居中, 8x16 字体)
         * 竖屏 128x296: 4 数字共 32 像素宽, 居中 x=(128-32)/2=48
         * 垂直居中 y=(296-16)/2=140
         */
        frame_clear();
        digit = (uint8)((counter / 1000U) % 10U);
        draw_digit(digit, 48, 140U);
        digit = (uint8)((counter / 100U) % 10U);
        draw_digit(digit, 56, 140U);
        digit = (uint8)((counter / 10U) % 10U);
        draw_digit(digit, 64, 140U);
        digit = (uint8)(counter % 10U);
        draw_digit(digit, 72, 140U);

        /* 局刷: 只写 0x24 + 0x0C 快刷 */
        set_led(0, 0, 1);          /* 蓝: 局刷中 */
        EPD_DisplayPartial(frame);

        set_led(0, 1, 0);          /* 绿: 空闲 1 秒 */
        DelayMS(1000);
        counter++;
        if (counter >= 10000U)
            counter = 0;

        /* 每 30 帧做一次全刷去残影 */
        if ((counter % 30U) == 0U)
        {
            set_led(1, 0, 0);
            EPD_DisplayBase((void *)0);
            EPD_InitPartial();
        }
    }
}
