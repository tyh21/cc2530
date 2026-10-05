/**
 * @file    nfc.h
 * @brief   MN63Y1213 (Panasonic NFC Tag LSI, 4Kbit FeRAM) 主机侧 I2C 驱动
 *
 * ============================================================================
 * ★ 本版依据 = 松下官方数据手册 (MN63Y1213-E1, 113 页 PDF)
 *   文件: ../nfc_doc/MN63Y1213_DS.pdf   (第 2 章引脚 / 第 3 章存储器 / 第 5 章串行通信)
 *
 * 【引脚 Table 2-1】SON8
 *   1 VB   线圈   2 VDDEX 触点供电(1.7~3.6V)   3 VSS GND   4 VA  线圈
 *   5 NIRQ 开漏中断输出   6 SCL 输入   7 SDA 开漏 I/O   8 VDDA 内部模拟电源
 *   手册原文(P.63 附近):
 *     "The SDA(IO) and NIRQ pins are open-drain output. Pull up these pins to
 *      the same voltage level as the power supply."
 *     "I2C is an N-ch open drain pin, so should be pulled up to VDDEX externally.
 *      It is available between the frequencies 20 kHz and 100 kHz.
 *      Start the access tBoot after applying VDDEX."
 *   => SDA/SCL/NIRQ 都靠**外部上拉**; SCL 只做输入(所以主机可以推挽驱动 SCL);
 *      SDA 必须按开漏用; 上电后要等 tBoot 才能访问。
 *
 * 【Table 5-1】I2C 规格: 20 kHz ~ 100 kHz, Slave, 7 位地址 + 8 位数据
 *
 * 【5.2.3 + Table 3-9】从地址: FeRAM 系统区 HW1 地址 0x01EF 的 I2C_SLV 决定,
 *   默认 0x54; 原厂固件写的是 0x55 (0x7DE5: MOV A,#0x55)。两个都要试。
 *
 * 【Table 5-5】命令表(命令码就是 I2C 的第一个数据字节)
 *   READ  0x08 : 0x08 | 起始地址(2B 大端) | 长度n(1B)   响应 = 状态 + 数据(n)
 *   WRITE 0x18 : 0x18 | 起始地址(2B) | 长度n | 数据(1..251)  响应 = 状态
 *   RREG  0x68 : 0x68                                  响应 = 状态 + 数据(1)
 *   WREG  0x78 : 0x78 | 数据(1)                          响应 = 状态
 *   STATUS     : **没有命令字节** —— 直接一个"从机发送请求"(地址+R/W=1)
 *
 * 【Table 5-3/5-4】状态字节: bit7..6 保留; bit5 RFCMD_IRQ; bit4 RFDET_IRQ;
 *   bit3..0 = CMD_RES: 0x5=正常结束 0x7=BUSY 0x8=未实现 0xA=参数错
 *                      0xB=自诊断错 0x9=tunnel 错 0x0=无信息
 *
 * 【5.2.6 时序图 —— 第一次按手册落实的关键点】
 *   t0: START + 从地址 + R/W(L) -> 芯片 ACK; 接着送命令数据, 每字节 ACK; 主机 STOP。
 *       (**手册明确写: "The RFID returns an ACK if the slave address is identified.")
 *   t1: 主机停时钟, 芯片内部处理。
 *   t2: 处理完成, 芯片把 NIRQ 拉低。
 *   t3: 主机看到 NIRQ 低 -> 再 START + 从地址 + R/W(H) -> 芯片 ACK 并松开 NIRQ;
 *       主机给时钟读状态+数据, 最后一字节回 NACK 后 STOP。
 *
 * ============================================================================
 * ★★ v4 的重大变化: 用 Timer1 当时间基准, 并把总线画到屏上 ★★
 *
 * 为什么之前 4 版都没结论: 所有"频率/时序"都是拿**软件延时循环估**出来的 ——
 * 一版读出 10kHz, 一版读出 254kHz, 差 25 倍, 说明一个都不能信。而 I2C 的合法
 * 窗口只有 20~100kHz: 跑快了芯片不认, 跑慢了也一样。测量工具本身不可信,
 * 后面的推理就全是空的。
 *
 * 所以 v4:
 *   1) 位周期交给 Timer1 (32MHz 晶振, 全芯片唯一可靠时基) 决定, 目标 40us=25kHz;
 *      并用 32.768kHz 睡眠定时器交叉校准 T1 的 tick 长度(两把尺子互校),
 *      把"真实位周期/kHz"变成一个屏上能读到的实测值。
 *   2) SDA 按开漏语义用(低=驱动, 高=先推挽抬一下再放开), 避免推挽高压着从机的 ACK。
 *   3) 新增**采样示波**: 主机自己产生 START+字节+ACK+STOP 的波形, 同时按固定
 *      节拍(4us)把 SCL/SDA 电平存下来, 直接画在墨水屏上。
 *      >>> 这样"第 9 个时钟上 SDA 到底为什么是低"就成了一幅能看的图, <<<
 *          而不是再靠一个采样点猜。
 *   4) 实测 SDA/SCL 的上升/下降时间(单位 0.1us) —— 直接回答"外部上拉到底在不在",
 *      这是"硬件没问题"这句话必须过的唯一一道量化关卡。
 *
 * ============================================================================
 * ★★★ v7: Type 3 (FeliCa) 适配 + ACK 判定放宽 ★★★
 *
 * v6 诊断结论: 硬件 DC 层全干净(25kHz/上拉/去耦 OK), 128 全 REAL 是边沿耦合假象,
 *   盲读全 FF = 无设备响应。根因在软件两层:
 *
 *   1) ACK 判定过严 (cmd_send / resp_read):
 *      旧逻辑每个字节都要求 NFC_ACK_REAL, 多字节长帧(如 WRITE 68 字节)必因某
 *      字节被判 LATE 而中途 return 0u, 命令从未发完 —— 解释了"能改状态不能配置"。
 *      ★ 修复: 从地址字节仍要求 REAL, 数据字节只要不是 NONE 就继续发。
 *
 *   2) NDEF 格式错误 (NFC_WriteTextEx / NFC_ReadTextEx):
 *      MN63Y1213 是 NFC Forum Type 3 Tag (FeliCa / JIS 6319-4), 不是 Type 2。
 *      FeRAM 布局:
 *        Block 0 (0x0000) = Attribute 属性块 (16B), 不是 NDEF!
 *          [0]=Ver(0x10) [1]=Nbr [2]=Nbw [6..7]=NmaxB [10]=WrFlag [12..13]=NdefLen [14..15]=Ln
 *        Block 1+ (0x0010+) = NDEF 记录 (从 block 1 开始)
 *      旧代码把 NDEF 记录直接写到 0x0000, 覆盖了 Attribute 块, 导致手机读到属性
 *      错乱("可写:不")、CC2530 自己也读不回。
 *      ★ 修复: 读时从 block 1(0x0010) 解析 NDEF; 写时保护 block 0, NDEF 从
 *        block 1 按块写, 最后更新 block 0 的 NdefLen/Ln 字段。
 *
 * 硬件接线 (LG 213 / 290 板同构)
 *   P0.0 = SDA    P0.1 = SCL    P2.3 = VDDEX    P1.0 = NIRQ
 *   墨水屏用的是 P0.2~P0.7 / P1.1 / P1.2, 与上面四根线不冲突(已核对 epd2in9.h)。
 * ============================================================================
 */
