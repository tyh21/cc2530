#ifndef __V40_EPD_H__
#define __V40_EPD_H__

/* ===========================================================================
 * ★★★★★ v40 EPD 版 —— 串口拔掉后, 所有诊断结果画到板载 2.9" 墨水屏 ★★★★★
 *
 * 背景:
 *   v30/v31..v36 的结论全靠串口抓包。现在串口 USB-TTL 已拔掉, 抓数链路失效。
 *   本块把同样的诊断搬到 EPD, 顺便验证一个**从未排除过的变量**:
 *     UART TX(P1.6/P1.7) 与 USB-TTL 的共地/线缆是否一直在干扰总线。
 *   判据: 拔串口后若 128/128 "全 ACK" 的现象消失 -> 之前是串口侧干扰。
 *
 * 屏幕: SSD1680 296x128 横屏, Font16 标题 + Font8 正文(5x8, 每行 10px)
 *   可用正文行 = row0..row8  (y = 32,42,...,112)
 *
 * 页面结构 (主循环每轮渲染一页):
 *   pid 0  主结果页      —— 有无链路的一眼看清单 (检测到 RF 时优先显示此页)
 *   pid 1  ACK 矩阵       —— A~F 六种情形的 (pre,hi,post)
 *   pid 2  时序 + ADC     —— 空时钟实验 + SDA 真实电压(mV)
 *   pid 3  第二字节扫描   —— 12 个候选值各自的 ACK 类型
 *   pid 4  地址全扫描     —— 0x00..0x7F 的 ACK 分布图
 *
 * RF 观测:
 *   每轮开头把 P2.3 放开成浮空输入, 以 10ms 间隔轮询它的读值。
 *   命中率 > 20% 判定为"手机 RF 场在场", 于是才**拉起 P2.3 供电**并重跑一轮诊断
 *   —— 这一步严格遵循用户坚持的顺序: RF 场先到 -> 主机再给 VDDEX 供电。
 * =========================================================================== */
#if defined(V40_EPD) && (V40_EPD != 0)

/* ---------------- 布局常量 ---------------- */
#define E4_X        2u
#define E4_TY       1u     /* Font16 标题 y */
#define E4_SY       20u    /* Font8 副标题 y */
#define E4_RY       32u    /* 正文首行 y */
#define E4_DY       10u    /* 行距 */
#define E4_ROWS     9u     /* row0..row8 */
#define E4_NCAND    12u    /* 第二字节候选个数 */

/* ---------------- 结果快照 (每轮 SEQ 重算, 各页共用) ---------------- */
static uint8  g_p23;
static uint8  g_nirq;
static uint8  g_line;
static uint8  g_d9k;
static uint8  g_s5[5];
static uint8  g_dbgA[4];
static uint8  g_dbgB[4];
static uint8  g_blkA[16];
static uint8  g_blkB[16];
static uint8  g_matrix[6];
static uint8  g_timing[6];
static uint16 g_mv[5];
static uint8  g_secOut[E4_NCAND];
static uint8  g_secCnt;
static uint8  g_scanReal;
static uint8  g_pwrmode;
static uint8  g_amap[32];      /* ★ v43: 2bit/地址 位图, 原为 [128] */

/* ★ v41 慢充电实验 */
static uint16 g_slowMv[6];
static uint8  g_slowDig[6];
static uint16 g_strMv[6];
static uint8  g_strDig[6];
static uint8  g_slotFast;      /* 原节奏(22us)下的 ACK 槽 */
static uint8  g_slotSlow;      /* 每点 2ms 下的 ACK 槽 */
static uint16 g_slotMv[4];     /* v42: SDA@pre, SCL@pre, SDA@hi, SCL@hi (mV) */
static uint8  g_slotDig[4];    /* v42: 对应的数字读值 */

/* ★ v42 寄存器/引脚真值快照 —— 先把"我们量的到底是哪根线"钉死 */
static uint8  g_rswap;         /* swap01 当前值 */
static uint8  g_rP0;           /* 原始 P0 字节 */
static uint8  g_rP0DIR;
static uint8  g_rP0SEL;
static uint8  g_rP0INP;
static uint8  g_rP2INP;
static uint8  g_rAPCFG;
static uint16 g_mvP00Hi;       /* P0.0 推挽驱高电压 */
static uint16 g_mvP00Lo;       /* P0.0 推挽驱低电压 */
static uint16 g_mvP01Hi;       /* P0.1 推挽驱高电压 */
static uint16 g_mvP01Lo;       /* P0.1 推挽驱低电压 */
static uint8  g_adcVer;        /* ADC 版本/稳定性自检 */

/* ---------------- 计数器 ---------------- */
static uint16 g_polls;      /* 累计轮询次数 */
static uint16 g_seq;        /* 已跑 SEQ 轮数 */
static uint16 g_rfSeq;      /* 由 RF 场触发的 SEQ 轮数 */
static uint16 g_nirqLo;     /* NIRQ 被拉低的采样数 */
static uint16 g_hits;       /* 本轮观测窗口里 P2.3 读出 1 的次数 */
static uint16 g_slots;      /* 本轮观测窗口总采样数 */

