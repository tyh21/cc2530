/**
 * @file    main.c
 * @brief   blozi 2.9 寸电子价签 (GDEH029A1, SSD1608) - pic_link 组网节点
 *
 * 角色: RF 接收节点, 收网关 (LG213) 转发的图, 流式变换后全刷显示
 *   屏: GDEH029A1, 面板 128x296 (16B/行 x 296 行), 横屏 296x128
 *   统一图源 212x104 居中嵌入 (+42, +12), 与 LG290 帧缓冲布局相同
 *
 * RF:   CC2530 片内 802.15.4, PAN=0xCAFE ch20, 节点地址 0xB290
 * EPD:  BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1 PWR=P0.3
 * 显示: 全刷 ~3s; 显示后深睡断电, 下一张图自动唤醒
 */
#include <ioCC2530.h>
#include "basic_rf.h"
#include "pl_node.h"
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

    /* 上电白底打底: "节点就绪"信号 */
    EPD_Init();
    EPD_Fill(0xFF);
    EPD_Sleep();

    /* BasicRF 初始化: 节点地址 0xB290 */
    rf.myAddr     = PL_ADDR_NODEB29;
    rf.panId      = PL_PAN_ID;
    rf.channel    = PL_CHANNEL;
    rf.ackRequest = TRUE;
    if (basicRfInit(&rf) == FAILED) {
        while (1)
            ;                      /* 射频初始化失败, 停机 */
    }
    basicRfReceiveOn();

    pl_node_run();                 /* 永不返回 */

    return 0;
}