#ifndef __NFC_H__
#define __NFC_H__

#include <ioCC2530.h>

#ifndef _UINT8_T_DEFINED
#define _UINT8_T_DEFINED
typedef unsigned char  uint8;
#endif
#ifndef _UINT16_T_DEFINED
#define _UINT16_T_DEFINED
typedef unsigned short uint16;
#endif
#ifndef _UINT32_T_DEFINED
#define _UINT32_T_DEFINED
typedef unsigned long  uint32;
#endif

/* ------------------------------------------------------------ 从地址 ------ */
/* ★ v11: 原厂固件 0x7DE5 写入 XRAM 0x1B61 的值是 0x55, 不是手册默认 0x54 */
#define MN63Y_SLV_DEFAULT  0x55u   /* 原厂固件实际使用值 (0x7DE2: MOV A,#0x55) */
#define MN63Y_SLV_FACTORY  0x55u   /* 原厂固件里的值 */

/* ------------------------------------------------------------ 命令码 ------ */
#define MN63Y_CMD_READ     0x08u
#define MN63Y_CMD_WRITE    0x18u
#define MN63Y_CMD_RREG     0x68u
#define MN63Y_CMD_WREG     0x78u

/* 状态字节 CMD_RES (低 4 位) */
#define MN63Y_ST_OK        0x05u
#define MN63Y_ST_BUSY      0x07u
#define MN63Y_ST_NOIMPL    0x08u
#define MN63Y_ST_PARAM     0x0Au
#define MN63Y_ST_SELFDIAG  0x0Bu

