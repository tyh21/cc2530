/**
 * @file    pl_gateway.h
 * @brief   pic_link 网关角色 (LG213): UART 收图 -> BasicRF 转发两节点 -> 自显
 */
#ifndef __PL_GATEWAY_H__
#define __PL_GATEWAY_H__

#include "pl_proto.h"

void pl_gateway_run(void);      /* 永不返回 */

#endif /* __PL_GATEWAY_H__ */
