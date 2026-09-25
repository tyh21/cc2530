/**
 * @file    main.c
 * @brief   LG 电子价签 (WFT0290CZ10, 2.9 寸墨水屏) EPD 测试
 *
 * 流程: 全刷白底打底 -> 局刷模式 -> 图形+时钟+Relay 状态演示, 定期全刷去残影
 * 刷新序列: 微雪 EPD_2in9d (用户实测完美点亮)
 *
 * EPD 引脚 (实测 + 原厂固件逆向双重验证):
 *   BUSY=P1.2  RST=P1.1  DC=P0.2  CS=P0.4  SCLK=P0.5  SDI=P0.3
 *   PWR=P0.7 (EPD 供电 MOS, 高电平上电, 原固件 0x11735/0x11766)
 *   BS=GND (4 线 SPI 模式)
 * 注意: 该板 P0.0/P0.1 被原固件用于软件 I2C (RTC/传感器),
 *       原模板的 RGB LED (P0.0/P0.1/P0.2) 在此板上不可用, 已禁用。
 */
#include <ioCC2530.h>
#include <stdint.h>
#include "epd2in9.h"

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

/* ---------------- RGB LED (此板无可用 LED, 空实现保留接口) ---------------- */
static void led_init(void)
{
    /* LG 板 P0.0/P0.1 为软件 I2C 总线, P0.2 为 EPD DC, 不做 LED */
}

/* ---------------- Timer1: 周期中断 ---------------- */
static volatile uint8  minute_flag = 0;
static volatile uint16 isr_cnt = 0;

#pragma vector=T1_VECTOR
__interrupt void T1_ISR(void)
{
    T1STAT &= ~0x01;           /* 清 CH0IF (bit0) */
    if (++isr_cnt >= 240) { isr_cnt = 0; minute_flag = 1; }
}

static void timer_init(void)
{
    T1CTL = 0x0C;              /* 128 分频 */
    T1CC0H = (31248 >> 8);     /* 60s */

    T1CC0L = (31248 & 0xFF);
    T1CCTL0 = 0x44;            /* 通道0 比较模式 + 中断使能 */
    T1STAT = 0x00;             /* 清所有 T1 标志 */
    T1CTL = 0x0E;              /* 模模式 + 128分频, 启动 */
    IEN1 |= 0x02;              /* T1IE=1 (bit1) */
    EA = 1;                    /* 全局中断 */
}

int main(void)
{
    uint16 counter = 0;
    uint8  relay_state = 0;

    clock_init();
    led_init();
    timer_init();


    EPD_Init();

    /* 全刷白底打底 (0x10 旧 RAM 写黑, 0x13 新 RAM 写白, 微雪 Clear 同款) */

    EPD_DisplayBase((void *)0);    /* NULL = 全白 */

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
            uint16 hh = (counter / 60U) % 100U;   /* 小时 0-99 */
            uint16 mm = counter % 60U;            /* 分钟 0-59 */
            tbuf[0] = (char)(hh / 10U + '0');
            tbuf[1] = (char)(hh % 10U + '0');
            tbuf[2] = ':';
            tbuf[3] = (char)(mm / 10U + '0');
            tbuf[4] = (char)(mm % 10U + '0');
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

        /* 局刷: 装 2in9d 局刷 LUT + 部分窗, 只写 0x13 新 RAM */

        EPD_ShowFrame();

        /* [诊断] LED 交替, 看程序是否活 */

        DelayMS(200);

        DelayMS(200);

        /* 等 60 秒 (Timer1 置 minute_flag) */
        while (!minute_flag) ;
        minute_flag = 0;

        counter++;                     /* 每分钟 +1 */
        if (counter >= 10U) {        /* 10分钟翻转 Relay */
            counter = 0;
            relay_state = !relay_state;
        }

        /* 每 30 帧做一次全刷去残影 */
        if ((counter % 10U) == 0U)
        {

            EPD_DisplayBase((void *)0);   /* 全刷去残影 (下次局刷自动重装局刷 LUT) */
        }
    }
}