/* ------------------------------------------------------------ 容量 -------- */
#define MN63Y_FERAM_SIZE   512u
#define NFC_READ_CHUNK     254u
#define NFC_WRITE_CHUNK    251u
#define NFC_TEXT_MAX       96

/* ------------------------------------------------------------ 返回码 ------ */
#define NFC_OK             0
#define NFC_ERR_NOACK      1
#define NFC_ERR_STATUS     2
#define NFC_ERR_NDEF       3
#define NFC_ERR_PARAM      4
#define NFC_ERR_NIRQ       5

/* ------------------------------------------------------ ACK 判定类别 ------ */
/*
 * 第 9 个时钟上读到"低"有 4 种完全不同的物理来源, 必须分开:
 *   REAL  : 8 位发完后, 从机在 SCL 低电平期就把 SDA 拉低(ACK 建立),
 *           整个 SCL 高电平期都保持低, SCL 一落就松开 -> 手册意义上的真应答。
 *   LATE  : 释放后线靠上拉慢慢恢复, SCL 抬起来时还没回到高 -> 假 ACK。
 *   NONE  : SCL 抬起时就是高 -> 没人应答。
 *   STUCK : SCL 落下去之后 SDA 还是低 -> 有东西钉住线(从机卡住/总线被钳位),
 *           这种情况下任何"ACK"都不算数。
 * 关键: 一定要采 SCL **下降沿之后**那一点 —— 真 ACK 一定在那里已经松开,
 *       上一版只采高电平期内两点, 所以把"卡死"误判成了真应答。
 */
#define NFC_ACK_REAL       0u
#define NFC_ACK_LATE       1u
#define NFC_ACK_NONE       2u
#define NFC_ACK_STUCK      3u

/* ------------------------------------------------------------ 配置 -------- */
void   NFC_SetSwap(uint8 s);          /* 0 = SDA=P0.0/SCL=P0.1 (默认) */
void   NFC_SetDrive(uint8 pp);        /* 1 = SCL 推挽(默认); SDA 恒为开漏语义 */
void   NFC_SetOrder(uint8 o);         /* 1 = 原厂上电顺序(默认) */
/* ★ v17: 上电序列开关
 *   pulse=1 : 发 VDDEX 高→低→高 复位脉冲(v8-v16 默认)
 *   early=1 : 先把 SDA/SCL 推挽驱动高再上 VDDEX(v8-v16 默认);
 *             early=0 改为先把 VDDEX 上稳, 之后才碰两条线 */
void   NFC_SetPwrSeq(uint8 pulse, uint8 early);

/* ===========================================================================
 *                        Timer1 时间基准 (v4 新增)
 * ===========================================================================
 * 全部位时序由 T1 计数决定, 不再依赖软件循环速度。
 * NFC_TimebaseCal() 同时用 T1 和 32.768kHz 睡眠定时器量同一段软件循环,
 * 两把尺子互校 -> 得到 1 个 T1 tick 到底是多少 ns, 再据此把位周期定到 40us。
 */
