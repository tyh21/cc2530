/**
 * @file    pl_time.h
 * @brief   时钟节点 RF 校时接收 (网关 PL_R_TIME -> clockface 时间)
 *
 * 链路: PC(pc_settime.py) --USB-TTL 115200--> LG213 网关 --BasicRF--> 本节点
 * 节点地址 / PAN / 信道与网关一致: PL_ADDR_NODEB29 (0xB290) / 0xCAFE / ch20
 */
#ifndef __PL_TIME_H__
#define __PL_TIME_H__

#include "hal_types.h"

/* 初始化 RF 接收 (basicRfInit + basicRfReceiveOn)
 * 返回 1 = 成功; 0 = RF 起不来 (此时照常当独立时钟走, 只是不能校时) */
uint8 pl_time_init(void);

/* 非阻塞收一包 RF: 收到合法校时包 -> clockface 校时并整屏重画, 返回 1; 否则 0 */
uint8 pl_time_poll(void);

#endif /* __PL_TIME_H__ */
