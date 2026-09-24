/**
 * @file    main.c
 * @brief   blozi_290 电子价签 EPD 局刷测试
 *
 * 流程: 全刷白底打底 -> 切局刷模式 -> 8x16 计数器每秒局刷跳数 (无闪烁)
 * RGB LED 指示: 红=初始化/全刷  蓝=局刷中  绿=空闲
 *
 * 局刷方案: Waveshare 原味 (芯片内部差分), 只写 0x24 + 0x0C 快刷
 *           不需要双帧缓冲, 单帧 4736B 即可
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

/* ---------------- 横屏数字绘制 ----------------
 * 用驱动层 EPD_SetPixel 逐点画, 字模自动随旋转方向映射
 * (x, y) 为横屏坐标左上角: x 0-288, y 0-112
 */
static void draw_digit_land(uint8 d, uint16 x, uint8 y)
{
    uint8  cx, cy;
    for (cy = 0; cy < 16; cy++)
    {
        uint8 line = font8x16[d][cy];
        for (cx = 0; cx < 8; cx++)
        {
            /* 字模 0=黑; bit=1 -> 白 */
            EPD_SetPixel(x + cx, y + cy, (uint8)!((line >> (7 - cx)) & 1));
        }
    }
}

/* ---------------- Timer1: 250ms (仅定义, 未调用) ---------------- */
static void timer_init(void)
{
    T1CTL = 0x0C;              /* 128 分频 */
    T1CC0H = (62500 >> 8);     /* 250ms */
    T1CC0L = (62500 & 0xFF);
    T1CTL = 0x0E;              /* 模模式 + 128分频, 启动 */
}

int main(void)
{
    uint16 counter = 0;
    uint8  digit;
    uint8  relay_state = 0;

    clock_init();
    led_init();

    set_led(1, 0, 0);              /* 红: 初始化 */
    EPD_Init();

    /* 全刷白底打底 (old RAM 也写白, 局刷参考) */
    set_led(1, 0, 0);
    EPD_DisplayBase((void *)0);    /* NULL = 全白, 内部已切回局刷 LUT */

    while (1)
    {
        /* 生成新帧: 白底 + 4 位计数器 (横屏居中, 8x16 字体)
         * 横屏 296x128: 4 数字共 32+8*0=32 像素宽, x=(296-32)/2=132
         * 字高 16, y=(128-16)/2=56
         */
        EPD_FrameClear(0xFF);

        /* 静态图形: 矩形边框 / 填充矩形 / 圆 / 填充圆 */
        EPD_DrawRect(8, 8, 55, 50, 1);
        EPD_DrawFilledRect(8, 70, 55, 112, 1);
        EPD_DrawCircle(268, 28, 18, 1);
        EPD_DrawFilledCircle(268, 100, 18, 1);

        /* 时钟 MM:SS (Font24) */
        {
            char tbuf[6];
            tbuf[0] = (char)((counter / 600U) % 10U + '0');
            tbuf[1] = (char)((counter / 60U) % 10U + '0');
            tbuf[2] = ':';
            tbuf[3] = (char)((counter / 10U) % 6U + '0');
            tbuf[4] = (char)(counter % 10U + '0');
            tbuf[5] = '\0';
            EPD_DrawString(105, 10, tbuf, &Font24, 1);
        }

        /* Relay 状态 (Font16) - 从 code 拷到 RAM, 避免 code 指针死机 */
        {
            char rbuf[11];
            rbuf[0]='R'; rbuf[1]='e'; rbuf[2]='l'; rbuf[3]='a'; rbuf[4]='y';
            rbuf[5]=':'; rbuf[6]=' ';
            if (relay_state) { rbuf[7]='O'; rbuf[8]='N'; rbuf[9]=' '; }
            else             { rbuf[7]='O'; rbuf[8]='F'; rbuf[9]='F'; }
            rbuf[10] = '\0';
            EPD_DrawString(90, 90, rbuf, &Font16, 1);
        }

        /* 局刷: 只写 0x24 + 0x0C 快刷 */
        set_led(0, 0, 1);          /* 蓝: 局刷中 */
        EPD_ShowFrame();

        set_led(0, 1, 0);          /* 绿: 空闲 1 秒 */
        DelayMS(10000);
        counter++;
        if (counter >= 6000U) {        /* 60 秒翻转一次 Relay */
            counter = 0;
            relay_state = !relay_state;
        }

        /* 每 30 帧做一次全刷去残影 */
        if ((counter % 10U) == 0U)
        {
            set_led(1, 0, 0);
            EPD_DisplayBase((void *)0);   /* 内部全刷 + 切回局刷 LUT */
        }
    }
}