void   NFC_T1Init(void);
uint8  NFC_T1Ok(void);                /* 1 = T1 在计数(自检通过) */
/* ★ v22: T1Init 时钟切换诊断 */
uint8  NFC_GetT1ClkCmd0(void);        /* 进入 T1Init 时 CLKCONCMD */
uint8  NFC_GetT1ClkSta(void);         /* 切换后 CLKCONSTA */
uint8  NFC_GetT1ClkCmd(void);         /* 切换后 CLKCONCMD */
uint16 NFC_GetT1WaitLoops(void);      /* 轮询圈数 (60000 = 超时死等) */
/* ★★★ v25: 32.768kHz 晶振 / P2.3-P2.4 冲突诊断 (决定性)
 *   返回打包位: bit0=起始 OSC32K 源(0晶/1RC) bit1=强制晶体后源(0生效/1强制不了)
 *   bit2=GPIO 驱高 P2.3 读回(1=能) bit3=晶振模式 P2.3 读回(1=能)
 *   bit4=~2ms 内 ST 动过(1=动) bit5=P2.4 GPIO 驱高读回(1=能)
 *   *stTicks 回填 ST 拍数 */
uint8  NFC_Probe32kXosc(uint16 *stTicks);

/* ★★★ v27: "倒灌源定位"实验 (决定性, 不改硬件)
 *   回答: P2.3(VDDEX) 被拉低时, SDA/SCL 上那点电压到底从哪来?
 *   步骤(全部 MCU 侧, P2.3 始终驱低 = VDDEX OFF):
 *     a) 全部三态   -> 读 SDA/SCL            (bit1:0 = 此时两线电平)
 *     b) 只放开 SDA, SCL 驱低 -> 读 SDA      (bit2 = SDA 是否有电)
 *     c) 只放开 SCL, SDA 驱低 -> 读 SCL      (bit3 = SCL 是否有电)
 *     d) SDA 驱低 200ms 后读 SCL             (bit4 = 拉低 SDA 能否把 SCL 拽下来=耦合)
 *     e) SCL 驱低 200ms 后读 SDA             (bit5 = 反向耦合)
 *     f) P2.3=1 (VDDEX ON), 全三态读两线     (bit7:6 = 正常上电时的两线电平)
 *   打包返回: bit1:0=ta  bit2=tb  bit3=tc  bit4=td  bit5=te  bit7:6=tf
 *   *stTicks 回填 ST 拍数(顺带看时钟是否活) */
uint8  NFC_DiagBackfeed(uint16 *stTicks);
extern uint8  g_bfP23;    /* v27: P2.3 驱高读回 */
extern uint16 g_bfHold;   /* v27: P2.3 放开后撑住 ms */

uint8  NFC_GetT1Forced32(void);       /* v23: 1 = 确认跑 32MHz XOSC(16M 切换失败) */
uint16 NFC_GetCalSpin(void);          /* v22: TimebaseCal 等 ST 的自旋圈数 */
void   NFC_TimebaseCal(void);         /* ★ 启动时调一次: 互校 + 定位周期 */
uint16 NFC_GetTickNs(void);           /* 1 tick = ? ns   (实测/回退值) */
uint16 NFC_GetBitTicks(void);         /* 位周期 = ? tick */
uint16 NFC_GetCalT1(void);            /* dly(500) 的 T1 tick 数        (原始读数) */
uint16 NFC_GetCalSt(void);            /* dly(500) 的睡眠定时器 tick 数 (原始读数) */
uint16 NFC_GetBitUs100(void);         /* 位周期实测值, 0.01us 单位 (v5 已修正 10x) */
uint16 NFC_GetBitNs(void);            /* 位周期实测值, ns (直接判 kHz 用) */
uint8  NFC_GetCalOk(void);            /* 1 = 睡眠定时器可用, 数据是实测的 */

/* ---------------------------------------------------- 位周期可调 (v16) ---- */
/* 手册硬约束: SCL 必须在 20kHz~100kHz。用于扫描"电平全对却 NOACK"的根因。 */
void   NFC_BitSave(void);             /* 保存当前 I2C 位周期 */
void   NFC_BitRestore(void);          /* 还原保存的位周期 */
void   NFC_SetBitUs(uint16 us);       /* 设整周期(us), 高/低各半 */
uint16 NFC_GetBitPerUs(void);         /* 当前整周期实测(us) */
uint16 NFC_MeasSclUs(void);           /* ★ v17: 用睡眠定时器**实测**的 SCL 整周期(us) */