/* ---------------- ★ v44 面包屑: 卡死位置记录 ----------------
 * 背景(Round 45): v42 固件跑到 step 9 就再也不动, 屏上停在 "9/15"。
 *  既不报错也没地方看 —— 于是让**硬件自己**把最后一刻的位置记下来:
 *   1) 硬件看门狗 ~1s; 喂狗点铺满所有长操作(含墨水屏全刷的 BUSY 等待);
 *   2) 每进一个步骤就把 (步骤号 / 子阶段 / 扫描进度 / T1 快照) 写进
 *      __no_init RAM —— 看门狗复位**不清零**这块, 与普通全局变量不同;
 *   3) 真卡住 -> 狗叫复位 -> 开机第一件事读 SLEEPSTA, 若判定是看门狗复位
 *      就立刻画 CRASH 页, 把面包屑甩到屏上。
 * CC2530 (SWRU191F p.154): MODE[3:2]=10 看门狗; INT[1:0]=00 -> ~1s;
 *   喂狗 = 对 CLR[7:4] 先写 0xA 再写 0x5。看门狗模式使能后**无法关闭**。
 * -------------------------------------------------------------------- */
__no_init uint8  g_brFlag;      /* 0xA5 = 面包屑有效 */
__no_init uint8  g_brStep;      /* 卡在第几步 */
__no_init uint8  g_brSub;       /* 第几子阶段 */
__no_init uint8  g_brAddr;      /* NFC_AddrScanAck 的实时进度 */
__no_init uint8  g_brRst;       /* 复位瞬间读到的 SLEEPSTA 原值 */
__no_init uint16 g_brN;         /* 累计看门狗复位次数 */
__no_init uint16 g_brOvf;       /* 当时的 T1 等待超时次数 */
__no_init uint16 g_brT1Prev;    /* 上一次的 T1 计数值 */
__no_init uint16 g_brT1Last;    /* 最新的 T1 计数值(两者相等 = T1 停了) */

static const uint8 g_cand[E4_NCAND] = {
    0x00u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u,
    0x20u, 0x40u, 0x55u, 0x80u, 0xA0u, 0xFFu
};

/* ============================ 引脚控制 ==================================== */
/* ★ P2.3 的 GPIO/外设选择位是 P2SEL.1 (不是 bit3) —— 早期 bug 的根因 */
static void e4_p23_float(void)
{
    P2SEL &= (uint8)~0x02u;   /* P2SEL.1 = P2.3 功能位, 0 = GPIO */
    P2INP |= 0x08u;           /* 三态: 不内拉, 只看外部 */
    P2DIR &= (uint8)~0x08u;   /* 输入 -> 浮空观测 RF 场耦合 */
}
static void e4_p23_hi(void)
{
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2    |= 0x08u;
}
static void e4_p23_low(void)
{
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2    &= (uint8)~0x08u;
}
static void e4_bus_float(void)
{
    P0SEL &= (uint8)~0x03u;
    P0INP |= 0x03u;
    P0DIR &= (uint8)~0x03u;
    P2INP |= 0x20u;           /* PDUP0=1 -> Port0 三态 */
    P0IEN &= (uint8)~0x03u;
}

/* ============================ v44 面包屑写入 ============================== */
#define E4_BR_MAGIC   0xA5u

static void e4_feed(void)
{
    NFC_WdtFeed();
}

/* step/sub 只是便于日后扩展; 真正的"最后一刻"信息是 g_scanCur 与 T1 快照 */
static void e4_br_set(uint8 step, uint8 sub)
{
    g_brT1Prev = g_brT1Last;
    g_brT1Last = NFC_T1Count();
    g_brOvf    = NFC_WaitOvf();
    g_brAddr   = g_scanCur;
    g_brSub    = sub;
    g_brStep   = step;
    g_brFlag   = E4_BR_MAGIC;
}

/* ============================ 画屏小工具 ================================== */
static void e4_flush(void)
{
    EPD_DisplayImage((const uint8 *)EPD_Frame);
}
static void e4_row(uint8 r, const char *s)
{
    if (r >= E4_ROWS) { return; }
    EPD_DrawString(E4_X, (uint16)(E4_RY + (uint16)r * E4_DY), s, &Font8, 1);
}
static void e4_page(const char *big, const char *sub)
{
    EPD_FrameClear(0xFF);
    EPD_DrawString(E4_X, E4_TY, big, &Font16, 1);
    if (sub != 0) { EPD_DrawString(E4_X, E4_SY, sub, &Font8, 1); }
}
/* 命中率百分比 */
static uint16 e4_pct(uint16 hit, uint16 tot)
{
    if (tot == 0u) { return 0u; }
    return (uint16)(((uint32)hit * 100UL) / (uint32)tot);
}
/* 三点采样 [pre,hi,post] 打成 "[x,y,z]" */
static void b_slot(uint8 v)
{
    b_ch('[');
    b_ch((v & 0x01u) ? '1' : '0'); b_ch(',');
    b_ch((v & 0x02u) ? '1' : '0'); b_ch(',');
    b_ch((v & 0x04u) ? '1' : '0'); b_ch(']');
}
/* 十六进制数组 -> " XX XX XX" */
static void b_hexarr(const uint8 *p, uint8 st, uint8 n)
{
    uint8 i;
    for (i = 0u; i < n; i++) {
        b_ch(' '); b_hex8(p[(uint8)(st + i)]);
    }
}

