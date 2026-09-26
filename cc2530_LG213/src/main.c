/**
 * @file    main.c
 * @brief   LG213 电子价签 (2.13" 三色墨水屏, 横屏 212x104) - 显示 + 走时演示
 *
 * 流程: 上电全刷白底打底 -> 主循环绘制 装饰图形 + 时钟(HH:MM), 每分钟走时一次并全刷
 * 驱动: 微雪 2in13b_V3 刷写序列 (全刷 ~15s), 帧缓冲 EPD_Frame + EPD_* 绘图 API
 * 注: 2in13b 三色屏只支持全刷, 故每分钟全刷一次 (无局刷)
 *
 * EPD 引脚 (同 LG290 板布局, 实测验证):
 *   BUSY=P1.2  RST=P1.1  DC=P0.2  CS=P0.4  SCLK=P0.5  SDI=P0.3
 *   PWR=P0.7 (EPD 供电 MOS, 高电平上电)
 *   BS=GND (4 线 SPI 模式)
 */
#include <ioCC2530.h>
#include "epd2in9.h"

/* ---------------- 延时 (32MHz 校准) ---------------- */
static void DelayMS(uint16 msec)
{
    uint16 i, j;
    for (i = 0; i < msec; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 系统时钟: 32MHz ---------------- */
static void clock_init(void)
{
    CLKCONCMD &= ~0x40;            /* 选 32MHz 晶振 */
    while (CLKCONSTA & 0x40)       /* 等稳定 */
        ;
    CLKCONCMD &= ~0x47;            /* 分频 1, 主频 32MHz */
}

/* ---------------- Timer1: 60 秒周期中断 (走时) ---------------- */
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
    T1CTL = 0x0C;              /* 128 分频 (先停表配置) */
    /* 32MHz/128 = 250kHz; T1CC0 = 31248 -> 62500 tick = 250ms/次
     * 240 次 x 250ms = 60s 整 (修正旧值 31248 实为 125ms/30s 的偏差) */
    T1CC0H = (31248 >> 8);
    T1CC0L = (31248 & 0xFF);
    T1CCTL0 = 0x44;            /* 通道0 比较模式 + 中断使能 */
    T1STAT = 0x00;             /* 清所有 T1 标志 */
    T1CTL = 0x0E;              /* 模模式 + 128分频, 启动 */
    IEN1 |= 0x02;              /* T1IE=1 (bit1) */
    EA = 1;                    /* 全局中断 */
}

int main(void)
{
    uint16 counter = 0;        /* 分钟计数 */

    clock_init();
    timer_init();

    EPD_Init();
    EPD_Fill(0xFF);            /* 全刷白底打底 */

    while (1)
    {
        /* 生成新帧: 白底 + 四角装饰图形 + 时钟 (横屏 212x104) */
        EPD_FrameClear(0xFF);

        /* 装饰图形: 矩形边框 / 填充矩形 / 圆 / 填充圆 */
        EPD_DrawRect(4, 4, 45, 36, 1);
        EPD_DrawFilledRect(4, 60, 45, 98, 1);
        EPD_DrawCircle(206, 20, 12, 1);
        EPD_DrawFilledCircle(206, 84, 12, 1);

        /* 时钟 HH:MM (Font24: 17x24, 5 字符共 85px)
         * x = (212-85)/2 = 63, 上部 y = 12 */
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
            EPD_DrawString(63, 12, tbuf, &Font24, 1);
        }

        /* 标签 (Font16: 11x16, "LG213 EPD" 9 字符共 99px)
         * x = (212-99)/2 = 56, 下部 y = 48 */
        {
            char lbuf[10];     /* 拷到 RAM, 避免 code 指针问题 */
            lbuf[0]='L'; lbuf[1]='G'; lbuf[2]='2'; lbuf[3]='1'; lbuf[4]='3';
            lbuf[5]=' '; lbuf[6]='E'; lbuf[7]='P'; lbuf[8]='D'; lbuf[9]='\0';
            EPD_DrawString(56, 48, lbuf, &Font16, 1);
        }

        /* 全刷显示 (~15s, 2in13b 仅支持全刷) */
        EPD_ShowFrame();

        /* 等 60 秒 (Timer1 置 minute_flag) */
        while (!minute_flag)
            ;
        minute_flag = 0;
        counter++;             /* 每分钟 +1 */
    }

    return 0;
}
