#ifndef __V45_WAKE_H__
#define __V45_WAKE_H__

#if defined(V45_WAKE) && (V45_WAKE != 0)

/* ===========================================================================
 * ★★★ v45 唤醒实测固件 —— "把 NFC 唤醒并验证到底吃哪套协议" ★★★
 *
 * 设计目标(相对 v40-v44 的 15 步大诊断做减法):
 *   1. 干净唤醒序列(核心):
 *        放电 100ms(VDDEX 驱低) -> P2.3 拉高(单次/原厂脉冲两模式)
 *        -> tBoot 50ms -> 记录 p23/nirq/总线电平 -> 接管总线
 *   2. 8 组合一次性测完所有未知数:
 *        swap {0=SDA P0.0, 1=SDA P0.1} x slv {0x54 手册默认, 0x55 原厂值}
 *        x proto {STD 手册标准协议(0x08 命令码), FAC 原厂直接块寻址}
 *   3. 双通道输出: 串口(115200, P1.6)实时打印每一步 + EPD 结果页。
 *   4. 保留 v44 的看门狗 + __no_init 面包屑: 卡死自报告, CRASH 页定位。
 *
 * 为什么加 STD(手册标准 READ 0x08):
 *   44 版一直在复刻原厂"直接块寻址", 板端却始终"地址 ACK + 第二字节 NACK"。
 *   手册 Table 5-5: 从地址后面跟的是**命令码**(READ 0x08|addrH|addrL|len),
 *   处理完芯片拉低 NIRQ, 再读回 状态+数据。
 *   若芯片跑的是手册协议, "块地址当第二字节"被 NACK 就是必然 —— 这条
 *   从 v31 修正真 ACK 判定之后就一直存在、但从未被完整测过的路, 今天补上。
 *
 * 结果字符:  '.'=probe 未过  'W'=写事务NACK  'Q'=NIRQ超时  'R'=读NACK
 *            'S'=状态错  'D'=数据OK  'B'=FAC 2nd NACK  'F'=FAC 全FF
 * =========================================================================== */

/* ---------------- 结果码 ---------------- */
#define W45_R_SKIP   0u   /* probe 写方向无真 ACK, 组合跳过        */
#define W45_R_WNACK  1u   /* STD: 写事务(命令帧)中途 NACK           */
#define W45_R_QTOUT  2u   /* STD: NIRQ 250ms 内没被芯片拉低          */
#define W45_R_RNACK  3u   /* STD: 读事务地址字节 NACK                */
#define W45_R_STBAD  4u   /* STD: 状态字节 CMD_RES != 0x5            */
#define W45_R_OK     5u   /* 数据有效                                */
#define W45_R_FACB   6u   /* FAC: 地址 ACK 但 2nd/3rd 字节 NACK      */
#define W45_R_FACF   7u   /* FAC: 三拍全 ACK 但数据全 FF/00 (无数据) */

/* ---------------- 结果快照 (每轮 SEQ 重算) ---------------- */
static __xdata uint8  w_res[8];      /* c = sw*4 + sv*2 + pf            */
static __xdata uint8  w_dbgS[8];     /* STD 协议每拍判定(最佳组合)      */
static __xdata uint8  w_blkS[16];   /* STD 协议读回的数据              */
static __xdata uint8  w_dbgF[4];     /* FAC 协议每拍判定(最佳组合)      */
static __xdata uint8  w_blkF[16];   /* FAC 协议读回的数据              */
static uint8  w_nirqS;              /* STD: NIRQ 等待 ms (0xFF=超时)   */
static uint8  w_bestS;              /* STD 详情来自哪个组合 (0xFF=无)  */
static uint8  w_bestF;              /* FAC 详情来自哪个组合 (0xFF=无)  */
static uint8  w_p23;                /* 唤醒后 P2.3 读回 (推挽真电平)    */
static uint8  w_nirq;               /* 唤醒后 NIRQ 电平                */
static uint8  w_line;                /* 唤醒后 SDA/SCL 电平(三态=外部上拉) */
static uint8  w_pwrmode;            /* 0=单次拉高 1=原厂复位脉冲        */
static uint8  w_rfMark;             /* 1=本 seq 由 RF 场触发(打印后清零) */
static uint16 w_seq;                /* 已跑 SEQ 轮数                   */
static uint16 w_rfSeq;              /* 由 RF 场触发的 SEQ 轮数          */
static uint16 w_polls;              /* 累计 RF 观测采样数              */
static uint16 w_hits;               /* 本窗口 P2.3=1 的采样数          */
static uint16 w_slots;              /* 本窗口总采样数                  */

