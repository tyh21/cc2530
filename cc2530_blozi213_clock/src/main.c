/**
 * @file    main.c
 * @brief   blozi 213 电子价签 (2.13 寸 104x212 三色屏) 主程序
 *
 * 换屏说明 (2026-10-25):
 *   重新拿到的屏是 DA14585 日历工程那块 2.13" 屏, 驱动已按该工程
 *   EPD_2in13_V2.c 逐字节对齐重写 (见 epd2in13.c), 现在是时钟程序。
 *
 * 运行模式 (MAIN_MODE):
 *   0 = 时钟程序 (默认): 上电从 12:34 起软件走时, 每分钟刷新,
 *       显示 "日期 + 中文星期 + 大号数字 HH:MM"; 以后接对时程序即可
 *   1 = 原来的 pic_link 组网节点: 收网关推来的图, 全刷显示
 *
 * 屏:  104x212 (13B/行), 横屏 212x104
 * EPD: BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1 PWR=P0.3
 * LED: P0.0=红 P0.1=蓝 P0.2=绿 (低电平点亮)
 * RF : CC2530 片内 802.15.4, PAN=0xCAFE ch20, 节点地址 0xB290 (仅模式 1)
 */
#include <ioCC2530.h>
#include "basic_rf.h"
#include "pl_node.h"
#include "epd2in13.h"
#include "clockface.h"

/* 0 = 时钟程序, 1 = pic_link RF 节点 */
#define MAIN_MODE       0

/* ---------------- 板载 RGB LED ----------------
 * 引脚: P0.0 = 红   P0.1 = 蓝   P0.2 = 绿 ; 低电平点亮
 */
#define LED_ON_LVL      0
#if LED_ON_LVL
#define LED_ON          1
#define LED_OFF         0
#else
#define LED_ON          0
#define LED_OFF         1
#endif

#define LED_RED         P0_0
#define LED_BLUE        P0_1
#define LED_GREEN       P0_2

/* 上电点亮哪几路 (1=亮 0=灭) */
#define LED_BOOT_R      1
#define LED_BOOT_B      0
#define LED_BOOT_G      0

static void led_init(void)
{
    P0SEL &= ~0x07;                /* P0.0-0.2 设为通用 IO */
    P0DIR |=  0x07;                /* 输出 */
    LED_RED   = LED_OFF;
    LED_BLUE  = LED_OFF;
    LED_GREEN = LED_OFF;
}

/* 1 = 点亮该路 */
static void led_set(uint8 r, uint8 b, uint8 g)
{
    LED_RED   = r ? LED_ON : LED_OFF;
    LED_BLUE  = b ? LED_ON : LED_OFF;
    LED_GREEN = g ? LED_ON : LED_OFF;
}

/* ---------------- 系统时钟: 32MHz (BasicRF halMcuWaitUs 按此校准) ---------------- */
static void clock_init(void)
{
    CLKCONCMD &= ~0x40;            /* 选 32MHz 晶振 */
    while (CLKCONSTA & 0x40)       /* 等稳定 */
        ;
    CLKCONCMD &= ~0x47;            /* 分频 1, 主频 32MHz */
}

int main(void)
{
    clock_init();

    /* 上电点亮板载 RGB LED (状态指示) */
    led_init();
    led_set(LED_BOOT_R, LED_BOOT_B, LED_BOOT_G);

#if MAIN_MODE == 0
    /* ================= 时钟程序 (永不返回) ================= */
    clockface_run();
#else
    /* ================= pic_link RF 节点 ================= */
    {
        basicRfCfg_t rf;

        /* 上电白底打底: "节点就绪"信号 */
        EPD_Init();
        EPD_Fill(0xFF);
        EPD_Sleep();

        rf.myAddr     = PL_ADDR_NODEB29;
        rf.panId      = PL_PAN_ID;
        rf.channel    = PL_CHANNEL;
        rf.ackRequest = TRUE;
        if (basicRfInit(&rf) == FAILED) {
            /* RF init 失败刷黑底白 X 报错画面后停机 */
            EPD_Init();
            EPD_FrameClear(0x00);
            EPD_DrawLine(8, 8, 203, 95, 0);
            EPD_DrawLine(8, 95, 203, 8, 0);
            EPD_DisplayImage((const uint8 *)EPD_Frame);
            EPD_Sleep();
            for (;;)
                ;
        }
        basicRfReceiveOn();

        pl_node_run();             /* 永不返回 */
    }
#endif

    return 0;
}
