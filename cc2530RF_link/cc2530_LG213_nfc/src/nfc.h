/**
 * @file    nfc.h
 * @brief   MN63Y1213 NFC 标签读写 (Serial / I2C 主机模式, bit-bang)
 *
 * 硬件 (万用表实测确认):
 *   P0.0 = SDA, P0.1 = SCL (软件 I2C, 7 位从地址 0x55)
 *   P2.3 = VDDEX 使能 (高 = 给 MN63Y1213 供电)
 *
 * 帧格式 (原厂固件 yuanma_LG_290 反汇编实证, 非推测):
 *   写/定位 : START  0xAA  ACK  block(8bit)  ACK  [数据...]  STOP
 *   读      : 虚写定位后 STOP，再 START  0xAB  ACK  数据...  STOP
 *   ★ block 号 0-31，每 block 16 字节；4 Kbit = 32 x 16
 *
 * 存储布局 (MN63Y1213 datasheet Ch.3):
 *   block 0-27  = 用户区 (0x0000-0x01BF)
 *   block 28-31 = 系统区 (CONFIG / IDM / PMM / RORF / ROSI ...)
 *
 * 已知坑 (IAR 8051 特有，务必知道):
 *   VDDEX = P2.3，把它拉高会让 P2 锁存 != 0。而 IAR 的伪栈 PSTACK 位于
 *   XDATA 第 0 页(0x0000-0x007F)，是用 MOVX @Ri 访问的 —— MOVX @Ri 的地址
 *   高字节取自 P2 锁存。所以 P2 != 0 期间，所有局部变量读写都会落到 XDATA
 *   的另一页上，压在本工程的 EPD 帧缓冲里。
 *   对策：延时计数用静态 __idata 变量(不走 MOVX)，并且 main 里把所有 NFC
 *   总线操作集中在一段，做完立刻 NFC_PowerOff()，再开始绘图。
 *   => 自己加代码时也照这个规矩：VDDEX 开着的时候不要做大量 XDATA 操作。
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

/* 读 tag 时一次抓多少个 block (1 block = 16 字节) */
#ifndef NFC_RAW_BLOCKS
#define NFC_RAW_BLOCKS   8      /* 8 * 16 = 128 字节，足够装常见 NDEF */
#endif

/* 读到的文本正文最大长度 (含结尾 0) */
#define NFC_TEXT_MAX     64

/* 结果码 */
#define NFC_OK          0
#define NFC_ERR_I2C     1      /* I2C 无应答 */
#define NFC_ERR_NDEF    2      /* 没找到 NDEF Text 记录 */
#define NFC_ERR_PARAM   3      /* 参数非法 */

/* ------------------------------------------------------------------ API --- */

/* 上电/下电 (P2.3 = VDDEX)。PowerOn 内含 tBoot 等待 */
void  NFC_PowerOn(void);
void  NFC_PowerOff(void);

/* 毫秒级忙等（排查时需要在外部单独等 tBoot） */
void  NFC_DelayMs(uint16 ms);

/* 读写指定 block (每 block 16 字节, len<=16) */
uint8 NFC_ReadBlock(uint8 block, uint8 *dst, uint8 len);
uint8 NFC_WriteBlock(uint8 block, const uint8 *src, uint8 len);

/* 连续读 nBlocks 个 block 到 dst，返回实际读到的字节数 */
uint8 NFC_ReadRaw(uint8 startBlock, uint8 nBlocks, uint8 *dst);

/* 读 NDEF 文本正文到 out (自动在抓到的数据里搜索记录) */
uint8 NFC_ReadText(char *out, uint16 max);

/* 反方向：写一条 NDEF Text 到 block 1，手机可读 */
uint8 NFC_WriteText(const char *text);

/* 诊断: 只发 slave 写地址看 ACK。返回 0 = 有 ACK */
uint8 NFC_Probe(void);

/* 诊断: 设总线速率倍率 1=标称(~21kHz) 2=半速 4=1/4速。排查时序用 */
void  NFC_SetSpeed(uint8 mul);