/* ---------------- v44 面包屑机制 (看门狗复位不清零) ---------------- */
__no_init uint8  w_brFlag;          /* 0xA5 = 面包屑有效 */
__no_init uint8  w_brStep;          /* 卡在第几步 */
__no_init uint16 w_brN;             /* 累计看门狗复位次数 */
__no_init uint16 w_brOvf;           /* 当时的 T1 等待超时次数 */
__no_init uint16 w_brT1Prev;
__no_init uint16 w_brT1Last;        /* 两者相等 = T1 停了 */

#define W45_BR_MAGIC 0xA5u

/* ---------------- 引脚控制 ---------------- */
static void w45_p23_float(void)
{
    P2SEL &= (uint8)~0x02u;   /* P2SEL.1 = P2.3 功能位(0=GPIO) */
    P2INP |= 0x08u;           /* 输入三态: 只看外部(RF 场耦合)   */
    P2DIR &= (uint8)~0x08u;
}
static void w45_p23_hi(void)
{
    P2SEL &= (uint8)~0x02u;
    P2INP |= 0x08u;
    P2DIR |= 0x08u;
    P2    |= 0x08u;
}
static void w45_p23_low(void)
{
    P2SEL &= (uint8)~0x02u;
    P2INP |= 0x08u;
    P2DIR |= 0x08u;
    P2    &= (uint8)~0x08u;
}
static void w45_bus_float(void)
{
    P0SEL &= (uint8)~0x03u;
    P0INP |= 0x03u;           /* P0.0/P0.1 三态 */
    P0DIR &= (uint8)~0x03u;
    P2INP |= 0x20u;           /* PDUP0=1 -> Port0 无内拉 */
    P0IEN &= (uint8)~0x03u;
}

/* ---------------- 面包屑写入 ---------------- */
static void w45_br_set(uint8 step)
{
    w_brT1Prev = w_brT1Last;
    w_brT1Last = NFC_T1Count();
    w_brOvf    = NFC_WaitOvf();
    w_brStep   = step;
    w_brFlag   = W45_BR_MAGIC;
}

/* ---------------- 串口辅助 (英文输出, 避免终端乱码) ---------------- */
static char w_kch(uint8 k)
{
    if      (k == NFC_ACK_REAL)  { return 'R'; }
    else if (k == NFC_ACK_LATE)  { return 'L'; }
    else if (k == NFC_ACK_STUCK) { return 'S'; }
    return 'n';
}
static void w_uk(uint8 k) { uputc(w_kch(k)); }

static char w_rch(uint8 r)
{
    switch (r) {
    case W45_R_SKIP:  return '.';
    case W45_R_WNACK: return 'W';
    case W45_R_QTOUT: return 'Q';
    case W45_R_RNACK: return 'R';
    case W45_R_STBAD: return 'S';
    case W45_R_OK:    return 'D';
    case W45_R_FACB:  return 'B';
    default:          return 'F';
    }
}

/* NFC_ReadStd 返回码 -> 结果码 */
static uint8 w_map_std(uint8 r)
{
    if      (r == 0u) { return W45_R_OK;    }
    else if (r == 1u) { return W45_R_WNACK; }
    else if (r == 2u) { return W45_R_QTOUT; }
    else if (r == 3u) { return W45_R_RNACK; }
    return W45_R_STBAD;
}

static uint16 w45_pct(uint16 hit, uint16 tot)
{
    if (tot == 0u) { return 0u; }
    return (uint16)(((uint32)hit * 100UL) / (uint32)tot);
}

/* ---------------- 页面渲染 ---------------- */
static void w45_flush(void) { flush(); }

