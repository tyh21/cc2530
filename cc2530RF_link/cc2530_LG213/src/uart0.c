/**
 * @file    uart0.c
 * @brief   USART1 UART Alt2 (TX=P1.6 RX=P1.7) 115200 8N1
 *          (文件名沿用 uart0, 实际外设是 USART1 -- 板上 P1.6/P1.7 焊盘
 *           是 USART1 Alt2 的 UART 脚, 不是 USART0 的!
 *           TI swrc135b hal_board.h 官方佐证: USART1 Alt2 SPI
 *           CLK=P1.5 MOSI=P1.6 MISO=P1.7, 同位置 UART: TX=MOSI 脚
 *           即 P1.6, RX=MISO 脚即 P1.7)
 *
 * 波特率 (32MHz, SWRU191F Table 23-2): U1BAUD=216, U1GCR=12 -> 115200
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
    /* 引脚: PERCFG.U1CFG=1 -> USART1 Alt2 (RX=P1.7, TX=P1.6) */
    PERCFG |= 0x02;             /* U1CFG=1: USART1 -> Alt2 */
    P1SEL  |= 0xC0;             /* P1.6/P1.7 = 外设功能 */
    P2SEL  &= ~0x20;            /* PRI2P1=0: USART1 优先于 Timer3 (TI 官方写法) */

    /* UART 模式 8N1 */
    U1CSR  |= 0x80;             /* MODE=1: UART */
    U1UCR   = 0x02;             /* FLUSH 清缓冲, 其余 0 = 8 数据位, 1 停止位, 无校验 */
    U1GCR   = 11;               /* BAUD_E  */
    U1BAUD  = 216;              /* BAUD_M  -> 115200 @ 32MHz
                                 * (验证: (256+216)*2^11*32M/2^28 = 115234 ~= 115200;
                                 *  E=12 会是 230400 -- 之前误写成 12 导致乱码!
                                 *  原厂例程 hal_board_cfg_RealApp.h: U1GCR=11, U1BAUD=216) */

    /* 中断接收 */
    URX1IF  = 0;                /* 清接收中断标志 */
    URX1IE  = 1;                /* IEN0.URX1IE */
    U1CSR  |= 0x40;             /* RE=1 接收使能 */
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
    U1CSR &= ~0x02;             /* 清 TX_BYTE */
    U1DBUF = b;
    while (!(U1CSR & 0x02))
        ;                       /* 等发送完成 */
}

/* ---------------- 诊断输出 ---------------- */
void uart0_puts(const char *s)
{
    while (*s)
        uart0_write((uint8)*s++);
}

void uart0_put_hex2(uint8 v)
{
    static const __code char hex[] = "0123456789ABCDEF";
    uart0_write((uint8)hex[v >> 4]);
    uart0_write((uint8)hex[v & 0x0F]);
}

void uart0_flush_rx(void)
{
    rx_tail = rx_head;
}

/* ---------------- 接收中断 ---------------- */
#pragma vector=URX1_VECTOR
__interrupt void uart1_rx_isr(void)
{
    uint8 next;
    URX1IF = 0;                 /* 清标志 */
    next = (rx_head + 1) & RXBUF_MSK;
    if (next != rx_tail) {
        rxbuf[rx_head] = U1DBUF;   /* 读 U1DBUF 自动清 RX_BYTE */
        rx_head = next;
    } else {
        (void)U1DBUF;           /* 满了, 丢弃并清标志位 */
    }
}