/* ---------------------------------------------------- 上升/下降时间 ------- */
/* 返回 0.1us 单位; 0 = 一直没变到目标电平(上拉缺失/线被钉死) */
uint16 NFC_SdaRiseUs10(void);         /* 强拉低 -> 放开 -> 回到高所需时间 */
uint16 NFC_SclRiseUs10(void);
uint16 NFC_SdaFallUs10(void);         /* 推不动高 -> 拉低所需时间 */

/* ---------------------------------------------------- 上拉/电源诊断 ------- */
/* 数据手册 2.3: "The SDA(IO) and NIRQ pins are open-drain ... Pull up these
 * pins to the same voltage level as the power supply of the host."
 * 且 Figure 2-3 参考电路: SDA/SCL/NIRQ 各 3.3k 上拉到 VDDEX, VDDEX 挂 0.1~2.2uF。
 * 下面这组把"外部上拉到底在不在""VDDEX 有没有去耦电容""线在断电时有没有
 * 倒灌(给芯片偷偷供电, 破坏干净上电复位)"变成能读出来的数字。 */
void   NFC_SetTristate(uint8 on);     /* 1 = 关掉 MCU 内部上拉(三态), 只为测外部上拉 */
uint8  NFC_BusLevel2(void);           /* 按当前配置放开两线后读 P0&0x03 */
uint8  NFC_BusOn(void);               /* VDDEX ON : 高4位=内拉时(SCL,SDA) 低4位=三态时 */
uint8  NFC_BusOff(void);              /* VDDEX OFF: 同上; 低4位=0 才说明线跟着 VDDEX 掉下去 */
uint16 NFC_VddHoldMs(void);           /* 放开 P2.3 后 VDDEX 还撑几 ms(有无去耦电容) */
uint16 NFC_VddHoldMs2(void);          /* ★ v24: 同上但用三态测量(P2INP.3=1), 不内拉 */
uint8  NFC_NirqWatchMs(uint16 ms, uint16 *lows);  /* 盯 NIRQ, 返回 1 = 期间见过低 */

/* ===========================================================================
 *                       采样示波 (v4 新增, 判决性)
 * ===========================================================================
 * NFC_ScopeTx(b): 主机自己产生 "START + b(8 位) + 第9时钟 + STOP" 的完整波形,
 * 同时每 4us 采一次 (SCL,SDA) 电平存到 g_scp[]:
 *   bit0 = SDA 电平(读的是引脚, 所以看到的是线的真实状态)
 *   bit1 = SCL 电平
 * 采样点下标固定, 所以分析位置是确定的:
 *   0..3   : 空闲 + START
 *   4..83  : 8 个数据位 (每位 10 点 = 40us)
 *   84..93 : 第 9 个时钟 (第 86 点上升, 第 91 点下降)
 *   94..125: STOP + 尾部(看从机是否继续钉住 SDA)
 * 派生量:
 *   p = 第 85 点(第 9 时钟抬升前)  e = 第 87 点(+4us)
 *   l = 第 90 点(高电平末期)        a = 第 93 点(下降沿后 +8us)
 *   post = 第 90 点之后 SDA 回高用了几个采样点 (0..14, 0x0F=一直低)
 */
#define SCP_N          126u
/* ★ v43: XDATA 只有 8KB, V40_EPD/V45_WAKE 纯诊断态不分配示波/FeRAM 全缓冲 */
#if (defined(V40_EPD) && (V40_EPD != 0)) || (defined(V45_WAKE) && (V45_WAKE != 0))
extern __xdata uint8 g_scp[1];
#else
extern __xdata uint8 g_scp[SCP_N];
#endif
extern uint8  g_scp_ack;              /* p<<3 | e<<2 | l<<1 | a */
extern uint8  g_scp_post;
void   NFC_ScopeTx(uint8 b);
uint8  NFC_ScpKind(void);             /* 按上面 4 类判定这次的第 9 位 */

/* ------------------------------------------------------------ 电源 -------- */
void   NFC_PowerUp(uint8 bootMs);
void   NFC_PowerOff(void);
void   NFC_DelayMs(uint16 ms);
uint8  NFC_NirqLevel(void);
/* 把 P2.3 短暂放开读**引脚真实电平** -> 判断 VDDEX 在负载下有没有塌 */
uint8  NFC_VddAlive(void);

