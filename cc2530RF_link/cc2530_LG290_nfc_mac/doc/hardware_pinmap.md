# LG290 价签「物理连接层」提取报告

> 方法：从原厂全镜像 `lg290_flat.bin` (256 KB) 逐字节反汇编，把所有对
> `P0/P1/P2`、`P0DIR/P1DIR/P2DIR`、`P0SEL/P1SEL/P2SEL`、`P0INP/P1INP/P2INP`
> 的写操作全部扫出并还原，得到「固件眼里的接线图」。
> 工具：`tools/pincfg.py`（本次新建）。

---

## 1. NFC 通道（MN63Y1213，两线 bit-bang I²C）

| 信号 | MCU 引脚 | 寄存器配置 | 原厂证据地址 |
|------|----------|-----------|--------------|
| **SDA** | **P0.0** | `P0SEL.0=0`；`P0DIR.0=1`；读时 `ANL P0DIR,#FE` 转输入 | `0x7D44/0x7D47/0x7D50`；`0x7B96` `ORL P0DIR,#03`；`0x7CBD` `ANL P0DIR,#FE` |
| **SCL** | **P0.1** | `P0SEL.1=0`；`P0DIR.1=1` | `0x7D44/0x7D4D`；`0x7B96` |
| **NIRQ** | **P1.0** | `P1SEL.0=0`；`P1DIR.0=0`（输入） | `0x7D4A/0x7D53` |
| **VDDEX** | **P2.3** | `P2SEL.1=0`；`P2DIR.3=1`；**高 = 上电** | `0x7D60/0x7D63/0x7D66` (`SETB P2.3`) |

- 从地址：7 位 **0x55**（原厂 `0x7DE5: MOV A,#0x55` 写入 XRAM `0x1B61`），
  读地址字节 = `0x55<<1|1 = 0xAB`，写地址字节 = `0xAA`。
- 原厂 **mode 1（NFC）上电只做 `SETB P2.3`，没有 VDDEX 复位脉冲**；
  那个「高→低→高」脉冲只存在于 **mode 2 后端**（bank1 `0xF57C/0xF586`）。

### 原厂 I²C 位层（逐字节核对）

```
START  0x7BEE : ORL P0DIR,#01 → SCL hi → SDA hi → dly(78) → SDA lo → dly(3C) → SCL lo → dly(3C)
STOP   0x7BCC : ORL P0DIR,#03 → SDA lo → dly(3C) → SCL hi → SDA lo → dly(3C) → SDA hi → dly(78)
写字节 0x7B8F : 每位 SDA 设值 → dly(3C) → SCL hi → dly(78) → SCL lo → dly(3C)，MSB 先行
读 ACK 0x7B5F : ANL P0DIR,#FE(SDA 输入) → SCL hi → dly(3C) → MOV A,P0 → 取 P0.0 → SCL lo
```

> ⚠️ 原厂 ACK **只采一个采样点**（SCL 抬高后），返回 `R1 = SDA 电平`，
> `R1==0`（低）才算 ACK。

### 读/写命令帧（thunk 解析后）

| thunk | 目标 | 含义 |
|-------|------|------|
| `0x18C0` | `0x7BEE` | START |
| `0x18BA` | `0x7BCC` | STOP |
| `0x18B4` | `0x7B8F` | 写字节 |
| `0x18AE` | `0x7B5F` | 读 ACK/读位 |
| `0x18CC` | `0x7CC7` | write_cmd |
| `0x18C6` | `0x7C10` | read_cmd |

- `write_cmd(0x7CC7)`：入口 `LCALL 0x18C0(START)`，出口 `LCALL 0x18BA(STOP)`，**无前置 STOP**。
- `read_cmd(0x7C10)`：入口 START，出口 STOP。
- 协议是**直接块寻址**（首字节 = 块地址，无命令码 0x08/0x18），16 字节一块。

---

## 2. EPD 通道

| 信号 | MCU 引脚 | 配置 | 原厂证据 |
|------|----------|------|----------|
| DC | P0.2 | GPIO 输出 | `0x116EA` |
| MOSI | P0.3 | **硬件 SPI**（`P0SEL.3=1`） | `0x11721` |
| CS | P0.4 | GPIO 输出 | `0x116EC` |
| SCK | P0.5 | **硬件 SPI**（`P0SEL.5=1`） | `0x11724` |
| PWR | P0.7 | GPIO 输出 | `0x11746` |
| RST | P1.1 | GPIO 输出 | `0x11739` |

与 `src/epd2in9.h` 的宏定义**完全一致**。

---

## 3. 结论

**原厂 NFC = P0.0/P0.1，与你 `_mac` 的 `nfc.c` 引脚映射一字不差；
EPD 引脚也与 `epd2in9.h` 一致。** 也就是说：

> 「接线假设」这一层没有问题——不是接错脚。

那么 `_mac` 仍然全部 `NOACK` 的原因，只剩下面三类（都不在「引脚映射」里）：
1. **电气层**：VDDEX(P2.3) 是否真的给 MN63Y1213 供上了电；SDA/SCL 外部
   上拉是否存在；总线是否被钳死。
2. **时序层**：`_mac` 的位延时 `dly_i2c_s/l` 折算出来的实际 SCL 频率
   是否落在手册要求的 **20 kHz–100 kHz** 窗口内。
3. **器件层**：这张价签是否真的装了 MN63Y1213、或它是否已损坏。

---

## 4. 「怎么看物理层」——工具能力说明

| 手段 | 能看到什么 | 看不到什么 |
|------|-----------|-----------|
| **固件反汇编**（本报告） | MCU 引脚 ↔ 外设的映射、上电/掉电时序、位协议 | 板子实际走线、器件是否焊好 |
| **IAR + CC Debugger（C-SPY）** | **引脚实时电平**：Register 窗口看 `P0/P0DIR/P0INP/P2/P2DIR`；在 I²C 例程下断点单步，看 SDA/SCL 翻转 | 走线本身（但能验证每个脚的实际行为） |
| **烧录软件**（SmartRF Flash Programmer / CC2531 等） | 芯片型号、Flash 大小、IEEE 地址、lock bits、读写/校验 flash | **板级连线（看不到）** |

CC2530 调试口：`P2.1 = DD`，`P2.2 = DC`（+ VCC/GND/RESET，共 5 线）——
照片里那束 5 色线很可能就是它。

---

*生成：2026-10-05 · 工具：tools/pincfg.py*