/* ============================ 进度指示 ====================================
 * ★ v41a: 在长流程的每个步骤前把步骤名刷到屏上。
 *   这样一旦某个步骤卡死, 屏幕上留下的就是**卡住的那一步**, 不用再猜。
 *   代价: 每步一次全刷(约 2s)。一轮 SEQ 有 12 步 ≈ 25s 额外开销,
 *   但换来的是"卡死可定位", 非常值。
 */
static void e4_step(const char *name, uint8 step, uint8 total)
{
    e4_br_set(step, 0u);        /* ★ v44: 先留痕, 再干活 */
    e4_feed();                  /* ★ 这一次全刷可能耗 >1s, 前后都喂一次 */

    EPD_FrameClear(0xFF);
    EPD_DrawString(E4_X, E4_TY, "RUNNING STEP", &Font16, 1);
    EPD_DrawString(E4_X, E4_SY, "if it freezes, this line is where", &Font8, 1);
    EPD_DrawString(E4_X, (uint16)(E4_RY + 20u), name, &Font16, 1);
    b_reset();
    b_str("step "); b_udec(step, 0u); b_ch('/'); b_udec(total, 0u);
    e4_row(4u, lb);
    e4_flush();

    e4_feed();
}

/* ============================ 一轮完整 SEQ ================================
 * mode 0 = 单次拉高 (v30e 做法)
 * mode 1 = 原厂复位脉冲 (高5ms -> 低5ms -> 高, 复刻 bank1 0xF57C)
 * 结束把供电与总线全部放开, 交还给 RF 观测窗口。
 */
#define E4_STEPS 15u
static void e4_seq_once(uint8 mode)
{
    uint8 i;

    e4_step("power P2.3", 1u, E4_STEPS);

    if (mode == 0u) {
        e4_p23_hi();
    } else {
        e4_p23_hi();  NFC_DelayMs(5u);
        e4_p23_low(); NFC_DelayMs(5u);
        e4_p23_hi();
    }
    NFC_DelayMs(12u);                   /* 让 VDDEX 电容充起来 */

    /* ★ 推挽输出状态下读 P2 得到的是**引脚真实电平**: 1 = >=VIH(~2.3V) */
    g_p23     = (uint8)((P2 & 0x08u) ? 1u : 0u);
    g_nirq    = NFC_NirqLevel();
    g_line    = (uint8)(P0 & 0x03u);
    g_pwrmode = mode;

    e4_step("gpio prep + bus recover", 2u, E4_STEPS);
    NFC_GpioPrepPub();
    NFC_BusRecover();

    e4_step("Decide9 (5pt probe)", 3u, E4_STEPS);
    g_d9k = NFC_Decide9(0x55u, 0u, g_s5);
    NFC_DelayMs(10u);

    e4_step("ReadMemDiag (STOP sep)", 4u, E4_STEPS);
    (void)NFC_ReadMemDiag(0x55u, 0x00u, 16u, g_blkA, g_dbgA);
    NFC_DelayMs(20u);

    e4_step("ReadMemDiag (repeat START)", 5u, E4_STEPS);
    (void)NFC_ReadMemDiagRs(0x55u, 0x00u, 16u, g_blkB, g_dbgB);
    NFC_DelayMs(20u);

    e4_step("ACK matrix A..F", 6u, E4_STEPS);
    (void)NFC_AckMatrix(g_matrix);
    NFC_DelayMs(10u);

    e4_step("ACK slot fast(22us)", 7u, E4_STEPS);
    {
        uint8 tmp[1];
        (void)NFC_AckTimingSweep(tmp);      /* tmp[0] = 空时钟 ACK 槽 */
        g_timing[0] = tmp[0];
        for (i = 1u; i < 6u; i++) { g_timing[i] = 0u; }
        g_slotFast = tmp[0];
    }
    NFC_DelayMs(10u);

    e4_step("2nd byte scan (12)", 8u, E4_STEPS);
    (void)NFC_ProbeSecondByte(0x55u, g_cand, E4_NCAND, g_secOut);
    g_secCnt = 0u;
    for (i = 0u; i < E4_NCAND; i++) {
        if (g_secOut[i] == NFC_ACK_REAL) { g_secCnt++; }
    }
    NFC_DelayMs(10u);

    e4_step("addr scan 0x00-0x7F", 9u, E4_STEPS);
    g_scanReal = NFC_AddrScanAck(g_amap);
    NFC_DelayMs(10u);

    e4_step("SLOW release (weak pull)", 10u, E4_STEPS);
    (void)NFC_SlowReleaseProbe(g_slowMv, g_slowDig);
    NFC_DelayMs(10u);

    e4_step("STRONG pull then release", 11u, E4_STEPS);
    (void)NFC_StrongPullProbe(g_strMv, g_strDig);
    NFC_DelayMs(10u);

    e4_step("ACK slot SLOW (500us)", 12u, E4_STEPS);
    {
        uint8 tmp[1];
        (void)NFC_AckSlotSlow(tmp);
        g_slotSlow = tmp[0];
    }
    NFC_DelayMs(10u);

    e4_step("ADC 5-point voltage", 13u, E4_STEPS);
    (void)NFC_AdcProbe(g_mv);
    NFC_DelayMs(10u);

    e4_step("pin truth + regs", 14u, E4_STEPS);
    {
        uint8 rg[8];
        uint16 pv[4];
        (void)NFC_PinTruthProbe(rg, pv);
        g_rswap  = rg[0];  g_rP0    = rg[1];  g_rP0DIR = rg[2];
        g_rP0SEL = rg[3];  g_rP0INP = rg[4];  g_rP2INP = rg[5];
        g_rAPCFG = rg[6];
        g_mvP00Hi = pv[0]; g_mvP00Lo = pv[1];
        g_mvP01Hi = pv[2]; g_mvP01Lo = pv[3];
    }
    NFC_DelayMs(10u);

    e4_step("ACK slot dual-line map", 15u, E4_STEPS);
    (void)NFC_AckSlotMap(g_slotMv, g_slotDig);
    NFC_DelayMs(10u);

    /* 收尾: 松电 + 放开总线, 回到可观测状态 */
    e4_p23_low();
    NFC_DelayMs(2u);
    e4_bus_float();
    e4_p23_float();

    g_seq++;
}