/* ------------------------------------------------------------ 引脚真值 ---- */
/* 逐脚验证: 强驱 0/1 后读回; 放开后读回(上拉); 交叉驱动查两线短路耦合 */
uint8  NFC_PinDrive(uint8 pin, uint8 level);
uint8  NFC_PinFloat(uint8 pin);
uint8  NFC_DriveAndReadOther(uint8 drivePin, uint8 level);

/* ------------------------------------------------------------ 总线 -------- */
uint8  NFC_BusLevel(void);            /* bit0=SDA bit1=SCL, 0x03 = 上拉正常 */
void   NFC_BusRecover(void);          /* 20 个 SCL 脉冲 + STOP 踢回空闲 */

/* ------------------------------------------------------------ ACK 探测 ---- */
/* addr7: 7 位从地址; 0xFF = 对照实验(一个地址字节都不发)
 * rd   : 1 = 按手册 STATUS 命令顺带读 1 个状态字节
 * 返回 : NFC_ACK_REAL / LATE / NONE / STUCK; 副作用 g_ack_kind / g_status */
uint8  NFC_AckProbe(uint8 addr7, uint8 rd, uint8 *status);
uint8  NFC_StatusCmd(uint8 slv, uint8 *status);
/* 全地址扫描 0x00..0x7F, 返回真应答个数 */
uint8  NFC_ScanAll(uint8 *first, uint8 *late, uint8 *none, uint8 *stuck);
/* 只用指定 R/W 位扫 0x00..0x7F, 返回 REAL 个数; first3 记前 3 个 REAL 地址。
 * ★ v5 新增: 用来钉死"扫描说 128 全 ACK, 单发又说没 ACK"这个自相矛盾。 */
uint8  NFC_ScanRd(uint8 rd, uint8 *first3);
uint8  NFC_Probe(uint8 addr7, uint8 rd);   /* = NFC_AckProbe(addr7, rd, 0) */
/* ★★★ v28: 寄生供电探测 —— SDA/SCL 推挽驱高给芯片充电后, 不碰 P2.3 直接探测 */
uint8  NFC_ProbeParasitic(uint8 addr7, uint16 chargeMs, uint8 *s5);

/* ★ v29: 暴露 gpio_prep 供 main.c 诊断(验证总线"高"是外部上拉还是 MCU 推挽自造) */
void   NFC_GpioPrepPub(void);

extern uint8 g_probe_pre;             /* 第 9 时钟抬升前 SDA 是否已自由 (1 = 自由) */

/* ===========================================================================
 *               v6: 第 9 位「长保持」判决 —— 一次钉死真 ACK vs 假象
 * ===========================================================================
 * v5 自相矛盾: NFC_Probe 说 REAL, 而同屏的 NFC_ScopeTx 画出来的同一笔
 * (ack=0x0F = 四个采样点全高) 说没 ACK。而且 128 个地址、2ms~1s 全部 tBoot
 * 都是 REAL —— 真从机做不到这两点。说明 clock9_kind 的 "REAL" 是假象:
 * 极可能是「上一个数据位驱动低 → 第 9 时钟抬 SCL 的瞬间还没恢复」+ 边沿耦合。
 *
 * NFC_Decide9(): 发完字节后, 把 SCL **长时间保持高**(~1.1ms), 期间多点采样 SDA:
 *     s[0] = SCL 高之前(低电平期末)          s[1] = 刚抬 SCL
 *     s[2] = +100us                          s[3] = +1.1ms(仍高)
 *     s[4] = SCL 落之后 +200us
 *   真从机 ACK: SCL 高期间**一直**低, 落沿后松开 (s[1..3]=0, s[4]=1)
 *   假象:       s[1] 可能低, 但 s[2]/s[3] 必回到高
 *   addr7 = 0xFF -> 一个字节都不发(对照组, 必须全高)
 * 判据只用 s[3]: s[3]=0 才是"真有东西把线按住"。
 */
