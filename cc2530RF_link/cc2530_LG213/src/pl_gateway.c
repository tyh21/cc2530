/**
 * @file    pl_gateway.c
 * @brief   pic_link 网关角色实现 (LG213, CC2530F256)
 *
 * 主流程 (停等协议, PC 每帧等 ACK):
 *   1. UART 收 START(总长 2756)      -> RF 发 START 到两节点 -> ACK
 *   2. 循环收 DATA(seq,len,data,crc) -> 存 imgbuf + RF 发 DATA -> ACK(seq)
 *   3. UART 收 END                   -> RF 发 END -> ACK
 *   4. 自显: pl_to_frame_213 -> EPD 全刷 -> 深睡
 *
 * 任何一步校验失败/超时: 丢弃当前图, 跳回等 START
 * (PC 端 500ms 无 ACK 即重发当前帧, 3 次失败自动整图重传, 状态机可自愈)
 *
 * IAR 8051 指针说明: icc8051 不做 __xdata -> generic 的隐式提升 (Pe167),
 * 调 generic 指针参数的库函数/BasicRF API 时一律显式 cast。
 */
#include <ioCC2530.h>
#include <string.h>
#include "pl_gateway.h"
#include "uart0.h"
#include "basic_rf.h"
#include "epd2in9.h"