/* ============================ 页 0: 主结果 ================================ */
static void e4_page_main(void)
{
    uint8 i;
    uint8 allFF = 1u;
    uint8 all00 = 1u;

    for (i = 0u; i < 16u; i++) {
        if (g_blkA[i] != 0xFFu) { allFF = 0u; }
        if (g_blkA[i] != 0x00u) { all00 = 0u; }
    }

    e4_page("LG290 NFC   MAIN", "out on epd / uart removed");

    b_reset();
    b_str("poll "); b_udec(g_polls, 0u);
    b_str(" rf ");  b_udec(g_hits, 0u); b_ch('/'); b_udec(g_slots, 0u);
    b_ch(' ');      b_udec(e4_pct(g_hits, g_slots), 0u); b_ch('%');
    b_str(" SEQ "); b_udec(g_seq, 0u);
    b_str(" rfSEQ "); b_udec(g_rfSeq, 0u);
    e4_row(0u, lb);

    b_reset();
    b_str("P23 "); b_dig(g_p23);
    b_str(" NIRQ "); b_dig(g_nirq);
    b_str(" LIN 0x"); b_dig(g_line);
    b_str(" D9 k"); b_dig(g_d9k);
    b_str(" s5 ");
    for (i = 0u; i < 5u; i++) { b_dig((uint8)(g_s5[i] & 0x01u)); }
    e4_row(1u, lb);

    b_reset();
    b_str("RMA W"); b_k1(g_dbgA[0]); b_ch('B'); b_k1(g_dbgA[1]); b_ch('R'); b_k1(g_dbgA[2]);
    b_str("  RMB W"); b_k1(g_dbgB[0]); b_ch('B'); b_k1(g_dbgB[1]); b_ch('R'); b_k1(g_dbgB[2]);
    b_str(" pwr"); b_dig(g_pwrmode);
    e4_row(2u, lb);

    b_reset(); b_str("blkA 00:"); b_hexarr(g_blkA, 0u, 8u);  e4_row(3u, lb);
    b_reset(); b_str("blkA 08:"); b_hexarr(g_blkA, 8u, 8u);  e4_row(4u, lb);

    b_reset();
    b_str("scan real ");  b_udec(g_scanReal, 0u); b_str("/128");
    b_str("   2nd ");     b_udec(g_secCnt, 0u);   b_str("/12");
    e4_row(5u, lb);

    b_reset();
    b_str("G empty clk "); b_slot(g_timing[0]);
    b_str(" SDAmv ");      b_udec(g_mv[3], 0u);
    e4_row(6u, lb);

    b_reset();
    b_str("=> ");
    if (g_dbgA[0] == NFC_ACK_REAL && g_dbgA[1] == NFC_ACK_REAL &&
        g_dbgA[2] == NFC_ACK_REAL && allFF == 0u && all00 == 0u) {
        b_str("*** LINK UP - chip talks ***");
    } else if (allFF != 0u) {
        b_str("no data: SDA idle HIGH all FF");
    } else if (all00 != 0u) {
        b_str("SDA stuck LOW all 00");
    } else if (g_dbgA[1] == NFC_ACK_NONE) {
        b_str("NOACK at 2nd byte (blk addr)");
    } else if (g_dbgA[0] == NFC_ACK_NONE) {
        b_str("NOACK at slave addr 0x55");
    } else {
        b_str("real addr, empty read - see DET");
    }
    e4_row(7u, lb);

    b_reset();
    if (e4_pct(g_hits, g_slots) > 20u) { b_str("RF field SEEN - rerun each poll"); }
    else                               { b_str("no RF - put phone on the coil"); }
    e4_row(8u, lb);
}

