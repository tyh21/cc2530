/**
 * @file    uart0.c
 * @brief   USART0 UART Alt2 (TX=P1.7 RX=P1.6) 115200 8N1
 */
#include <ioCC2530.h>
#include "uart0.h"

#define RXBUF_SZ    128
#define RXBUF_MSK   (RXBUF_SZ - 1)

static volatile __xdata uint8 rxbuf[RXBUF_SZ];
static volatile uint8 rx_head = 0;
static volatile uint8 rx_tail = 0;

void uart0_init(void)
{
    /* 引脚: PERCFG.U0CFG=1 -> Alt2 (RX=P1.6, TX=P1.7) */
    PERCFG |= 0x01;
    P1SEL  |= 0xC0;             /* P1.6/P1.7 = 外设功能 */
    P2DIR  &= ~0xC0;            /* PRIP1=00: USART0 在 Port1 优先级最高 (默认) */

    /* UART 模式 8N1 */
    U0CSR  |= 0x80;             /* MODE=1: UART */
    U0UCR   = 0x02;             /* FLUSH 清缓冲, 其余 0 = 8 数据位, 1 停止位, 无校验 */
    U0GCR   = 12;               /* BAUD_E  */
    U0BAUD  = 216;              /* BAUD_M  -> 115200 @ 32MHz */

    /* 中断接收 */
    URX0IF  = 0;                /* 清接收中断标志 (IRCON.2) */
    URX0IE  = 1;                /* IEN0.URX0IE */
    U0CSR  |= 0x40;             /* RE=1 接收使能 */
    EA      = 1;
}

uint8 uart0_read(uint8 *b)
{
    if (rx_head == rx_tail)
        return 0;
    *b = rxbuf[rx_tail];
    rx_tail = (rx_tail + 1) & RXBUF_MSK;
    return 1;
}

void uart0_write(uint8 b)
{
    U0CSR &= ~0x02;             /* 清 TX_BYTE */
    U0DBUF = b;
    while (!(U0CSR & 0x02))
        ;                       /* 等发送完成 */
}

void uart0_flush_rx(void)
{
    rx_tail = rx_head;
}

/* ---------------- 接收中断 ---------------- */
#pragma vector=URX0_VECTOR
__interrupt void uart0_rx_isr(void)
{
    uint8 next;
    URX0IF = 0;                 /* 清标志 */
    next = (rx_head + 1) & RXBUF_MSK;
    if (next != rx_tail) {
        rxbuf[rx_head] = U0DBUF;   /* 读 U0DBUF 自动清 RX_BYTE */
        rx_head = next;
    } else {
        (void)U0DBUF;           /* 满了, 丢弃并清标志位 */
    }
}