static void w45_page_main(void)
{
    uint8 anyStd = 0u, anyFac = 0u, anyPrb = 0u, c;

    for (c = 0u; c < 8u; c++) {
        if (w_res[c] != W45_R_SKIP) { anyPrb = 1u; }
        if ((c & 1u) == 0u && w_res[c] == W45_R_OK) { anyStd = 1u; }
        if ((c & 1u) != 0u && w_res[c] == W45_R_OK) { anyFac = 1u; }
    }

    title("V45 WAKE RESULT", "clean VDDEX wake + 8-way protocol test");

    b_reset();
    b_str("poll "); b_udec(w_polls, 0u);
    b_str(" rf ");  b_udec(w_hits, 0u); b_ch('/'); b_udec(w_slots, 0u);
    b_ch(' ');      b_udec(w45_pct(w_hits, w_slots), 0u); b_ch('%');
    b_str(" SQ ");  b_udec(w_seq, 0u);
    b_str(" rf");   b_udec(w_rfSeq, 0u);
    b_str(" pw");   b_dig(w_pwrmode);
    row8(0u, lb);

    b_reset();
    b_str("wake p23:");   b_dig(w_p23);
    b_str(" nirq:");      b_dig(w_nirq);
    b_str(" lin:");       b_dig(w_line);
    b_str(" (1=high, ext pullup)");
    row8(1u, lb);

    b_reset(); b_str("sw0 54 STD:"); b_ch(w_rch(w_res[0]));
    b_str(" FAC:"); b_ch(w_rch(w_res[1])); row8(2u, lb);
    b_reset(); b_str("sw0 55 STD:"); b_ch(w_rch(w_res[2]));
    b_str(" FAC:"); b_ch(w_rch(w_res[3])); row8(3u, lb);
    b_reset(); b_str("sw1 54 STD:"); b_ch(w_rch(w_res[4]));
    b_str(" FAC:"); b_ch(w_rch(w_res[5])); row8(4u, lb);
    b_reset(); b_str("sw1 55 STD:"); b_ch(w_rch(w_res[6]));
    b_str(" FAC:"); b_ch(w_rch(w_res[7])); row8(5u, lb);

    b_reset();
    b_str("bestSTD ");
    if (w_bestS == 0xFFu) { b_str("none"); }
    else {
        b_str("sw"); b_dig((uint8)(w_bestS >> 2));
        b_str(" 0x"); b_hex8((w_bestS & 2u) ? 0x55u : 0x54u);
    }
    b_str("  nirq="); b_udec(w_nirqS, 0u); b_str("ms");
    row8(6u, lb);

    b_reset();
    b_str("=> ");
    if      (anyStd != 0u) { b_str("STD PROTOCOL WORKS! see STD pg"); }
    else if (anyFac != 0u) { b_str("FAC PROTOCOL WORKS! see FAC pg"); }
    else if (anyPrb == 0u) { b_str("no slave ACK: not in serial mode"); }
    else {
        uint8 rn = 0u, bn = 0u;
        for (c = 0u; c < 8u; c++) {
            if (w_res[c] == W45_R_FACB) { bn++; }
            if (w_res[c] == W45_R_QTOUT) { rn++; }
        }
        if (bn != 0u) { b_str("addr ACK, 2nd NACK -> try STD pg"); }
        else if (rn != 0u) { b_str("NIRQ never low -> see STD pg"); }
        else { b_str("partial - see detail pages"); }
    }
    row8(7u, lb);

    b_reset();
    if (w45_pct(w_hits, w_slots) > 20u) { b_str("RF field ON - rerun each poll"); }
    else                               { b_str("no RF - put phone on the coil"); }
    row8(8u, lb);
}

static void w45_page_std(void)
{
    uint8 i;

    title("V45 STD DETAIL", "manual ch5: READ 08 | addrH addrL | len");

    b_reset();
    if (w_bestS == 0xFFu) {
        b_str("no combo passed probe - nothing to show");
        row8(0u, lb);
        b_reset(); b_str("probe = write-dir 5-point verdict (R=real)");
        row8(2u, lb);
        return;
    }
    b_str("combo sw"); b_dig((uint8)(w_bestS >> 2));
    b_str(" slv 0x");  b_hex8((w_bestS & 2u) ? 0x55u : 0x54u);
    b_str(" res ");     b_ch(w_rch(w_res[w_bestS]));
    row8(0u, lb);

    b_reset();
    b_str("W:");  b_k1(w_dbgS[0]);
    b_str(" 08:"); b_k1(w_dbgS[1]);
    b_str(" aH:"); b_k1(w_dbgS[2]);
    b_str(" aL:"); b_k1(w_dbgS[3]);
    b_str(" ln:"); b_k1(w_dbgS[4]);
    b_str(" R:");  b_k1(w_dbgS[5]);
    row8(1u, lb);

    b_reset();
    b_str("status=0x"); b_hex8(w_dbgS[6]);
    b_str(" (low4: 5=OK 7=busy A=prmb)");
    row8(2u, lb);

    b_reset();
    b_str("NIRQ wait "); b_udec(w_nirqS, 0u); b_str("ms");
    if (w_nirqS == 0xFFu) { b_str(" TIMEOUT"); }
    row8(3u, lb);

    b_reset(); b_str("d00-07:");
    for (i = 0u; i < 8u; i++) { b_ch(' '); b_hex8(w_blkS[i]); }
    row8(4u, lb);
    b_reset(); b_str("d08-0F:");
    for (i = 8u; i < 16u; i++) { b_ch(' '); b_hex8(w_blkS[i]); }
    row8(5u, lb);

    b_reset();
    b_str("D=Ver 10 means NDEF attr block - real chip");
    row8(7u, lb);
}

