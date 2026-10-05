V30E_BLOCK = r'''#if defined(V30_RFWATCH) && (V30_RFWATCH != 0)
/* ★★★★ v30e: 顺序实验 —— "RF 场先到 -> 再开 VDDEX" ★★★★
 *
 * ★★ 本轮来自用户的指正(核心思路):
 *   之前所有实验的**顺序反了** —— 不能先拉高 P2.3。
 *   芯片在 RF-only(场取电)模式下, 会用内部电路把 VDDEX 节点**牵制**住;
 *   此时 MCU 抢着推挽拉高, 两股力量顶牛, 节点被钉在 ~0.6V, 于是既供不上电、
 *   也把 I2C 侧拖死。正确顺序应该是:
 *
 *     RF 场出现 -> 芯片检测到 -> [芯片通知主机 NIRQ] -> **主机才拉 P2.3**
 *               -> VDDEX 正常升到 3.2V -> 芯片进 Serial 模式 -> I2C 可用
 *
 * ★ 但 v30b/c/d 全程实测数据说话: **NIRQ 从未拉低过 (nLo 恒等于 0)**。
 *   => 用户说的"通知主机"这一环, 在这块板 / 这种模式下**确实不存在**。
 *   => 所以本固件退一步, 改用 **RF 场在 P2.3 浮空节点上耦合出的电位抬升**
 *      本身当触发源(v30b/c/d 已反复证明: 手机贴上 -> P23 读数=1、
 *      line -> 0x03, 且能稳定维持几十秒)。
 *
 * ★ 本固件做两组对比, 一轮跑完就能定死结论:
 *     [A] 对照组   上电就硬拉 P2.3(不等 RF) -> 读回引脚真实电平 + 试 I2C
 *     [B] 实验组   浮空监测, **检测到 RF 场之后才**拉 P2.3 -> 读回 + 试 I2C
 *
 * ★★ 关键诊断手法(不需要万用表):
 *     CC2530 在**推挽输出高**的状态下读 P2 寄存器, 得到的是**引脚真实电平**:
 *       读到 1  -> 引脚确实 >= VIH(约 2.3V) -> VDDEX 真供上去了
 *       读到 0  -> 引脚被外部钉在 <2.3V     -> 存在牵制 / 钳位
 *     这一位就足以判定 VDDEX 到底有没有真正起来。
 *
 * 打印字段:
 *   [RFW] n / p23 / nirq / line / nHi / nLo / SEQ
 *   >>> SEQ#k <<<            第 k 轮序列
 *   P2.3 读回 = x            ★ 关键: 1=供上, 0=被钉低
 *   Decide9 k / s5           0 REAL 1 LATE 2 NONE 3 STUCK
 *   ReadMem[i] rm=           0=OK(并打印 blk) 1=NOACK
 */

/* P2.3 放开成三态输入 —— 用于浮空观测 RF 场耦合 */
static void v30e_p23_float(void)
{
    P2SEL &= (uint8)~0x02u;   /* P2SEL.1 = P2.3 功能位(0=GPIO) */
    P2INP |= 0x08u;           /* 三态: 不内拉, 只看外部 */
    P2DIR &= (uint8)~0x08u;   /* 输入 */
}

/* P2.3 推挽拉高 */
static void v30e_p23_drive_hi(void)
{
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2    |= 0x08u;
}

/* SDA/SCL 全放开(三态不内拉) —— 观测时不打扰芯片 */
static void v30e_bus_float(void)
{
    P0SEL &= (uint8)~0x03u;
    P0INP |= 0x03u;
    P0DIR &= (uint8)~0x03u;
    P2INP |= 0x20u;           /* PDUP0=1 -> Port0 三态 */
    P0IEN &= (uint8)~0x03u;
}

/* 一轮完整序列: 拉 P2.3 -> 读回 -> gpio_prep -> 问 I2C -> 松开 */
static void v30e_seq_once(uint16 seq)
{
    uint8  s5[5], k, blk[8], rm, drv;
    uint8  i, j;

    uputs("\r\n  >>> SEQ#"); udec(seq); uputs(" 现在才拉起 P2.3 <<<\r\n");

    v30e_p23_drive_hi();
    NFC_DelayMs(10u);                       /* 让 VDDEX 电容充起来 */

    drv = (uint8)((P2 & 0x08u) ? 1u : 0u);  /* ★ 推挽输出下读回引脚真实电平 */
    uputs("    P2.3 读回 = "); udig(drv);
    if (drv != 0u) { uputs("  [OK] VDDEX 维持住高电平(>=VIH 2.3V)"); }
    else           { uputs("  [NG] 被牵制/钉低(<VIH), 顺序仍不对或有钳位"); }
    uputs("\r\n");

    NFC_GpioPrepPub();                      /* SDA/SCL 推挽; P2.3 保持输出高 */

    k = NFC_Decide9(0x55u, 0u, s5);
    uputs("    Decide9 k="); udig(k);
    uputs("  s5=");
    for (i = 0u; i < 5u; i++) { udig((uint8)(s5[i] & 0x01u)); }
    uputs("\r\n");

    /* 连读 3 次, 给芯片进入 Serial 模式的时间窗 */
    for (i = 0u; i < 3u; i++) {
        rm = NFC_ReadMem(0x55u, 0x0000u, 8u, blk, (uint8 *)0);
        uputs("    ReadMem["); udig(i); uputs("] rm="); udig(rm);
        if (rm == 0u) {
            uputs("  blk=");
            for (j = 0u; j < 8u; j++) { uh8(blk[j]); uputc(' '); }
            uputs("\r\n    *** I2C TONG LE! rm=0 ***\r\n");
        } else {
            uputs("  (NOACK)");
            uputs("\r\n");
        }
        NFC_DelayMs(60u);
    }

    /* 松开: 断电 + 总线/引脚全放开, 回到浮空观测 */
    P2 &= (uint8)~0x08u;
    NFC_DelayMs(2u);
    v30e_bus_float();
    v30e_p23_float();
}

int main(void)
{
    uint16 n    = 0u;
    uint16 seq  = 0u;
    uint16 nHi  = 0u;
    uint16 nLo  = 0u;
    uint8  armedPrev = 0u;

    clock_init();
    uart_init();
    uputs("\r\n===== v30e SEQ-TEST: RF 先到 -> 再开 VDDEX =====\r\n");

    /* NIRQ(P1.0) 输入 + 上拉 */
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;   /* PDUP1=0 -> Port1 上拉 */

    v30e_bus_float();
    v30e_p23_float();

    NFC_T1Init();
    NFC_TimebaseCal();

    /* ---------- [A] 对照组: 不等 RF, 上电即硬拉 ---------- */
    uputs("\r\n---- [A] 对照组: 不等 RF 场, 上电直接硬拉 P2.3 ----\r\n");
    v30e_seq_once(0u);
    NFC_DelayMs(300u);

    /* ---------- [B] 实验组: 检测到 RF 才拉 ---------- */
    uputs("\r\n---- [B] 实验组: 浮空监测, 检测到 RF 场后才拉 P2.3 ----\r\n");
    uputs("现在把手机贴上线圈并保持; 每检测到一次 RF 就跑一轮 SEQ\r\n\r\n");

    for (;;) {
        uint8 p23   = (uint8)((P2 & 0x08u) ? 1u : 0u);
        uint8 nirq  = NFC_NirqLevel();
        uint8 lines = (uint8)(P0 & 0x03u);

        if (p23  != 0u) { nHi++; }
        if (nirq == 0u) { nLo++; }

        uputs("[RFW] n=");   udec(n);
        uputs(" p23=");      udig(p23);
        uputs(" nirq=");     udig(nirq);
        uputs(" line=0x");   uh8(lines);
        uputs(" nHi=");      udec(nHi);
        uputs(" nLo=");      udec(nLo);
        uputs(" SEQ=");      udec(seq);
        uputs("\r\n");

        /* ★ 只在 RF 场"新出现"(p23 由 0 变 1) 时才开 VDDEX */
        if (p23 != 0u && armedPrev == 0u) {
            seq++;
            v30e_seq_once(seq);
            NFC_DelayMs(400u);      /* 松开后等 VDDEX 电容泄放, 避免误判仍在场 */
        }
        armedPrev = p23;

        n++;
        { volatile uint32 w = 0u; while (w < 15000UL) { w++; } }
    }
}
#endif  /* V30_RFWATCH */
'''


import os

def main():
    path = os.path.join("src", "main.c")
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    # 1-based: 1418 .. 1550  ->  0-based slice [1417:1550]
    old = lines[1417:1550]
    assert old[0].startswith("#if defined(V30_RFWATCH)"), "unexpected start: %r" % old[0]
    assert old[-1].startswith("#endif"), "unexpected end: %r" % old[-1]

    new = V30E_BLOCK
    if not new.endswith("\n"):
        new += "\n"

    lines[1417:1550] = [new]
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.writelines(lines)
    print("OK: replaced lines 1418-1550 -> v30e (%d lines removed)" % len(old))


main()