/* ============================ 页 1: ACK 矩阵 ============================== */
static const char * e4_mname(uint8 q)
{
    if (q == 0u) { return "A START+send 00  "; }
    if (q == 1u) { return "B START+send FF  "; }
    if (q == 2u) { return "C START+no byte  "; }
    if (q == 3u) { return "D no START/no b  "; }
    if (q == 4u) { return "E START+send AA  "; }
    return "F 00 after AA    ";
}

static void e4_page_matrix(void)
{
    uint8 q;

    e4_page("DET 1/4  ACK MATRIX", "[pre,hi,post] 1=high; hi=0 means ACK");

    for (q = 0u; q < 6u; q++) {
        b_reset();
        b_str(e4_mname(q));
        b_slot(g_matrix[q]);
        b_ch(' ');
        if ((g_matrix[q] & 0x02u) == 0u) {
            b_str((g_matrix[q] & 0x04u) != 0u ? "ACK then released" : "HELD LOW (fake ACK)");
        } else {
            b_str("no ACK");
        }
        e4_row(q, lb);
    }
    b_reset();
    b_str("C/D: no byte sent at all - any ACK there is electrical noise");
    e4_row(6u, lb);
    b_reset();
    b_str("if every row says ACK -> the ACK verdict itself is not trustworthy");
    e4_row(7u, lb);
}

/* ============================ 页 2: 时序 + ADC ============================ */
static void e4_page_timing(void)
{
    uint8 q;

    e4_page("DET 2/4  TIMING+ADC", "G = START + 8 empty clocks, SDA undriven");

    b_reset();
    b_str("G empty clocks "); b_slot(g_timing[0]);
    b_ch(' ');
    if ((g_timing[0] & 0x02u) == 0u) { b_str("-> ACK with NO byte (artifact)"); }
    else                             { b_str("-> no ACK (needs a real byte)"); }
    e4_row(0u, lb);

    b_reset();
    b_str("release SDA then read: ");
    e4_row(1u, lb);
    b_reset();
    for (q = 0u; q < 5u; q++) {
        b_ch(' '); b_dig((uint8)(g_timing[1u + q] & 0x01u));
    }
    b_str("   @22/66/176/616/1716us");
    e4_row(2u, lb);

    b_reset();
    b_str("SDA real voltage (mV, AVDD ref)");
    e4_row(3u, lb);
    b_reset(); b_str(" push-pull HI base "); b_udec(g_mv[0], 0u); e4_row(4u, lb);
    b_reset(); b_str(" released idle     "); b_udec(g_mv[1], 0u); e4_row(5u, lb);
    b_reset(); b_str(" after 8 empty clk "); b_udec(g_mv[2], 0u); e4_row(6u, lb);
    b_reset(); b_str(" 9th clk HIGH (ACK "); b_udec(g_mv[3], 0u); e4_row(7u, lb);
    b_reset(); b_str(" 9th clk LOW  pt)  "); b_udec(g_mv[4], 0u); e4_row(8u, lb);
}

/* ============================ 页 3: 第二字节 ============================== */
static void e4_page_second(void)
{
    uint8 r, k, i;

    e4_page("DET 3/4  2ND BYTE", "START+0xAA+X+STOP -> ACK type of X");

    for (r = 0u; r < 3u; r++) {
        b_reset();
        for (k = 0u; k < 4u; k++) {
            i = (uint8)(r * 4u + k);
            b_hex8(g_cand[i]); b_ch('-'); b_k1(g_secOut[i]); b_str("  ");
        }
        e4_row(r, lb);
    }

    b_reset();
    b_str("real ACK count = "); b_udec(g_secCnt, 0u); b_str("/12");
    e4_row(3u, lb);

    b_reset();
    b_str("R/L/n/S = real / late / none / stuck-low");
    e4_row(4u, lb);

    b_reset();
    if (g_secCnt == 0u) {
        b_str("chip answers addr but refuses ANY 2nd byte");
    } else {
        b_str("2nd byte matters - it is a real protocol field");
    }
    e4_row(5u, lb);

    b_reset();
    b_str("RMA W"); b_k1(g_dbgA[0]); b_ch('B'); b_k1(g_dbgA[1]); b_ch('R'); b_k1(g_dbgA[2]);
    b_str("  RMB W"); b_k1(g_dbgB[0]); b_ch('B'); b_k1(g_dbgB[1]); b_ch('R'); b_k1(g_dbgB[2]);
    e4_row(6u, lb);

    b_reset(); b_str("blkB 00:"); b_hexarr(g_blkB, 0u, 8u);  e4_row(7u, lb);
    b_reset(); b_str("blkB 08:"); b_hexarr(g_blkB, 8u, 8u);  e4_row(8u, lb);
}