static void w45_page_fac(void)
{
    uint8 i;

    title("V45 FAC DETAIL", "factory style: direct block addressing");

    b_reset();
    if (w_bestF == 0xFFu) {
        b_str("no combo passed probe - nothing to show");
        row8(0u, lb);
        return;
    }
    b_str("combo sw"); b_dig((uint8)(w_bestF >> 2));
    b_str(" slv 0x");  b_hex8((w_bestF & 2u) ? 0x55u : 0x54u);
    b_str(" res ");     b_ch(w_rch(w_res[(uint8)(w_bestF + 1u)]));
    row8(0u, lb);

    b_reset();
    b_str("W:");  b_k1(w_dbgF[0]);
    b_str(" B:"); b_k1(w_dbgF[1]);
    b_str(" R:"); b_k1(w_dbgF[2]);
    b_str("  (addr/block/read)");
    row8(1u, lb);

    b_reset(); b_str("d00-07:");
    for (i = 0u; i < 8u; i++) { b_ch(' '); b_hex8(w_blkF[i]); }
    row8(2u, lb);
    b_reset(); b_str("d08-0F:");
    for (i = 8u; i < 16u; i++) { b_ch(' '); b_hex8(w_blkF[i]); }
    row8(3u, lb);

    b_reset();
    b_str("B = known issue: addr ACK, 2nd NACK");
    row8(5u, lb);
    b_reset();
    b_str("if STD shows D here -> chip speaks manual");
    row8(6u, lb);
}

static void w45_page_health(void)
{
    uint16 t1a, t1b;

    title("V45 SYS HEALTH", "timer + watchdog + breadcrumb");

    t1a = NFC_T1Count();
    NFC_DelayMs(2u);
    t1b = NFC_T1Count();

    b_reset();
    b_str("T1 d2ms=");  b_udec((uint16)(t1b - t1a), 0u);
    b_str(" tov=");     b_udec(NFC_WaitOvf(), 0u);
    b_str(" last=");    b_udec(NFC_WaitLast(), 0u);
    row8(0u, lb);

    b_reset();
    b_str("tickNs="); b_udec(NFC_GetTickNs(), 0u);
    b_str(" calOk=");  b_dig(NFC_GetCalOk());
    b_str(" SCLus=");  b_udec(NFC_GetBitPerUs(), 0u);
    row8(1u, lb);

    b_reset();
    b_str("br step="); b_udec(w_brStep, 0u);
    b_str(" fg=0x");   b_hex8(w_brFlag);
    b_str(" wdtN=");   b_udec(w_brN, 0u);
    row8(2u, lb);

    b_reset();
    b_str("T1 prev/last ");
    b_udec(w_brT1Prev, 0u); b_ch('/'); b_udec(w_brT1Last, 0u);
    row8(3u, lb);

    b_reset();
    if (NFC_WaitOvf() != 0u) { b_str("!! T1 waits TIMED OUT - report"); }
    else                     { b_str("all delays normal"); }
    row8(5u, lb);
}

static void w45_page_crash(void)
{
    title("!! WDT FIRED !!", "watchdog caught a freeze");

    b_reset();
    b_str("wdtN="); b_udec(w_brN, 0u);
    b_str(" brFlag=0x"); b_hex8(w_brFlag);
    row8(0u, lb);

    b_reset();
    b_str("frozen at step "); b_udec(w_brStep, 0u); b_str("/99");
    row8(2u, lb);

    b_reset();
    b_str("T1 prev/last ");
    b_udec(w_brT1Prev, 0u); b_ch('/'); b_udec(w_brT1Last, 0u);
    row8(3u, lb);

    b_reset();
    b_str("T1 wait timeouts="); b_udec(w_brOvf, 0u);
    row8(4u, lb);

    b_reset();
    if (w_brT1Prev == w_brT1Last) { b_str("=> T1 FROZEN at last breath"); }
    else                          { b_str("=> T1 was still running"); }
    row8(6u, lb);

    b_reset();
    b_str("re-power to restart");
    row8(8u, lb);
}

/* ===========================================================================
 *                        一轮完整唤醒 + 8 组合测试
 *  mode 0 = 单次拉高(手册最简唤醒)   mode 1 = 原厂复位脉冲(高5-低5-高)
 * =========================================================================== */