uint8  NFC_Decide9(uint8 addr7, uint8 rd, uint8 *s5);
/* 无视 ACK 的盲读: START + 地址+R + n 字节 + NACK + STOP (复刻读序列, 看芯片有没有数据) */
uint8  NFC_BlindRead(uint8 addr7, uint8 *dst, uint8 n);
/* 1 = 数据位 "1" 也推挽驱动(复刻原厂 0x7B8F 的 ORL P0DIR,#0x03 + SETB P0.0) */
void   NFC_SetSdaPP(uint8 on);
/* 用「长保持判据」扫 0x00..0x7F(rd=1); 返回 REAL 个数, *holdCnt = s[3] 为低的地址数 */
uint8  NFC_ScanLong(uint8 *first3, uint8 *holdCnt);

/* -------------------------------------------------------- 文档化命令 ------- */
uint8  NFC_WaitNirqLow(uint16 ms);    /* 返回等待毫秒, 0xFF = 超时 */
uint8  NFC_ReadMem(uint8 slv, uint16 addr, uint8 len, uint8 *dst, uint8 *status);
uint8  NFC_WriteMem(uint8 slv, uint16 addr, const uint8 *src, uint8 len, uint8 *status);
/*
 * ★ v31 分步诊断版 ReadMem: 不因 NACK 提前退出, 硬走完并把每一拍的第 9 位判定吐出。
 *   dbg[0]=写方向地址(slv<<1|0) ACK 类型
 *   dbg[1]=块地址            ACK 类型
 *   dbg[2]=读方向地址(slv<<1|1) ACK 类型
 *   dbg[3]=0x5A 哨兵(表示流程走完)
 *   类型: 0 REAL / 1 LATE / 2 NONE / 3 STUCK / 0xFF 未执行
 *   返回值固定 0, 不看返回值, 看 dbg[] 与 dst[]。
 */
uint8  NFC_ReadMemDiag(uint8 slv, uint8 block, uint8 len, uint8 *dst, uint8 *dbg);
/* 同上, 但两段之间用"重复 START"衔接(不下 STOP) —— 排查地址指针被中途 STOP 清掉的情况 */
uint8  NFC_ReadMemDiagRs(uint8 slv, uint8 block, uint8 len, uint8 *dst, uint8 *dbg);
/* ★★★ v45: 手册标准 READ 协议 (第 5 章, 命令码 0x08 + NIRQ 等待 + 状态字节)
 *   返回: 0=OK 1=写事务NACK 2=NIRQ超时 3=读事务NACK 4=状态错 5=参数错
 *   dbg[0..4]=写事务各拍ACK  dbg[5]=读地址ACK  dbg[6]=状态字节  dbg[7]=0x5A哨兵
 *   *nirqMs = NIRQ 拉低等待 ms (0xFF=超时) */
uint8  NFC_ReadStd(uint8 slv, uint16 addr, uint8 len, uint8 *dst,
                   uint8 *dbg, uint8 *nirqMs);
/* ★★★ v45: STATUS 直读 (手册 5.1/5.2.5.5: 不发命令, 直接 slv+R 读 1 字节状态)
 *   数字核死活探针: 读回非 FF = 核活着; FF = 响应通道无数据 */
uint8  NFC_StatusRead(uint8 slv, uint8 *status, uint8 *ackK);
/* ★★★ v45: NIRQ 紧轮询(微秒粒度) —— 抓 < 1ms 的短脉冲; 返回捕获 us, 0xFFFF=超时 */
uint16 NFC_WaitNirqFast(uint16 ms);
extern uint16 g_nirqUsLast;           /* 最近一次 WaitNirqFast 的捕获值(us) */
/* v32 第二字节扫描: START+写地址+cand+STOP, 记录每个 cand 的 ACK 类型到 out[] */
uint8  NFC_ProbeSecondByte(uint8 slv, const uint8 *cand, uint8 n, uint8 *out);
/* v33 逐比特总线读回: 同一次事务内对比"首字节"与"次字节"的真实引脚电平 */
uint8  NFC_Tx8Trace(uint8 addr7, uint8 second, uint8 *rep);
/* v33 地址全扫描(0x00..0x7F): out[a]=0无ACK/1真ACK/2钉死, 返回真ACK个数 */
/* ★ v43: out 改为 2bit 位图, 只需 32 字节(原 128); 地址 a 占 out[a>>2] 的 bit(2*(a&3)):
 *   00=无ACK 01=真ACK 10=钉死。返回真 ACK 个数。 */