/* ============================ 页 4: 地址全扫描 ============================ */
static void e4_page_scan(void)
{
    uint8 g, i;

    e4_page("DET 4/4  ADDR SCAN", "0x00-0x7F write-dir; 0=none R=real S=stuck");

    b_reset();
    b_str("addresses answering = "); b_udec(g_scanReal, 0u); b_str(" / 128");
    e4_row(0u, lb);

    for (g = 0u; g < 8u; g++) {
        b_reset();
        b_hex8((uint8)(g * 16u)); b_str(": ");
        for (i = 0u; i < 16u; i++) {
            uint16 ai = (uint16)(g * 16u + i);
            /* ★ v43: 2bit 位图解包 —— 地址 a 占 g_amap[a>>2] 的 bit(2*(a&3)) */
            uint8 v = (uint8)((g_amap[(uint8)(ai >> 2)] >> (uint8)(2u * (ai & 3u))) & 0x03u);
            b_ch(v == 0u ? '.' : (v == 1u ? 'R' : 'S'));
        }
        e4_row((uint8)(1u + g), lb);
    }
}

/* ============================ 页 5: 慢充电实验 ============================ */
/* 时间档标签: 22 / 66 / 176 / 660 / 1980 / 6600 us */
static const char * e4_us(uint8 i)
{
    if (i == 0u) { return "22us  "; }
    if (i == 1u) { return "66us  "; }
    if (i == 2u) { return "176us "; }
    if (i == 3u) { return "660us "; }
    if (i == 4u) { return "1980us"; }
    return "6600us";
}

static void e4_page_slow(void)
{
    uint8 i;
    uint8 mono = 1u;

    e4_page("DET 5/6 SLOW CHARGE", "SDA released: does digital follow mV?");

    b_reset();
    b_str("hi="); b_ch((g_slotFast & 0x02u) ? '1' : '0');
    b_str("(22us)  hi="); b_ch((g_slotSlow & 0x02u) ? '1' : '0');
    b_str("(2ms/step)");
    e4_row(0u, lb);

    b_reset();
    b_str("if slow gives NO ACK -> every ACK was charging lag");
    e4_row(1u, lb);

    b_reset();
    b_str("t      dig  mV    |  dig  mV  (strong pull)");
    e4_row(2u, lb);

    for (i = 0u; i < 6u; i++) {
        b_reset();
        b_str(e4_us(i)); b_ch(' ');
        b_dig(g_slowDig[i]); b_ch(' ');
        b_udec(g_slowMv[i], 4u);
        b_str("  | ");
        b_dig(g_strDig[i]); b_ch(' ');
        b_udec(g_strMv[i], 4u);
        e4_row((uint8)(3u + i), lb);
    }

    /* 单调爬升判定 */
    for (i = 1u; i < 6u; i++) {
        if (g_slowMv[i] < g_slowMv[i - 1u]) { mono = 0u; }
    }

    b_reset();
    if (g_strMv[0] > 2600u && g_slowMv[0] < 2600u) {
        b_str("PROOF: strong-pull reads HIGH, weak does not");
    } else if (mono != 0u && g_slowMv[0] < 2600u) {
        b_str("mV rises with time -> internal pullup too weak");
    } else {
        b_str("both paths behave the same -> not a charge issue");
    }
    e4_row(8u, lb);
}

/* ============================ 页 6: 谁是谁 (WHO) ========================== */
static void e4_page_who(void)
{
    uint8 ok0, ok1;

    e4_page("DET 6/7  WHO IS WHO", "which wire does ADC really measure?");

    b_reset();
    b_str("swap01="); b_dig(g_rswap);
    b_str("  P0DIR=0x"); b_hex8(g_rP0DIR);
    b_str(" P0SEL=0x");  b_hex8(g_rP0SEL);
    e4_row(0u, lb);

    b_reset();
    b_str("P0INP=0x"); b_hex8(g_rP0INP);
    b_str(" P2INP=0x"); b_hex8(g_rP2INP);
    b_str(" APCFG=0x"); b_hex8(g_rAPCFG);
    e4_row(1u, lb);

    b_reset();
    b_str("P0=0x"); b_hex8(g_rP0);
    b_str("   (bit0=P0.0 bit1=P0.1)");
    e4_row(2u, lb);

    b_reset();
    b_str("drive test (mV):");
    e4_row(3u, lb);

    b_reset();
    b_str(" P0.0 hi="); b_udec(g_mvP00Hi, 4u);
    b_str("  lo=");     b_udec(g_mvP00Lo, 4u);
    e4_row(4u, lb);

    b_reset();
    b_str(" P0.1 hi="); b_udec(g_mvP01Hi, 4u);
    b_str("  lo=");     b_udec(g_mvP01Lo, 4u);
    e4_row(5u, lb);

    /* 判据: 驱高应 > 2800, 驱低应 < 500 */
    ok0 = (uint8)((g_mvP00Hi > 2800u && g_mvP00Lo < 500u) ? 1u : 0u);
    ok1 = (uint8)((g_mvP01Hi > 2800u && g_mvP01Lo < 500u) ? 1u : 0u);

    b_reset();
    b_str(" => P0.0 ");
    b_str(ok0 ? "OK" : "ABNORMAL");
    b_str("   P0.1 ");
    b_str(ok1 ? "OK" : "ABNORMAL");
    e4_row(6u, lb);

    /* ACK 槽双线对照 */
    b_reset();
    b_str("ACK slot:      SDA    SCL");
    e4_row(7u, lb);

    b_reset();
    b_str(" before clk : ");
    b_dig(g_slotDig[0]); b_ch('/'); b_udec(g_slotMv[0], 4u);
    b_ch(' '); b_dig(g_slotDig[1]); b_ch('/'); b_udec(g_slotMv[1], 4u);
    e4_row(8u, lb);
}