static void w45_seq_once(uint8 mode)
{
    uint8 sw, sv, c, k, r, i;
    uint8 s5[5];
    uint8 nirq;
    uint8 dbgT[8];
    uint8 blkT[16];
    uint8 blkAllFF, blkAll00;

    w_pwrmode = mode;
    w_seq++;

    uputs("\r\n[seq "); udec(w_seq);
    uputs(w_rfMark != 0u ? "] *RF* mode " : "] mode ");
    udig(mode);
    uputs(mode == 0u ? " single-rise" : " factory-pulse");
    uputs("\r\n");
    w_rfMark = 0u;

    /* --- 1. 放电: VDDEX 驱低 100ms, 总线三态(防倒灌, 干净 POR) --- */
    w45_br_set(1u);
    w45_p23_low();
    w45_bus_float();
    NFC_DelayMs(100u);
    uputs("[seq] step1 discharged 100ms\r\n");

    /* --- 1b. ★ 断电对照 probe —— 分辨"地址 ACK"真伪的终极实验 ★
     *   芯片此刻确定无电(VDDEX=0 且已放 100ms)。若断电时也报 R,
     *   说明 ACK 是主机电气假象(浮空/耦合/残留), 与芯片无关;
     *   若断电时全 n, 则上电后的 R 是芯片真实应答。
     *   0x6B = 必然无器件的对照地址。 */
    NFC_GpioPrepPub();
    NFC_BusRecover();
    {
        uint8 da;
        for (da = 0u; da < 3u; da++) {
            uint8 daddr = (da == 0u) ? 0x54u : ((da == 1u) ? 0x55u : 0x6Bu);
            k = NFC_Decide9(daddr, 0u, s5);
            uputs("[dead] 0x"); uh8(daddr);
            uputs(" k="); w_uk(k);
            uputs(" s5=");
            uputc((char)('0' + (uint8)(s5[0] & 1u)));
            uputc((char)('0' + (uint8)(s5[1] & 1u)));
            uputc((char)('0' + (uint8)(s5[2] & 1u)));
            uputc((char)('0' + (uint8)(s5[3] & 1u)));
            uputc((char)('0' + (uint8)(s5[4] & 1u)));
            uputs("\r\n");
        }
    }

    /* --- 2. 唤醒: VDDEX 上电 --- */
    w45_br_set(2u);
    if (mode == 0u) {
        w45_p23_hi();
    } else {
        w45_p23_hi();  NFC_DelayMs(5u);
        w45_p23_low(); NFC_DelayMs(5u);
        w45_p23_hi();
    }

    /* --- 3. tBoot 等待 + 引脚状态(此时总线仍三态, 读的是外部上拉真值) --- */
    w45_br_set(3u);
    NFC_DelayMs(50u);
    w_p23  = (uint8)((P2 & 0x08u) ? 1u : 0u);   /* 推挽下读回真实电平 */
    w_nirq = NFC_NirqLevel();
    w_line = (uint8)(P0 & 0x03u);
    uputs("[seq] step3 tBoot done: p23=");
    udig(w_p23); uputs(" nirq="); udig(w_nirq);
    uputs(" line=0x"); uh8(w_line); uputs("\r\n");

    /* --- 4. 接管总线 --- */
    w45_br_set(4u);
    NFC_GpioPrepPub();
    NFC_BusRecover();
    uputs("[seq] step4 bus takeover\r\n");

    /* --- 4b. ★ STATUS 直读 —— 数字核死活探针 (绕过命令处理) ★
     *   手册 5.1: 省略命令发送, 直接 START+slv+R 读状态字节。
     *   读回非 FF = 数字核活着; FF = 响应通道无数据。 */
    {
        uint8 stV = 0xFFu, stK = 9u;
        uint8 da2;
        for (da2 = 0u; da2 < 2u; da2++) {
            uint8 saddr = (da2 == 0u) ? 0x54u : 0x55u;
            NFC_WdtFeed();
            (void)NFC_StatusRead(saddr, &stV, &stK);
            uputs("[st] 0x"); uh8(saddr);
            uputs(" ack="); w_uk(stK);
            uputs(" st=0x"); uh8(stV);
            uputs(" CMD_RES="); udig((uint8)(stV & 0x0Fu));
            uputs("\r\n");
        }
        /* ★ v45b: 连续 10 次 STATUS —— 看 ACK 是否断续(数字核复位循环模型预测) */
        {
            uint8 q, ackN = 0u, dataN = 0u;
            for (q = 0u; q < 10u; q++) {
                uint8 sV = 0xFFu, sK = 9u;
                (void)NFC_StatusRead(0x55u, &sV, &sK);
                if (sK == NFC_ACK_REAL) { ackN++; }
                if (sV != 0xFFu)        { dataN++; }
            }
            uputs("[stx] 10x 0x55 ackN="); udec(ackN);
            uputs(" nonFF="); udec(dataN);
            uputs("\r\n");
        }
    }

    /* --- 5. 8 组合: swap x slv x proto --- */
    w45_br_set(5u);
    for (sw = 0u; sw < 2u; sw++) {
        NFC_SetSwap(sw);
        for (sv = 0u; sv < 2u; sv++) {
            uint8 slv = (uint8)((sv == 0u) ? 0x54u : 0x55u);
            NFC_WdtFeed();

            c = (uint8)(sw * 4u + sv * 2u);

            /* 5a. probe: 写方向 5 点判决 */
            k = NFC_Decide9(slv, 0u, s5);
            uputs("[prb] sw"); udig(sw);
            uputs(" 0x");      uh8(slv);
            uputs(" k=");      w_uk(k);
            uputs("\r\n");
            if (k != NFC_ACK_REAL) {
                w_res[c]         = W45_R_SKIP;
                w_res[(uint8)(c + 1u)] = W45_R_SKIP;
                continue;
            }

            /* 5b. STD: 手册标准 READ 0x08 */
            NFC_GpioPrepPub();
            NFC_BusRecover();
            r = NFC_ReadStd(slv, 0x0000u, 16u, blkT, dbgT, &nirq);
            w_res[c] = w_map_std(r);
            if (w_bestS == 0xFFu || r == 0u) {
                w_bestS = c;
                w_nirqS = nirq;
                for (i = 0u; i < 8u; i++)  { w_dbgS[i] = dbgT[i]; }
                for (i = 0u; i < 16u; i++) { w_blkS[i] = blkT[i]; }
            }
            uputs("[std] sw"); udig(sw);
            uputs(" 0x");      uh8(slv);
            uputs(" W:");      w_uk(dbgT[0]);
            uputs(" 08:");     w_uk(dbgT[1]);
            uputs(" aH:");     w_uk(dbgT[2]);
            uputs(" aL:");     w_uk(dbgT[3]);
            uputs(" ln:"); w_uk(dbgT[4]);
            uputs(" nirqUs="); udec(g_nirqUsLast);
            uputs(" R:"); w_uk(dbgT[5]);
            uputs(" st=0x");   uh8(dbgT[6]);
            uputs(" res=");    udig(r);
            uputs("\r\n");

            /* 5c. FAC: 原厂直接块寻址 */
            NFC_GpioPrepPub();
            NFC_BusRecover();
            (void)NFC_ReadMemDiag(slv, 0x00u, 16u, blkT, dbgT);
            blkAllFF = 1u; blkAll00 = 1u;
            for (i = 0u; i < 16u; i++) {
                if (blkT[i] != 0xFFu) { blkAllFF = 0u; }
                if (blkT[i] != 0x00u) { blkAll00 = 0u; }
            }
            if      (dbgT[0] != NFC_ACK_REAL) { r = W45_R_SKIP; }
            else if (dbgT[1] != NFC_ACK_REAL) { r = W45_R_FACB; }
            else if (dbgT[2] != NFC_ACK_REAL) { r = W45_R_FACB; }
            else if (blkAllFF != 0u || blkAll00 != 0u) { r = W45_R_FACF; }
            else                               { r = W45_R_OK; }
            w_res[(uint8)(c + 1u)] = r;
            if (w_bestF == 0xFFu || r == W45_R_OK) {
                w_bestF = c;
                for (i = 0u; i < 4u; i++)  { w_dbgF[i] = dbgT[i]; }
                for (i = 0u; i < 16u; i++) { w_blkF[i] = blkT[i]; }
            }
            uputs("[fac] sw"); udig(sw);
            uputs(" 0x");      uh8(slv);
            uputs(" W:");      w_uk(dbgT[0]);
            uputs(" B:");      w_uk(dbgT[1]);
            uputs(" R:");      w_uk(dbgT[2]);
            uputs(" res=");    udig(r);
            uputs("\r\n");
        }
    }

    /* --- 5c. ★★ 全地址扫描 0x00-0x7F ★★ —— 从地址由 FeRAM 系统区 HW1.I2C_SLV
     *        决定(产线写入, 可为任意值!)。0x54/0x55 只是手册默认/原厂值。
     *        连线已通(万用表验证), 若芯片串行口活着, 它应答的地址一定在这 128 个里。 */
    {
        static __xdata uint8 w_amap[32];
        uint8 swS, a, cnt;
        for (swS = 0u; swS < 2u; swS++) {
            NFC_SetSwap(swS);
            NFC_WdtFeed();
            NFC_GpioPrepPub();
            NFC_BusRecover();
            cnt = NFC_AddrScanAck(w_amap);
            uputs("[scan] sw"); udig(swS);
            uputs(" hits="); udec(cnt);
            uputs(":");
            {
                uint8 g2, i2;
                for (g2 = 0u; g2 < 8u; g2++) {
                    for (i2 = 0u; i2 < 16u; i2++) {
                        uint16 ai = (uint16)(g2 * 16u + i2);
                        uint8 v = (uint8)((w_amap[(uint8)(ai >> 2)] >>
                                           (uint8)(2u * (ai & 3u))) & 0x03u);
                        if (v != 0u) {
                            uputc(' '); uh8((uint8)ai);
                            uputc((v == 1u) ? 'R' : 'S');
                        }
                    }
                }
            }
            uputs("\r\n");
        }
        NFC_SetSwap(0u);
    }

    /* --- 5b. ★ 速度扫描 —— 手册要求 SCL 在 20kHz~100kHz ★
     *   默认 67us 位周期 ≈ 15kHz, 低于手册下限! 行为未定义区。
     *   对 swap0 的两个从地址, 用 50/33/25/20us(20k/30k/40k/50kHz)
     *   各跑一次手册标准 READ, 看命令字节是否开始被接受。 */
    {
        static const uint16 SPD[4] = { 50u, 33u, 25u, 20u };
        uint8 si, sa;
        for (sa = 0u; sa < 2u; sa++) {
            uint8 saddr = (sa == 0u) ? 0x54u : 0x55u;
            for (si = 0u; si < 4u; si++) {
                uint8 sr, dbgS[8];
                uint8 blkS[16];
                uint8 nirqS = 0xFFu;
                NFC_WdtFeed();
                NFC_SetBitUs(SPD[si]);
                sr = NFC_ReadStd(saddr, 0x0000u, 16u, blkS, dbgS, &nirqS);
                uputs("[spd] 0x"); uh8(saddr);
                uputs(" bit="); udec(SPD[si]);
                uputs("us W:");  w_uk(dbgS[0]);
                uputs(" 08:"); w_uk(dbgS[1]);
                uputs(" aH:"); w_uk(dbgS[2]);
                uputs(" aL:"); w_uk(dbgS[3]);
                uputs(" ln:"); w_uk(dbgS[4]);
                uputs(" nirqUs="); udec(g_nirqUsLast);
                uputs(" R:"); w_uk(dbgS[5]);
                uputs(" st=0x"); uh8(dbgS[6]);
                uputs(" res="); udig(sr);
                if (sr == 0u) {
                    uint8 q2;
                    uputs(" DATA:");
                    for (q2 = 0u; q2 < 8u; q2++) { uputc(' '); uh8(blkS[q2]); }
                }
                uputs("\r\n");
            }
        }
        NFC_SetBitUs(67u);           /* 恢复原厂节奏 */
    }

    /* --- 6. 断电收尾 --- */
    w45_br_set(6u);
    w45_p23_low();
    NFC_DelayMs(2u);
    w45_bus_float();
    w45_p23_float();

    /* --- 串口 verdict --- */
    uputs("[verdict] ");
    {
        uint8 anyStd = 0u, anyFac = 0u, anyPrb = 0u, cc;
        for (cc = 0u; cc < 8u; cc++) {
            if (w_res[cc] != W45_R_SKIP) { anyPrb = 1u; }
            if ((cc & 1u) == 0u && w_res[cc] == W45_R_OK) { anyStd = 1u; }
            if ((cc & 1u) != 0u && w_res[cc] == W45_R_OK) { anyFac = 1u; }
        }
        if (anyStd != 0u) {
            uputs("STD PROTOCOL LINK UP (manual 0x08 works)");
        } else if (anyFac != 0u) {
            uputs("FAC PROTOCOL LINK UP (block addressing works)");
        } else if (anyPrb == 0u) {
            uputs("no slave ACK at all - chip not in serial mode");
        } else {
            uputs("addr ACK but no data path - see [std]/[fac] lines");
        }
    }
    uputs("\r\n");
}

