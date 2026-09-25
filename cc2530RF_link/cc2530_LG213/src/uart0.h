/**
 * @file    uart0.h
 * @brief   USART0 UART Alt2 驱动 (LG213 网关: TX=P1.7 RX=P1.6, 115200 8N1)
 *          三板通用映射 (P1.6/P1.7 焊盘), 中断收 + 轮询发, 128B 环形缓冲
 *
 * 波特率 (32MHz, SWRU191F Table 23-2): U0BAUD=216, U0GCR=12 -> 115200
 */
#ifndef __UART0_H__
#define __UART0_H__

#include "hal_types.h"

void   uart0_init(void);
uint8  uart0_read(uint8 *b);        /* 1=取到 1 字节, 0=无数据 */
void   uart0_write(uint8 b);        /* 轮询发送 1 字节 */
void   uart0_flush_rx(void);       /* 清空接收缓冲 */

#endif /* __UART0_H__ */
