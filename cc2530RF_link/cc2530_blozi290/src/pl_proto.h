/**
 * @file    pl_proto.h
 * @brief   pic_link 三板组网推图 - 公共协议定义与图像变换
 *
 * 组网架构:
 *   PC(pic_push.py) --USB-TTL 115200--> LG213 网关 --BasicRF 2.4GHz--> LG290 / blozi290 节点
 *
 * 统一图源 (三板共用):
 *   212 x 104 黑白位图, 行优先位流, MSB first, bit=1 黑
 *   总量 = 212*104/8 = 2756 字节 = 29 包 x 96B (末包 68B)
 *   该格式即 LG213 (2.13" 三色) 的横屏像素排布, 其他屏做嵌入变换
 */
#ifndef __PL_PROTO_H__
#define __PL_PROTO_H__

#include "hal_types.h"

/* hal_types.h 已定义 uint8/uint16, 防止 epd2in9.h/fonts.h 重复 typedef */
#define _UINT8_T_DEFINED
#define _UINT16_T_DEFINED

/* ==================== 统一图源 ==================== */
#define PL_IMG_W        212
#define PL_IMG_H        104
#define PL_IMG_BYTES    ((PL_IMG_W * PL_IMG_H) / 8)                 /* 2756 */
#define PL_CHUNK        96                                          /* 每包图数据 */
#define PL_N_CHUNKS     ((PL_IMG_BYTES + PL_CHUNK - 1) / PL_CHUNK)  /* 29 */
#define PL_LAST_LEN     (PL_IMG_BYTES - (PL_N_CHUNKS - 1) * PL_CHUNK) /* 68 */

/* ==================== UART 帧 (PC <-> 网关, 115200 8N1) ====================
 * 格式: A5 5A | LEN_H LEN_L | TYPE | PAYLOAD(LEN 字节) | CRC_H CRC_L
 *   LEN  = PAYLOAD 字节数 (不含帧头/长度/类型/CRC), 最大 104
 *   CRC16-CCITT (poly 0x1021, init 0xFFFF), 覆盖 TYPE + PAYLOAD
 * 流控: 停等协议, PC 每帧等 ACK 再发下一帧 (超时 500ms 重发, 3 次失败整图重传)
 */
#define PL_U_SYNC_H     0xA5
#define PL_U_SYNC_L     0x5A
#define PL_U_MAX_PAY    104

#define PL_U_START      0x01    /* P: 总长 2B (=2756)          -> ACK(0xFFFF) */
#define PL_U_DATA       0x02    /* P: SEQ_H SEQ_L LEN DATA.. CRC_H CRC_L
                                        (块 CRC 覆盖 SEQ+LEN+DATA) -> ACK(seq) */
#define PL_U_END        0x03    /* P: 空                       -> ACK(0xFFFF) */
#define PL_U_ACK        0x80    /* P: SEQ_H SEQ_L (END/START 用 0xFFFF) */
#define PL_U_NAK        0x81    /* P: SEQ_H SEQ_L */

/* ==================== RF 包 (网关 -> 节点, BasicRF 单播) ====================
 * 格式: TYPE | SEQ_H SEQ_L | LEN | DATA(LEN) | CRC_H CRC_L
 *   CRC 覆盖 TYPE..DATA, RF payload 最大 4+96+2 = 102 <= BASIC_RF_MAX_PAYLOAD_SIZE(103)
 * 可靠性: BasicRF ackRequest=TRUE (802.15.4 MAC 硬件 ACK) + 块级 CRC
 * 节点侧: 每包校验 CRC, END 时核对总字节数, 错误则全黑显示
 */
#define PL_R_START      0x11
#define PL_R_DATA       0x12
#define PL_R_END        0x13

/* ==================== 网络参数 (三板一致) ==================== */
#define PL_PAN_ID       0xCAFE
#define PL_CHANNEL      20          /* 2.4GHz ch20, 避开 WiFi 1/6/11 */
#define PL_ADDR_GATEWAY 0x1213      /* LG213 网关 */
#define PL_ADDR_NODE290 0x1290      /* LG290 节点 */
#define PL_ADDR_NODEB29 0xB290      /* blozi 290 节点 */

/* ==================== API ==================== */
/* CRC16-CCITT (poly 0x1021, init 0xFFFF)
 * 注: IAR 8051 下 generic 指针参数, 传 __xdata 缓冲需显式 cast (icc8051 不做隐式提升) */
uint16 pl_crc16(const uint8 *p, uint16 len);

/* ---- 整图变换: 2756B 统一图源 -> EPD_Frame (面板原生布局) ----
 * 内部先清白帧, 再逐位写入。变换完成后调 EPD_DisplayImage()/EPD_ShowFrame()
 * 参数为 __xdata 指针 (IAR 8051 下 generic<->xdata 隐式转换报 Pe167) */
void pl_to_frame_213(const uint8 __xdata *img);   /* LG213: gate=211-px, src=py       */
void pl_to_frame_290(const uint8 __xdata *img);   /* LG290/blozi290: 居中嵌入 +42/+12 */

/* ---- 流式区间变换: RF 收到第 off 字节起的 len 字节即写帧 ----
 * 节点端 RAM 紧张 (LG290 帧缓冲 4736B), 不保存原图, 收一包变一包 */
void pl_frame_write_213(uint16 off, const uint8 __xdata *data, uint16 len);
void pl_frame_write_290(uint16 off, const uint8 __xdata *data, uint16 len);

#endif /* __PL_PROTO_H__ */