/* ============================ 页 7: ACK 槽双线 ============================ */
static void e4_page_slot(void)
{
    e4_page("DET 7/7  SLOT 2 LINES", "SDA vs SCL measured at the ACK point");

    b_reset();
    b_str("              dig  mV");
    e4_row(0u, lb);

    b_reset();
    b_str("SCL low  SDA  ");
    b_dig(g_slotDig[0]); b_ch(' '); b_udec(g_slotMv[0], 4u);
    e4_row(1u, lb);

    b_reset();
    b_str("SCL low  SCL  ");
    b_dig(g_slotDig[1]); b_ch(' '); b_udec(g_slotMv[1], 4u);
    e4_row(2u, lb);

    b_reset();
    b_str("SCL HIGH SDA  ");
    b_dig(g_slotDig[2]); b_ch(' '); b_udec(g_slotMv[2], 4u);
    e4_row(3u, lb);

    b_reset();
    b_str("SCL HIGH SCL  ");
    b_dig(g_slotDig[3]); b_ch(' '); b_udec(g_slotMv[3], 4u);
    e4_row(4u, lb);

    b_reset();
    b_str("if SDA stays high while SCL goes low");
    e4_row(6u, lb);
    b_reset();
    b_str("-> the ACK lock was SCL, not SDA (swap)");
    e4_row(7u, lb);
}

/* ============================ 页 8: 系统健康 ============================== */
/* v44: T1 有没有停 / 延时有没有在超时 / 地址扫描干到第几个 —— 一屏看完 */
static void e4_page_health(void)
{
    uint16 t1a, t1b;

    e4_page("DET 8/8  SYS HEALTH", "timer1 sanity + watchdog state");

    /* T1 还在不在跑: 隔 2ms 取两次, 差值为 0 就说明时钟停了 */
    t1a = NFC_T1Count();
    NFC_DelayMs(2u);
    t1b = NFC_T1Count();

    b_reset();
    b_str("T1 d2ms=");   b_udec(t1b - t1a, 0u);
    b_str("  tov=");     b_udec(NFC_WaitOvf(), 0u);
    b_str(" last=");     b_udec(NFC_WaitLast(), 0u);
    e4_row(0u, lb);

    b_reset();
    b_str("(tov>0 means the wait loop timed out)");
    e4_row(1u, lb);

    b_reset();
    b_str("scan addr done="); b_udec(g_scanCur, 0u); b_str("/127");
    e4_row(3u, lb);

    b_reset();
    b_str("SLEEPSTA 0x"); b_hex8(SLEEPSTA);
    b_str(" wdtN=");      b_udec(g_brN, 0u);
    e4_row(4u, lb);

    b_reset();
    b_str("br step="); b_udec(g_brStep, 0u);
    b_str(" adr=");    b_udec(g_brAddr, 0u);
    b_str(" fg=");     b_hex8(g_brFlag);
    e4_row(5u, lb);

    b_reset();
    b_str("brT1 prev/last ");
    b_udec(g_brT1Prev, 0u); b_ch('/'); b_udec(g_brT1Last, 0u);
    e4_row(6u, lb);

    b_reset();
    if (NFC_WaitOvf() != 0u) { b_str("!! T1 waits TIMED OUT - report me"); }
    else                     { b_str("all delays normal"); }
    e4_row(8u, lb);
}

/* ============================ CRASH 页 (看门狗专用) =======================
 * v44: 固件一旦被看门狗抬走, 开机就直奔这里 —— 屏上给出"最后一刻在干什么"。
 * 与普通输出不同: 这一页**不能**依赖当次运行的数据, 全部读 __no_init 面包屑。
 */
static void e4_page_crash(void)
{
    uint8 r = g_brRst;

    e4_page("!! WDT FIRED !!", "watchdog caught a freeze - here is where");

    b_reset();
    b_str("SLEEPSTA 0x"); b_hex8(r);
    b_str("  wdtN ");     b_udec(g_brN, 0u);
    e4_row(0u, lb);

    b_reset();
    b_str("frozen at step "); b_udec(g_brStep, 0u); b_str("/15");
    e4_row(2u, lb);

    b_reset();
    b_str("addr-scan progress "); b_udec(g_brAddr, 0u); b_str("/127");
    e4_row(3u, lb);

    b_reset();
    b_str("T1 wait timeouts="); b_udec(g_brOvf, 0u);
    e4_row(4u, lb);

    b_reset();
    b_str("T1 prev/last ");
    b_udec(g_brT1Prev, 0u); b_ch('/'); b_udec(g_brT1Last, 0u);
    e4_row(5u, lb);

    b_reset();
    if (g_brT1Prev == g_brT1Last) { b_str("=> T1 FROZEN at last breath"); }
    else                          { b_str("=> T1 was still running"); }
    e4_row(6u, lb);

    b_reset();
    b_str("brFlag="); b_hex8(g_brFlag);
    b_str(" (A5 = trail valid)");
    e4_row(7u, lb);

    b_reset();
    b_str("re-power to restart");
    e4_row(8u, lb);
}

