/**
 * @file    pl_node.c
 * @brief   pic_link 节点角色实现 (LG290 / blozi290, CC2530F256)
 *
 * RAM 策略: 不保存原图 (2756B), 收一包流式变换一包, 只留面板帧缓冲
 *   LG290/blozi290: EPD_Frame 4736B (16B x 296 行), 两板屏布局相同可通用
 *
 * 流程:
 *   START -> 复位计数 + 帧缓冲清白 (纯 RAM, 快, 不阻塞 RF)
 *   DATA  -> CRC 校验 -> pl_frame_write_290(seq*96, ...) -> rx_bytes += len
 *   END   -> rx_bytes==2756 且无坏包: EPD_Init + 全刷显示 + 深睡
 *            否则: 全黑显示报错 (EPD_Fill 0x00)
 *
 * 显示完成进深睡断电省电, 下一张图 START 后会重新 init
 */
#include <ioCC2530.h>
#include "pl_node.h"
#include "basic_rf.h"
#include "epd2in9.h"

void pl_node_run(void)
{
    static __xdata uint8 rfbuf[110];
    uint8  state = 0;           /* 0=空闲, 1=收图中 */
    uint16 rx_bytes = 0;
    uint8  bad = 0;
    uint16 seq;
    uint8  len, n;

    for (;;) {
        if (!basicRfPacketIsReady())
            continue;

        n = basicRfReceive((uint8 *)rfbuf, 110, NULL);
        if (n < 6)
            continue;

        /* 整包 CRC 校验 (TYPE..DATA, 末 2 字节为 CRC) */
        if (pl_crc16((const uint8 *)rfbuf, (uint16)(n - 2)) !=
            (uint16)((uint16)rfbuf[n - 2] << 8 | rfbuf[n - 1])) {
            bad++;
            continue;
        }

        len = rfbuf[3];
        if (n != (uint8)(6 + len))
            continue;                       /* 长度不符, 丢弃 */
        seq = (uint16)((uint16)rfbuf[1] << 8 | rfbuf[2]);

        switch (rfbuf[0]) {
        case PL_R_START:
            state    = 1;
            rx_bytes = 0;
            bad      = 0;
            EPD_FrameClear(0xFF);       /* 纯 RAM 清白, ~3ms 不丢包 */
            break;

        case PL_R_DATA:
            if (!state)
                break;
            /* 流式变换: 该块图为第 seq*96 字节起 len 字节 */
            pl_frame_write_290((uint16)(seq * PL_CHUNK), &rfbuf[4], len);
            rx_bytes += len;
            break;

        case PL_R_END:
            if (!state)
                break;
            state = 0;
            if (rx_bytes == PL_IMG_BYTES && bad == 0) {
                EPD_Init();                         /* 深睡后需重新上电 init */
                EPD_DisplayImage((const uint8 *)EPD_Frame);  /* 全刷 ~4s, 自动清残影 */
                EPD_Sleep();                        /* 深睡 + 断电 */
            } else {
                EPD_Init();
                EPD_Fill(0x00);                     /* 全黑 = 接收出错信号 */
                EPD_Sleep();
            }
            break;

        default:
            break;
        }
    }
}
