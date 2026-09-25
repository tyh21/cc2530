/**
 * @file    main.c
 * @brief   LG213 电子价签 (2.13" 三色 EPD) - pic_link 组网网关
 *
 * 角色: PC 串口推图的网关
 *   PC(pic_push.py) --USB-TTL 115200--> 本板 --BasicRF--> LG290 / blozi290
 *   同时自己显示收到的图 (三色屏全刷 ~15s)
 *
 * 硬件:
 *   串口: USART1 Alt2, TX=P1.6 RX=P1.7, 115200 8N1 (P1.6/P1.7 焊盘)
 *   RF:   CC2530 片内 802.15.4, PAN=0xCAFE ch20, 地址 0x1213
 *   EPD:  BUSY=P1.2 RST=P1.1 DC=P0.2 CS=P0.4 SCLK=P0.5 SDI=P0.3 PWR=P0.7
 *
 * 调试: 上电后串口打印 boot 横幅与各阶段状态, 供串口调试工具观察
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

    /* 串口最先初始化: 之后每一步都能被 PC 侧观察到 */
    uart0_init();
    uart0_puts("\r\n[GW] LG213 pic_link gateway boot\r\n");

    /* EPD 初始上电白底 (给用户"网关就绪"的直观信号, 阻塞 ~15s) */
    uart0_puts("[GW] EPD white flash (~15s)...\r\n");
    EPD_Init();
    EPD_Fill(0xFF);
    EPD_Sleep();
    uart0_puts("[GW] EPD ok\r\n");

    /* BasicRF 初始化: 网关地址 0x1213 */
    rf.myAddr     = PL_ADDR_GATEWAY;
    rf.panId      = PL_PAN_ID;
    rf.channel    = PL_CHANNEL;
    rf.ackRequest = TRUE;
    if (basicRfInit(&rf) == FAILED) {
        uart0_puts("[GW] RF INIT FAIL - halted!\r\n");
        while (1)
            ;                      /* 射频初始化失败, 停机 */
    }
    basicRfReceiveOn();
    uart0_puts("[GW] READY 115200 8N1 TX=P1.6 RX=P1.7\r\n");

    /* 串口主循环 (永不返回) */
    pl_gateway_run();

    return 0;
}