/* ============================ main ======================================== */
int main(void)
{
    uint8  pidx = 0u;
    uint8  mode = 0u;
    uint16 t;
    uint8  rst;

    clock_init();
    uart_init();                    /* 无害: 只是再也收不到, 引脚不冲突 */
    uputs("\r\n===== v44: wdt + breadcrumb =====\r\n");

    /* ---- v44 第一件事: 记下这次是怎么启动的 ----
     * SLEEPSTA.RST[1:0] (bit2:1): 00=上电/欠压  01=外部复位  10=看门狗复位
     * 必须尽早读, 后面的时钟/外设初始化可能会动相关状态。 */
    rst = SLEEPSTA;
    NFC_WdtFeed();                  /* 无害: 上电时还没 Arm, 写了不起作用 */
    if ((rst & 0x06u) == 0x04u) {   /* 被看门狗抬走过 -> 说明确实卡死过 */
        if (g_brN < 0xFFFFu) { g_brN++; }
        g_brRst = rst;
    }

    /* NIRQ (P1.0) 输入 + 上拉 */
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;         /* PDUP1=0 -> Port1 上拉 */

    NFC_T1Init();
    NFC_TimebaseCal();

    /* ---- 屏: 先证明屏本身通了, 再谈其它 ---- */
    EPD_Init();
    EPD_Fill(0xFF);

    /* ---- 如果是看门狗复位: 立刻把"卡在哪"甩到屏上, 然后停住 ---- */
    if ((rst & 0x06u) == 0x04u) {
        e4_page_crash();
        e4_flush();
        /* 停住不再往下跑; 还得继续喂狗, 否则 1 秒后又被抬走 */
        for (;;) { NFC_WdtFeed(); NFC_DelayMs(20u); }
    }
    g_brN    = 0u;                  /* 正常启动: 计数归零 */
    g_brFlag = 0u;

    e4_page("LG290 NFC   v44", "watchdog armed - a freeze will self-report");
    e4_row(0u, "T1 bounded waits + 1s watchdog");
    e4_row(1u, "if it freezes -> CRASH page");
    e4_row(2u, "baseline SEQ next, please wait");
    e4_flush();

    e4_bus_float();
    e4_p23_float();

    NFC_WaitClr();
    NFC_WdtArm();                   /* ★ 从此刻起, 任何 >1s 的卡死都会被抬走 */

    /* 首轮基线: 不等 RF, 直接上电跑一轮, 保证第一屏就有数据 */
    e4_seq_once(0u);

    for (;;) {
        uint16 hit  = 0u;
        uint16 slot = 0u;

        /* ---- 观测窗口: P2.3 浮空, 10ms 间隔轮询 RF 场耦合 ---- */
        e4_br_set(99u, 0u);          /* ★ 99 = 主循环观测窗口 */
        e4_bus_float();
        e4_p23_float();
        for (t = 0u; t < 150u; t++) {
            if ((P2 & 0x08u) != 0u) { hit++; }
            if (NFC_NirqLevel() == 0u) { g_nirqLo++; }
            slot++;
            g_polls++;
            NFC_DelayMs(10u);
            if ((t & 0x1Fu) == 0u) { e4_feed(); }   /* 每 ~320ms 喂一次狗 */
        }
        g_hits  = hit;
        g_slots = slot;

        /* ---- RF 场判定 + 供电 ---- */
        if (e4_pct(hit, slot) > 20u) {
            /* 检测到 RF 才拉起 VDDEX, 然后重跑一轮完整诊断 */
            g_rfSeq++;
            mode = (uint8)((g_rfSeq & 1u) != 0u ? 1u : 0u);
            e4_seq_once(mode);
            pidx = 0u;                       /* 有 RF 时优先看主结果页 */
        } else {
            pidx++;
            if (pidx > 8u) { pidx = 0u; }
        }

        if      (pidx == 0u) { e4_page_main(); }
        else if (pidx == 1u) { e4_page_matrix(); }
        else if (pidx == 2u) { e4_page_timing(); }
        else if (pidx == 3u) { e4_page_second(); }
        else if (pidx == 4u) { e4_page_scan(); }
        else if (pidx == 5u) { e4_page_slow(); }
        else if (pidx == 6u) { e4_page_who(); }
        else if (pidx == 7u) { e4_page_slot(); }
        else                 { e4_page_health(); }

        e4_flush();
        e4_feed();
    }
}

#endif /* V40_EPD */
#endif /* __V40_EPD_H__ */