/* 诊断: 扫 0x00-0x7F 全部 7 位地址，返回应答个数，首个应答地址写进 *first */
uint8 NFC_ScanAddr(uint8 *first);

/* 诊断: 释放总线后读回电平 bit0=SDA bit1=SCL（0x03 = 上拉正常） */
uint8 NFC_BusLevel(void);

/* 诊断: 给 20 个 SCL 脉冲 + STOP，把可能卡住的从机踢回空闲态 */
void  NFC_BusRecover(void);

/* 诊断: 0=开漏(释放靠上拉) 1=推挽(完全复刻原厂 0x7D4D 的做法) */
void  NFC_SetDrive(uint8 pp);

/* 诊断: 在 SCL 为低 / 为高两个时刻分别把 SDA 强力拉低再读回。
 * 返回 bit0 = SCL 低时读回的电平, bit1 = SCL 高时读回的电平（1 = 确实拉低了）。
 * 0x03 = 两根线的通路都正常；0x00 = SDA 短路到别的东西或脚焊错。 */
uint8 NFC_PinTest(void);

/* 诊断: 双线互证。master 把 SDA 拉低，然后读回 P0.1 的输入锁存。
 * 返回 bit0 = P0.0 引脚电平读回, bit1 = P0.1 在 SDA 拉低时读到的电平。
 * 正常应为 0x01（SDA=0，SCL 保持上拉）。若 P0.1 也跟着变 -> 两线短路。 */
uint8 NFC_XTalk(void);

/* 诊断: 把 P0.0/P0.1 全部恢复成高阻输入（连内部上拉也关掉） */
void  NFC_BusFloat(void);

/* 诊断: 单独把 SDA 推挽拉高、再单独把 SCL 推挽拉高，返回读回电平。
 * bit0 = SDA 拉高后读回, bit1 = SCL 拉高后读回。
 * 配合"每拉一根就量一次 VDD"，可定位是哪根线在往外部倒灌、拖低整板供电。 */
uint8 NFC_PullHighTest(void);

/*
 * ★ 模拟法引脚诊断 —— 用 ADC(AIN0=P0.0, AIN1=P0.1) 直接量引脚真实电压，
 *   绕开"数字读回"的所有坑（P0DIR=1 时位寻址读到的是输出锁存，恒等于写进去的值）。
 *
 *   自校准，不需要任何标定常数：
 *     先把脚推挽拉低 -> code_lo      再把脚推挽拉高 -> code_hi
 *     code_lo / code_hi 的百分比 = "拉低时残留多少电平"
 *       ~0%   = 确实拉低了，外部没抢 -> 之前 PT=0 是软件读法有问题
 *       >50%  = 外部有强驱动顶住     -> 硬件/走线问题
 */
void  NFC_PinAnalog(void);
extern uint16 g_nfc_a0_lo, g_nfc_a0_hi;   /* P0.0 拉低/拉高时的 ADC 码 */
extern uint16 g_nfc_a1_lo, g_nfc_a1_hi;   /* P0.1 拉低/拉高时的 ADC 码 */

/* ------------------------------------------------------------- 诊断量 ----- */
extern uint8 g_nfc_ack_w;      /* 最后一次发字节后的 ACK: 0=ACK, 1=NACK */
extern uint8 g_nfc_ack_r;      /* 读阶段发 0xAB 的 ACK: 0=OK */
extern uint8 g_nfc_status;     /* 保留 */
extern uint8 g_nfc_lastblk;    /* 最后访问的 block 号 */
extern uint8 g_nfc_errcode;    /* 出错位置: 0=无 1=0xAA无ACK 2=block无ACK */
                               /*           3=数据无ACK 4=0xAB无ACK */
extern uint8 g_nfc_ndef_at;    /* NDEF 记录在 raw 里的偏移, 0xFF=没找到 */
extern uint8 g_nfc_raw_bytes;  /* 本次实际读到的 tag 字节数 */

#endif /* __NFC_H__ */