/* ===========================================================================
 *                                main
 * =========================================================================== */
int main(void)
{
    uint8  pidx = 0u;
    uint8  mode = 0u;
    uint16 t;
    uint8  rst;

    clock_init();
    uart_init();
    uputs("\r\n===== v45 wake test: clean VDDEX + 8-way proto =====\r\n");

    /* v44 机制: 尽早读 SLEEPSTA, 判断是否被看门狗复位 */
    rst = SLEEPSTA;
    NFC_WdtFeed();
    if ((rst & 0x06u) == 0x04u) {
        if (w_brN < 0xFFFFu) { w_brN++; }
    }

    /* NIRQ (P1.0) 输入 + 上拉 */
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;

    NFC_T1Init();
    NFC_TimebaseCal();

    uputs("[tb] tickNs="); udec(NFC_GetTickNs());
    uputs(" calOk=");      udig(NFC_GetCalOk());
    uputs(" bitUs=");      udec(NFC_GetBitPerUs());
    uputs("\r\n");

    /* 屏自证: 先证明屏通了 */
    EPD_Init();
    EPD_Fill(0xFF);

    /* 看门狗复位过: 面包屑甩到串口 + 屏, 然后**继续跑**(不锁死)。
     * ★ 教训(实测): CSpyBat 的调试复位不清 SLEEPSTA.RST —— 上次若死于 WDT,
     *   本次烧录复位后 rst 仍读出 WDT。所以这里绝不能死循环, 否则每次烧录
     *   都会被残留标记锁进 CRASH 页, 固件永远跑不起来。
     *   显示事故 -> 清面包屑 -> 继续主流程, 两种场景(真WDT/调试复位)都畅通。 */
    if ((rst & 0x06u) == 0x04u) {
        uputs("[crash] WDT fired! frozen at step ");
        udec(w_brStep);
        uputs(" wdtN=");    udec(w_brN);
        uputs(" T1prev=");  udec(w_brT1Prev);
        uputs(" last=");    udec(w_brT1Last);
        uputs(" tov=");     udec(w_brOvf);
        uputs("\r\n");
        w45_page_crash();
        w45_flush();
        w_brFlag = 0u;                /* 消费事故, 继续跑 */
    }
    w_brN = 0u;                      /* 正常启动: 计数归零 */

    /* 初始化快照为"未测" */
    for (t = 0u; t < 8u; t++) { w_res[t] = W45_R_SKIP; }
    w_bestS = 0xFFu; w_bestF = 0xFFu; w_nirqS = 0xFFu;
    w_p23 = 0u; w_nirq = 0u; w_line = 0u; w_pwrmode = 0u;
    w_seq = 0u; w_rfSeq = 0u; w_polls = 0u; w_hits = 0u; w_slots = 0u;

    title("V45 WAKE TEST", "clean VDDEX wake - first round starts now");
    row8(0u, "wake -> probe -> read, 8 combos");
    row8(1u, "results on serial + this screen");
    w45_flush();

    w45_bus_float();
    w45_p23_float();

    NFC_WaitClr();
    NFC_WdtArm();                   /* 从此刻起 >1s 卡死即被抬走 */

    /* 首轮基线: 不等 RF, 直接唤醒跑一轮, 保证第一屏有数据 */
    w45_seq_once(0u);

    for (;;) {
        uint16 hit  = 0u;
        uint16 slot = 0u;
        uint16 nLo  = 0u;               /* ★ RF 场下 NIRQ 被芯片拉低的采样数 */

        /* ---- RF 观测窗口: P2.3 浮空, 10ms 轮询 ---- */
        w45_br_set(99u);
        w45_bus_float();
        w45_p23_float();
        for (t = 0u; t < 150u; t++) {
            if ((P2 & 0x08u) != 0u) { hit++; }
            if (NFC_NirqLevel() == 0u) { nLo++; }
            slot++;
            w_polls++;
            NFC_DelayMs(10u);
            if ((t & 0x1Fu) == 0u) { NFC_WdtFeed(); }
        }
        w_hits  = hit;
        w_slots = slot;

        /* ★ 手册 90 页: RF 场给芯片供电时芯片会主动拉低 NIRQ 通知主机。
         *   RF 在场而 NIRQ 始终高 = 芯片数字核没在跑(复位循环/异常)。 */
        if (hit != 0u) {
            uputs("[rfirq] rf "); udec(hit);
            uputs("/"); udec(slot);
            uputs(" nirqLo "); udec(nLo);
            uputs("\r\n");
        }

        /* ---- RF 场在场才再唤醒(RF 先到 -> 主机后供电) ---- */
        if (w45_pct(hit, slot) > 20u) {
            w_rfSeq++;
            w_rfMark = 1u;                       /* ★ 标记: 本轮由 RF 触发 */
            mode = (uint8)((w_rfSeq & 1u) != 0u ? 1u : 0u);
            w45_seq_once(mode);
            pidx = 0u;
        } else {
            pidx++;
            if (pidx > 3u) { pidx = 0u; }
        }

        if      (pidx == 0u) { w45_page_main();   }
        else if (pidx == 1u) { w45_page_std();   }
        else if (pidx == 2u) { w45_page_fac();   }
        else                 { w45_page_health(); }

        w45_flush();
        NFC_WdtFeed();
    }
}

#endif /* V45_WAKE */
#endif /* __V45_WAKE_H__ */
