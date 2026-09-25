/**
 * @file    main.c
 * @brief   LG213 电子价签 (2.13" 三色 EPD) - pic_link 组网网关
 *
 * 角色: PC 串口推图的网关
 *   PC(pic_push.py) --USB-TTL 115200--> 本板 --BasicRF--> LG290 / blozi290
 *   同时自己显示收到的图 (三色屏全刷 ~15s)
 *
 * 硬件:
 *   串口: USART0 Alt2, TX=P1.7 RX=P1.6, 115200 8N1 (板上有焊盘, 免飞线)
 *   RF:   CC2530 片内 802.15.4, PAN=0xCAFE ch20, 地址 0x1213
 *   EPD:  BUSY=P1.2 RST=P1.1 DC=P0.2 CS=P0.4 SCLK=P0.5 SDI=P0.3 PWR=P0.7
 */
#include <ioCC2530.h>
#include "basic_rf.h"
#include "pl_gateway.h"
#include "uart0.h"
#include "epd2in9.h"

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
    basicRfCfg_t rf;

    clock_init();

    /* EPD 初始上电白底 (给用户"网关就绪"的直观信号) */
    EPD_Init();
    EPD_Fill(0xFF);
    EPD_Sleep();

    /* BasicRF 初始化: 网关地址 0x1213 */
    rf.myAddr     = PL_ADDR_GATEWAY;
    rf.panId      = PL_PAN_ID;
    rf.channel    = PL_CHANNEL;
    rf.ackRequest = TRUE;
    if (basicRfInit(&rf) == FAILED) {
        while (1)
            ;                      /* 射频初始化失败, 停机 */
    }
    basicRfReceiveOn();

    /* 串口 (网关专用) + 主循环 */
    uart0_init();
    pl_gateway_run();              /* 永不返回 */

    return 0;
}