uint8  NFC_AddrScanAck(uint8 *out);
/* v34 ACK 矩阵: 定位"假 ACK"触发条件, m[0..5] 为各情形的 (pre,hi,post) 位图 */
uint8  NFC_AckMatrix(uint8 *m);
/* v34 时序扫描: rep[0]=空时钟后的ACK槽; rep[1..5]=释放后延时读到的SDA电平 */
uint8  NFC_AckTimingSweep(uint8 *rep);
/* v35 ADC 实测 SDA 电压(mV): 0=推挽高基准 1=放开静置 2=8空时钟后 3=第9时钟高 4=落下后 */
uint8  NFC_AdcProbe(uint16 *mv);
/* v36 引脚状态快照: 同一时刻取数字读值/真实电压/寄存器, 定位"读到0却是高电平" */
uint8  NFC_PinProbe(uint16 *mv, uint8 *dig, uint8 *regs);
/* v36 静态漏电观测: 不翻时钟, 放开SDA 后在 0/1/5/20ms 取 数字值+电压 */
uint8  NFC_IdleLeakProbe(uint16 *mv, uint8 *dig);
/* ★ v41 慢充电实验: 放开 SDA 后在 22/66/176/660/1980/6600us 读 数字值 + 电压(mV) */
uint8  NFC_SlowReleaseProbe(uint16 *mv, uint8 *dig);
/* ★ v41 强上拉对照: 先推挽驱高充满再放开, 同样时间轴采样 —— 反证充电不足 */
uint8  NFC_StrongPullProbe(uint16 *mv, uint8 *dig);
/* ★ v41 ACK 槽慢速版: 每个采样点前留 2ms, rep[0] = (pre,hi,post) 位图 */
uint8  NFC_AckSlotSlow(uint8 *rep);
/* ★ v42 引脚真值: regs[0..7]=swap01/P0/P0DIR/P0SEL/P0INP/P2INP/APCFG/回读,
 *   mv[0..3]=P0.0高/P0.0低/P0.1高/P0.1低 (mV) */
uint8  NFC_PinTruthProbe(uint8 *regs, uint16 *mv);
/* ★ v42 ACK 槽双线对照: out[0..3]=SDA@pre,SCL@pre,SDA@hi,SCL@hi (mV) */
uint8  NFC_AckSlotMap(uint16 *out, uint8 *dig);
uint8  NFC_RReg(uint8 slv, uint8 *status, uint8 *data);
uint8  NFC_WReg(uint8 slv, uint8 data, uint8 *status);

/* -------------------------------------------------------- 应用层 ---------- */
uint8  NFC_ReadTextEx(uint8 slv, char *out, uint16 max);
uint8  NFC_WriteTextEx(uint8 slv, uint16 addr, const char *text);

/* ----------------------------------------------------- v44 看门狗 / 死等统计 */
void   NFC_WdtArm(void);      /* ~1s 看门狗, 使能后不可关闭 */
void   NFC_WdtFeed(void);     /* 喂狗 (0xA -> 0x5) */
uint16 NFC_WaitOvf(void);     /* t1_wait_ticks 超时次数 */
uint16 NFC_WaitLast(void);    /* 最后一次超时时要等的 tick 数 */
void   NFC_WaitClr(void);
uint16 NFC_T1Count(void);     /* 当前 T1 计数值 */
extern uint8 g_scanCur;       /* 地址全扫描的实时进度 (最后干到哪个地址) */

/* --------------------------------------------------------- 诊断量 --------- */
#if (defined(V40_EPD) && (V40_EPD != 0)) || (defined(V45_WAKE) && (V45_WAKE != 0))
extern __xdata uint8 g_mem[1];
#else
extern __xdata uint8 g_mem[MN63Y_FERAM_SIZE];
#endif
extern uint8  g_ack_kind;
extern uint8  g_status;
extern uint8  g_nirq_ms;              /* 0xFF = NIRQ 一直没低 */
extern uint8  g_ndef_at;
extern uint16 g_mem_len;

#endif /* __NFC_H__ */
