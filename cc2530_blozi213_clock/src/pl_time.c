/**
 * @file    pl_time.c
 * @brief   时钟节点 RF 校时接收实现
 *
 * RF 包 (网关 -> 节点): TYPE | SEQ_H SEQ_L | LEN | DATA(LEN) | CRC_H CRC_L
 *   校时包 TYPE = PL_R_TIME(0x14), LEN = PL_TIME_LEN(7),
 *   DATA = Y_H Y_L 月 日 时 分 秒 (年大端, 如 2026 = 0x07EA)
 *   CRC 覆盖 TYPE..DATA, 与网关 gw_rf_send() 用同一套 pl_crc16。
 * 收到合法包: clockface_set_date/ set_time + 整屏全刷重画。
 *
 * 说明: 主循环每秒调用一次 (见 clockface_run); 网关重发最多 3 次都在几十 ms 内,
 *       节点每秒取一次总还能拿到最后那包, 对时的 1 秒内延迟无影响。
 */
#include <ioCC2530.h>
#include "pl_time.h"
#include "pl_proto.h"
#include "basic_rf.h"
#include "clockface.h"

uint8 pl_time_init(void)
{
    basicRfCfg_t rf;

    rf.myAddr     = PL_ADDR_NODEB29;   /* 与网关转发目标一致 */
    rf.panId      = PL_PAN_ID;
    rf.channel    = PL_CHANNEL;
    rf.ackRequest = TRUE;

    if (basicRfInit(&rf) == FAILED)
        return 0;                      /* RF 起不来: 当独立时钟照跑 */
    basicRfReceiveOn();
    return 1;
}

uint8 pl_time_poll(void)
{
    static __xdata uint8 rfbuf[16];
    uint8  n;
    uint16 y;
    uint8  mo, d, hh, mi, ss;

    if (!basicRfPacketIsReady())
        return 0;

    n = basicRfReceive((uint8 *)rfbuf, 16, NULL);
    if (n < 6)
        return 0;

    /* 整包 CRC 校验 (TYPE..DATA, 末 2 字节 = CRC) */
    if (pl_crc16((const uint8 *)rfbuf, (uint16)(n - 2)) !=
        (uint16)((uint16)rfbuf[n - 2] << 8 | rfbuf[n - 1]))
        return 0;

    if (rfbuf[0] != PL_R_TIME)
        return 0;                      /* 不是校时包 (如推图包), 交给别人 */
    if (rfbuf[3] != PL_TIME_LEN || n != (uint8)(6 + PL_TIME_LEN))
        return 0;

    y  = (uint16)((uint16)rfbuf[4] << 8 | rfbuf[5]);
    mo = rfbuf[6];
    d  = rfbuf[7];
    hh = rfbuf[8];
    mi = rfbuf[9];
    ss = rfbuf[10];

    /* 合法性: 任一分量非法则整包丢弃 (网关已校验, 这里是二道防线) */
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        hh > 23 || mi > 59 || ss > 59)
        return 0;

    clockface_set_date(y, mo, d);
    clockface_set_time(hh, mi, ss);
    clockface_draw();                  /* 全刷重画 (校时是低频事件, 顺带清残影) */
    return 1;
}