/* ---------------- 延时 (32MHz 粗校准, 用于接收超时) ---------------- */
static void gw_delay_ms(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

/* ---------------- 大缓冲 (xdata: imgbuf 2756 + pay 104 + rf 104 + crc 105
 *                            + EPD_Frame 2756 = ~6.1KB / 8KB SRAM) ---------------- */
static __xdata uint8 imgbuf[PL_IMG_BYTES];      /* 图缓冲 2756B */
static __xdata uint8 paybuf[PL_U_MAX_PAY];      /* UART 帧 payload */
static __xdata uint8 rfbuf[104];                /* RF 包缓冲 */
static __xdata uint8 crcbuf[PL_U_MAX_PAY + 1];  /* CRC 计算暂存 (两函数共用, 不并发) */

/* ---------------- UART 读 1 字节 (带超时) ---------------- */
static uint8 gw_read_byte(uint16 tmo_ms, uint8 *b)
{
    while (tmo_ms--) {
        if (uart0_read(b))
            return 1;
        gw_delay_ms(1);
    }
    return 0;
}

/* ---------------- 收一帧 UART (同步头自适应) ----------------
 * 返回 1: paybuf 有效, *plen = payload 长度, *ptype = 帧类型
 * 返回 0: 超时或 CRC 错
 */
static uint8 gw_wait_frame(uint8 *ptype, uint8 *plen)
{
    uint8 b, prev = 0;
    uint16 len, crc_calc, crc_rx;
    uint16 i;

    /* 同步: 找 A5 5A (跨字节边界自适应) */
    for (;;) {
        if (!gw_read_byte(800, &b))
            return 0;
        if (prev == PL_U_SYNC_H && b == PL_U_SYNC_L) {
            uart0_puts("[GW] sync\r\n");
            break;
        }
        prev = b;
    }

    /* 长度 */
    if (!gw_read_byte(50, &b)) { uart0_puts("[GW] tmo:len\r\n");  return 0; }
    len = (uint16)(b << 8);
    if (!gw_read_byte(50, &b)) { uart0_puts("[GW] tmo:len\r\n");  return 0; }
    len |= b;
    if (len > PL_U_MAX_PAY) {           /* len==0 合法: END 是零长帧, 勿拒! */
        uart0_puts("[GW] bad len\r\n");
        return 0;
    }

    /* 类型 */
    if (!gw_read_byte(50, ptype)) { uart0_puts("[GW] tmo:type\r\n"); return 0; }

    /* payload */
    for (i = 0; i < len; i++) {
        if (!gw_read_byte(50, (uint8 *)&paybuf[i])) {
            uart0_puts("[GW] tmo:data\r\n");
            return 0;
        }
    }

    /* CRC (覆盖 TYPE+PAYLOAD) */
    crcbuf[0] = *ptype;
    memcpy((void *)&crcbuf[1], (void const *)paybuf, len);
    crc_calc = pl_crc16((const uint8 *)crcbuf, (uint16)(len + 1));

    if (!gw_read_byte(50, &b)) { uart0_puts("[GW] tmo:crc\r\n");  return 0; }
    crc_rx = (uint16)(b << 8);
    if (!gw_read_byte(50, &b)) { uart0_puts("[GW] tmo:crc\r\n");  return 0; }
    crc_rx |= b;

    if (crc_calc != crc_rx) {
        uart0_puts("[GW] CRC err\r\n");
        return 0;
    }

    uart0_puts("[GW] RX t=");
    uart0_put_hex2(*ptype);
    uart0_puts(" n=");
    uart0_put_hex2((uint8)len);
    uart0_puts("\r\n");

    *plen = (uint8)len;
    return 1;
}

/* ---------------- UART 发帧 (回 ACK) ---------------- */
static void gw_send_frame(uint8 type, const uint8 __xdata *pay, uint8 len)
{
    uint16 crc;
    uint8 i;

    uart0_write(PL_U_SYNC_H);
    uart0_write(PL_U_SYNC_L);
    uart0_write(0x00);
    uart0_write(len);
    uart0_write(type);

    crcbuf[0] = type;
    memcpy((void *)&crcbuf[1], (void const *)pay, len);

    for (i = 0; i < len; i++)
        uart0_write(pay[i]);

    crc = pl_crc16((const uint8 *)crcbuf, (uint16)(len + 1));
    uart0_write((uint8)(crc >> 8));
    uart0_write((uint8)(crc & 0xFF));
}

static void gw_send_ack(uint16 seq)
{
    static __xdata uint8 p[2];
    p[0] = (uint8)(seq >> 8);
    p[1] = (uint8)(seq & 0xFF);
    gw_send_frame(PL_U_ACK, p, 2);
}

/* ---------------- 两节点送达失败计数 (每张图 END 时串口汇总打印) -------- */
static uint16 gw_290_fail = 0;
static uint16 gw_b29_fail = 0;

/* ---------------- RF 发送 (两节点各一包, 各自重发) ----------------
 * RF payload: TYPE | SEQ_H SEQ_L | LEN | DATA(LEN) | CRC_H CRC_L
 * CRC 覆盖 TYPE..DATA, 最大 4+96+2 = 102 <= BASIC_RF_MAX_PAYLOAD_SIZE(105)
 *
 * [2026-09-25 修复 "推图时节点无反应"]
 * BasicRF 的 MAC ACK 是硬件级可靠送达: basicRfSendPacket 返回 SUCCESS
 * = 节点 RF 层已收到且硬件 CRC 通过。原代码 (void) 忽略返回值,
 * 任何一包无 ACK 即静默丢包 -> 节点 END 校验失败 -> 刷全黑报错。
 * 现在两节点各自重发, 最多 3 次; 3 连败打告警并计数。
 * 节点已加包位图去重, 重发不会重复计数。
 */
static void gw_rf_send_one(uint16 addr, uint8 type, uint16 seq, uint8 plen)
{
    uint8 try;
    for (try = 0; try < 3; try++) {
        if (basicRfSendPacket(addr, (uint8 *)rfbuf, plen) == SUCCESS)
            return;                         /* MAC ACK = 已收到 */
        gw_delay_ms(2);
    }
    /* 3 连败: 计数 + 告警 */
    if (addr == PL_ADDR_NODE290) gw_290_fail++; else gw_b29_fail++;
    uart0_puts(addr == PL_ADDR_NODE290 ? "[GW] !! 290 TX FAIL" : "[GW] !! B29 TX FAIL");
    uart0_puts(" t=");
    uart0_put_hex2(type);
    uart0_puts(" s=");
    uart0_put_hex2((uint8)seq);
    uart0_puts("\r\n");
}

static void gw_rf_send(uint8 type, uint16 seq, const uint8 __xdata *data, uint8 len)
{
    uint16 crc;
    uint8  plen = (uint8)(6 + len);

    rfbuf[0] = type;
    rfbuf[1] = (uint8)(seq >> 8);
    rfbuf[2] = (uint8)(seq & 0xFF);
    rfbuf[3] = len;
    memcpy((void *)&rfbuf[4], (void const *)data, len);
    crc = pl_crc16((const uint8 *)rfbuf, (uint16)(4 + len));
    rfbuf[4 + len]     = (uint8)(crc >> 8);
    rfbuf[5 + len]     = (uint8)(crc & 0xFF);

    gw_rf_send_one(PL_ADDR_NODE290, type, seq, plen);
    gw_rf_send_one(PL_ADDR_NODEB29, type, seq, plen);
}

/* ==================== 网关主循环 ==================== */
void pl_gateway_run(void)
{
    uint8 type, plen;
    uint16 seq, rseq, crc_calc, crc_rx;
    uint8  clen, expect;

    for (;;) {
        /* ---- 1. START ---- */
        if (!gw_wait_frame(&type, &plen))
            continue;
        if (type != PL_U_START || plen != 2) {
            uart0_puts("[GW] want START\r\n");
            continue;
        }
        if (((uint16)paybuf[0] << 8 | paybuf[1]) != PL_IMG_BYTES) {
            uart0_puts("[GW] bad img size\r\n");
            continue;
        }

        uart0_puts("[GW] START ok -> RF fwd + ACK\r\n");
        gw_290_fail = 0;
        gw_b29_fail = 0;                 /* 每张图独立统计 */
        gw_rf_send(PL_R_START, 0, paybuf, 2);
        gw_send_ack(0xFFFF);

        /* ---- 2. DATA x 29 (停等) ---- */
        for (seq = 0; seq < PL_N_CHUNKS; seq++) {
            if (!gw_wait_frame(&type, &plen))
                goto drop;
            if (type != PL_U_DATA) {
                uart0_puts("[GW] want DATA\r\n");
                goto drop;
            }
            /* payload: SEQ_H SEQ_L LEN DATA.. CRC_H CRC_L */
            if (plen < 5)
                goto drop;
            rseq = (uint16)(paybuf[0] << 8 | paybuf[1]);
            clen = paybuf[2];
            expect = (seq == PL_N_CHUNKS - 1) ? PL_LAST_LEN : PL_CHUNK;
            if (rseq != seq || clen != expect ||
                plen != (uint8)(3 + clen + 2))
                goto drop;

            /* 块 CRC 校验 (SEQ+LEN+DATA) */
            crc_calc = pl_crc16((const uint8 *)paybuf, (uint16)(3 + clen));
            crc_rx   = (uint16)(paybuf[3 + clen] << 8 | paybuf[4 + clen]);
            if (crc_calc != crc_rx)
                goto drop;

            /* 存图缓冲 + RF 转发 (纯图数据)
             * [2026-09-25 修复全黑 bug] 原来发 &paybuf[2]/clen+1, 把 UART 块内
             * 的 LEN 字节也带进了 RF DATA; 节点按 "rfbuf[3]=图数据长度,
             * rfbuf[4..]=纯图数据" 解析 -> 每包多算 1 字节, rx_bytes=2785!=2756,
             * END 校验失败 -> 节点刷全黑报错。改发纯图数据(&paybuf[3], clen)。 */
            memcpy((void *)&imgbuf[seq * PL_CHUNK], (void const *)&paybuf[3], clen);
            gw_rf_send(PL_R_DATA, seq, &paybuf[3], clen);
            gw_send_ack(seq);
            uart0_puts("[GW] D=");
            uart0_put_hex2((uint8)seq);
            uart0_puts("\r\n");
        }

        /* ---- 3. END ---- */
        if (!gw_wait_frame(&type, &plen))
            continue;
        if (type != PL_U_END || plen != 0) {
            uart0_puts("[GW] want END\r\n");
            continue;
        }
        uart0_puts("[GW] END ok -> display ~15s\r\n");
        gw_rf_send(PL_R_END, 0, paybuf, 0);
        gw_send_ack(0xFFFF);

        /* 两节点送达汇总: 正常 OK, 异常打印失败次数 (查电池/距离/天线) */
        if (gw_290_fail != 0) {
            uart0_puts("[GW] !! 290 unreachable, TX fail x");
            uart0_put_hex2((uint8)(gw_290_fail >> 8));
            uart0_put_hex2((uint8)gw_290_fail);
            uart0_puts("\r\n");
        } else {
            uart0_puts("[GW] 290 all MAC-ACK ok\r\n");
        }
        if (gw_b29_fail != 0) {
            uart0_puts("[GW] !! B29 unreachable, TX fail x");
            uart0_put_hex2((uint8)(gw_b29_fail >> 8));
            uart0_put_hex2((uint8)gw_b29_fail);
            uart0_puts("\r\n");
        } else {
            uart0_puts("[GW] B290 all MAC-ACK ok\r\n");
        }

        /* ---- 4. 自显 (三色屏全刷 ~15s, 全刷自动清残影) ----
         * 此时 PC 已收完 ACK, 15s 刷屏不阻塞停等协议;
         * 上一张图后已深睡, 重新 init 上电 */
        pl_to_frame_213(imgbuf);
        EPD_Init();
        EPD_DisplayImage((const uint8 *)EPD_Frame);
        EPD_Sleep();
        continue;

drop:
        uart0_puts("[GW] DROP, wait START\r\n");
        gw_delay_ms(20);        /* 让 PC 超时重发, 本图作废 */
    }
}
