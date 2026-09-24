/**
 * @file    main.c
 * @brief   blozi_290 电子价签 EPD 点屏测试
 *
 * 现象: 上电后屏做一次初始全刷 -> 之后白屏/黑屏交替全刷,
 *       RGB LED 指示状态: 红=忙(刷新中) 绿=本轮回合完成 3 秒
 *
 * 板载 RGB LED: R=P0.1  G=P0.2  B=P0.0 (低电平点亮)
 * EPD 引脚    : BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1
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

int main(void)
{
    clock_init();
    led_init();

    set_led(1, 0, 0);              /* 红: EPD 初始化中 */
    EPD_Init();

    while (1)
    {
        set_led(1, 0, 0);          /* 红: 正在刷白屏 */
        EPD_Fill(0xFF);            /* 白屏 */
        set_led(0, 1, 0);
        DelayMS(3000);

        set_led(0, 0, 1);          /* 蓝: 正在刷黑屏 */
        EPD_Fill(0x00);            /* 黑屏 */
        set_led(0, 1, 0);
        DelayMS(3000);
    }
}
