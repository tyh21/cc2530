/**
 * @file    uart0.h
 * @brief   USART1 UART Alt2 驱动 (LG213 网关: TX=P1.6 RX=P1.7, 115200 8N1)
 *          板上 P1.6/P1.7 焊盘即此串口 (USART1 Alt2), 中断收 + 轮询发, 128B 环形缓冲
 *
 * 波特率 (32MHz, SWRU191F Table 23-2): U1BAUD=216, U1GCR=11 -> 115200
 * (注意 E=12/M=216 是 230400, 勿混! 原厂例程同款: GCR=11, BAUD=216)
 */
#ifndef __UART0_H__
#define __UART0_H__

#include "hal_types.h"

void   uart0_init(void);
uint8  uart0_read(uint8 *b);        /* 1=取到 1 字节, 0=无数据 */
void   uart0_write(uint8 b);        /* 轮询发送 1 字节 */
void   uart0_puts(const char *s);         /* 诊断: 打印字符串 (generic 指针, 字面量直接传) */
void   uart0_put_hex2(uint8 v);           /* 诊断: 打印 2 位 HEX */
void   uart0_flush_rx(void);       /* 清空接收缓冲 */

#endif /* __UART0_H__ */
