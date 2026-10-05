# MN63Y1213 NFC 芯片 I2C 驱动完整逆向分析报告

> 目标固件: `fw_LG290_256k.bin` (256KB, LG290 电子价签原厂固件)
> 逆向工具: `dis8051_v2.py` (自研 8051 反汇编器)
> 逆向日期: 2026-10-04 ~ 2026-10-05
> 芯片: MN63Y1213 (Panasonic, 4Kbit FeRAM, NFC Forum Type 3 Tag)
> 主控: CC2530 (TI, 增强 8051 内核, 16MHz)
> 原厂时钟: 32MHz

---

## 一、核心结论

原厂固件的 NFC 驱动位于代码区 **0x7B00-0x7F30**，通过 XRAM 0x1B61 存储的 7 位从地址
(0x55) 访问 MN63Y1213。原厂使用的是 **非标准 I2C 命令协议 (直接块寻址/EEPROM-like)**，
而非 MN63Y1213 手册第 5 章描述的标准 READ/WRITE 命令码 (0x08/0x18)。

原厂协议特征:
- 第一数据字节是**块地址**，不是命令码
- 结束用 **START** 而非 STOP (非标准!)
- 读多字节时每字节后发**推挽 ACK 脉冲**，无 NACK
- SDA/SCL 均**推挽驱动** (非开漏+上拉)
- START/STOP/写字节 **开头都先 ORL P0DIR,#0x03 确保推挽**

---

## 二、Stub 映射表 (已全部确认)

原厂通过 stub 区 (0x18A8-0x18FC) 间接调用 NFC 物理层和命令层:

| Stub 地址 | 目标地址 | 功能 |
|-----------|----------|------|
| 0x18A8 | 0x7B48 | set_slv: 写 XRAM 0x1B61 (设从地址) |
| 0x18AE | 0x7B5F | read_sda: 读 SDA 单 bit (ACK 读取) |
| 0x18B4 | 0x7B8F | write_byte: 写 8 位 (MSB 先行) |
| 0x18BA | 0x7BCC | START: 非标准 START 序列 |
| 0x18C0 | 0x7BEE | STOP: 原厂 STOP 序列 |
| 0x18C6 | 0x7C10 | read_cmd: 读命令层 (读 N 字节) |
| 0x18CC | 0x7CC7 | write_cmd: 写命令层 (写块地址+数据) |
| 0x18D2 | 0x7D25 | reset: 复位/初始化 |
| 0x18D8 | 0x7D30 | power_on: VDDEX 上电配置 |
| 0x18DE | 0x7D6D | power_off: 下电/释放 |
| 0x18E4 | 0x7DAD | batch_read: 批量读 (16B 一段) |
| 0x18EA | 0x7E5C | batch_write: 批量写 |
| 0x18F0 | 0x7EB9 | (power_on 封装) |
| 0x18F6 | 0x7ECD | (单字节写封装) |
| 0x18FC | 0x7EED | verify_cmd: 校验命令 |
| 0x1DFA | 0xFD1A | write_2byte: 写两字节 |

---

## 三、从地址

- **XRAM 0x1B61** 存储 7 位从地址，默认值 **0x55**
- 由 0x7DE2 初始化: `MOV DPTR,#1B61; MOV A,#55; MOVX @DPTR,A`
- 也可由 set_slv (0x7B48) 的 R1 参数动态写入
- 帧发送前: 读 0x1B61 → `RLC A` (左移 1 位) → `ORL A,#01` (读) 或保持 0 (写)
- 即: **7 位地址 0x55 → 写地址 0xAA, 读地址 0xAB**

---

## 四、物理层原语完整反汇编

### 4.1 START (0x7BCC, stub 0x18BA)

非标准 START 序列 (非 SCL 高时 SDA 下降沿):

```
0x7BCC: PUSH DPL0
0x7BCE: PUSH DPH0
0x7BD0: ORL P0DIR, #03      ; ★ 确保 SDA+SCL 推挽
0x7BD3: CLR P0.0             ; SDA lo
0x7BD5: MOV R2, #3C          ; delay(3C)
0x7BD9: LCALL 0x1590         ; delay
0x7BDC: SETB P0.1            ; SCL hi
0x7BDE: CLR P0.0             ; SDA lo (保持)
0x7BE0: MOV R2, #3C          ; delay(3C)
0x7BE4: LCALL 0x1590          ; delay
0x7BE7: SETB P0.0            ; SDA hi
0x7BE9: MOV R2, #78          ; delay(78)
0x7BEB: LJMP 0x7B53          ; 公共返回 (恢复 DPTR + RET)
```

序列: `ORL P0DIR,#03 → SDA lo → delay(3C) → SCL hi → SDA lo → delay(3C) → SDA hi → delay(78)`

★ 关键: 开头 `ORL P0DIR,#03` 确保推挽 —— 上一步可能是 read_sda/i2c_rx8 留 SDA 在输入模式

### 4.2 STOP (0x7BEE, stub 0x18C0)

```
0x7BEE: PUSH DPL0
0x7BF0: PUSH DPH0
0x7BF2: ORL P0DIR, #01      ; ★ 确保 SDA 推挽 (只 SDA, SCL 已推挽)
0x7BF5: SETB P0.1            ; SCL hi
0x7BF7: SETB P0.0            ; SDA hi
0x7BF9: MOV R2, #78          ; delay(78)
0x7BFD: LCALL 0x1590         ; delay
0x7C00: CLR P0.0             ; SDA lo (= STOP 信号)
0x7C02: MOV R2, #3C          ; delay(3C)
0x7C06: LCALL 0x1590         ; delay
0x7C09: CLR P0.1             ; SCL lo
0x7C0B: MOV R2, #3C          ; delay(3C)
0x7C0D: LJMP 0x7B53          ; 公共返回
```

序列: `ORL P0DIR,#01 → SCL hi → SDA hi → delay(78) → SDA lo → delay(3C) → SCL lo → delay(3C)`
末态: SCL 低, SDA 低

★ 关键: 开头 `ORL P0DIR,#01` 确保 SDA 推挽

### 4.3 write_byte (0x7B8F, stub 0x18B4)

写 8 位, MSB 先行:

```
0x7B8F: MOV A, #F7           ; 函数标记
0x7B91: LCALL 0x0E24         ; 日志/调试
0x7B94: MOV A, R1            ; R1 = 要写的字节
0x7B95: MOV R7, A
0x7B96: ORL P0DIR, #03       ; ★ 确保推挽
0x7B99: CLR P0.0             ; SDA lo (初始)
0x7B9B: MOV R6, #08          ; 8 位循环
; 循环体 (每位):
0x7B9D: MOV A, R7
0x7B9E: MOV C, ACC.7          ; 取 MSB
0x7BA0: JNC 0x7BA6
0x7BA2: SETB P0.0             ; SDA hi (bit=1)
0x7BA4: SJMP 0x7BA8
0x7BA6: CLR P0.0              ; SDA lo (bit=0)
0x7BA8: MOV R2, #3C           ; delay(3C)
0x7BAC: LCALL 0x1590
0x7BAF: SETB P0.1             ; SCL hi
0x7BB1: MOV A, R7             ; (取回字节)
0x7BB2: CLR C
0x7BB3: RLC A                 ; 左移
0x7BB4: MOV R7, A
0x7BB5: MOV R2, #78           ; delay(78)
0x7BB9: LCALL 0x1590
0x7BBC: CLR P0.1              ; SCL lo
0x7BBE: MOV R2, #3C           ; delay(3C)
0x7BC2: LCALL 0x1590
0x7BC5: DEC R6
0x7BC7: JNZ 0x7B9D            ; 循环 8 次
0x7BC9: LJMP 0x7D68           ; 返回 (R7=1)
```

每位序列: `SDA 设值 → delay(3C) → SCL hi → delay(78) → SCL lo → delay(3C)`
★ 开头 `ORL P0DIR,#03` 确保推挽
★ 出口 SCL 低

### 4.4 read_sda (0x7B5F, stub 0x18AE)

读 SDA 单 bit (用于 ACK 判定), 在 SCL 高电平期间采样:

```
0x7B5F: MOV A, #F7           ; 函数标记
0x7B61: LCALL 0x0E24         ; 日志/调试
0x7B64: LCALL 0x7CBD         ; ★ sda_input: ANL P0DIR,#FE (SDA 切输入)
0x7B67: LCALL 0x1590         ; delay
0x7B6A: SETB P0.1            ; SCL hi
0x7B6C: MOV R2, #3C          ; delay(3C)
0x7B70: LCALL 0x1590
0x7B73: MOV A, P0            ; ★ 读 P0 端口
0x7B75: MOV R6, A            ; 存到 R6
0x7B76: MOV R2, #3C           ; delay(3C)
0x7B7A: LCALL 0x1590
0x7B7D: CLR P0.1             ; SCL lo
0x7B7F: MOV R2, #3C           ; delay(3C)
0x7B83: LCALL 0x1590
0x7B86: MOV A, R6
0x7B87: MOV C, ACC.0          ; 取 bit0 = SDA
0x7B89: CLR A
0x7B8A: RLC A                ; C → A.bit0
0x7B8B: MOV R1, A            ; R1 = 0(ACK/SDA低) 或 1(NACK/SDA高)
0x7B8C: LJMP 0x7D68          ; 返回 (R7=1)
```

序列: `sda_input → delay → SCL hi → delay(3C) → 读 P0 → delay(3C) → SCL lo → delay(3C)`
返回: R1 = 0(ACK, SDA 低) 或 1(NACK, SDA 高)
★ 末尾**不切回推挽**! SDA 留在输入模式 (由下一帧 write_byte/START/STOP 切回)

### 4.5 sda_input (0x7CBD)

```
0x7CBD: ANL P0DIR, #FE       ; SDA(P0.0) 切输入
0x7CC0: CLR P0.1             ; SCL lo
0x7CC2: MOV R2, #3C          ; delay(3C)
0x7CC6: RET
```

### 4.6 延时 (0x1590)

参数 R2:R3, 约 0.375us @32MHz
- 0x3C (60) ≈ 22us @32MHz
- 0x78 (120) ≈ 45us @32MHz
- @16MHz 约减半

---

## 五、命令层完整反汇编

### 5.1 write_cmd (0x7CC7, stub 0x18CC)

写命令: 设块地址 + 写数据

```
入口: R1=块地址, R2:R3=XRAM数据指针, R4=数据长度
0x7CC7: MOV A, #F3           ; 函数标记
0x7CC9: LCALL 0x0E24
0x7CCC: MOV dir0A, R1        ; dir0A = 块地址
0x7CCE: MOV R6, A (R2)       ; R6:R7 = XRAM 指针
0x7CD0: MOV R7, A (R3)
0x7CD2: MOV dir0C, R4        ; dir0C = 数据长度
0x7CD4: MOV dir0B, #00       ; dir0B = 字节计数器

0x7CD7: MOV DPTR, #1B61      ; ★ 读从地址
0x7CDA: MOVX A, @DPTR
0x7CDB: MOV dir08, A         ; dir08 = slv
0x7CDD: LCALL 0x18C0         ; STOP (保险)
0x7CE0: MOV A, dir08
0x7CE2: CLR C
0x7CE3: RLC A                ; slv << 1
0x7CE4: MOV R1, A            ; R1 = 写地址 (0xAA)
0x7CE5: LCALL 0x18B4         ; write_byte(写地址)
0x7CE8: LCALL 0x18AE         ; read_sda (ACK)
0x7CEB: MOV A, R1
0x7CEC: JNZ 0x7D14           ; ★ NACK(R1≠0) → 错误
; 写块地址:
0x7CEE: MOV R1, dir0A        ; R1 = 块地址
0x7CF0: LCALL 0x18B4         ; write_byte(块地址)
0x7CF3: LCALL 0x18AE         ; read_sda (ACK)
0x7CF6: MOV A, R1
0x7CF7: JNZ 0x7D14           ; NACK → 错误
; 写数据循环:
0x7CFB: INC dir0B
0x7CFD: MOV A, dir0B
0x7CFF: CLR C
0x7D00: SUBB A, dir0C        ; dir0B < dir0C(长度)?
0x7D02: JNC 0x7D1B          ; 写完 → 跳结束
0x7D04: MOV dir08, dir0B
0x7D07: LCALL 0x7EB4         ; 计算 XRAM 地址 → R1 = 数据
0x7D0A: MOV R1, A
0x7D0B: LCALL 0x18B4         ; write_byte(数据)
0x7D0E: LCALL 0x18AE         ; read_sda (ACK)
0x7D11: MOV A, R1
0x7D12: JZ 0x7CFB           ; ACK(R1=0) → 继续
; 错误出口:
0x7D14: LCALL 0x18BA         ; START
0x7D17: MOV R1, #00          ; R1=0 = 失败
0x7D19: SJMP 0x7D20
; 正常结束:
0x7D1B: LCALL 0x18BA         ; ★ START (结束, 非标准 STOP!)
0x7D1E: MOV R1, #01          ; R1=1 = 成功
0x7D20: MOV R7, #05
0x7D22: LJMP 0x0FF6          ; 返回
```

帧结构: `STOP → START → 写地址(0xAA) → ACK → 块地址 → ACK → 数据[0..len-1] → ACK → START`
★ 第一数据字节是块地址, 无命令码
★ 结束用 START 而非 STOP
★ ACK 判定: R1=0(ACK) 成功, R1≠0(NACK) 失败

### 5.2 read_cmd (0x7C10, stub 0x18C6)

读命令: 读 N 字节 (块地址由外部先调 write_cmd 设入)

```
入口: R1=读长度, R2:R3=XRAM缓冲指针
0x7C10: MOV A, #F4           ; 函数标记
0x7C12: LCALL 0x0E24
0x7C15: MOV R6, R2           ; R6:R7 = XRAM 指针
0x7C17: MOV R7, R3
0x7C19: MOV dir0B, R1        ; dir0B = 读长度
0x7C1B: MOV dir0A, #00       ; dir0A = 字节计数器

0x7C1E: MOV DPTR, #1B61      ; ★ 读从地址
0x7C21: MOVX A, @DPTR
0x7C22: MOV dir08, A
0x7C24: LCALL 0x18C0         ; STOP (保险)
0x7C27: MOV A, dir08
0x7C29: CLR C
0x7C2A: RLC A                ; slv << 1
0x7C2B: ORL A, #01           ; | 1 = 读地址 (0xAB)
0x7C2D: MOV R1, A
0x7C2E: LCALL 0x18B4         ; write_byte(读地址)
0x7C31: LCALL 0x18AE         ; read_sda (ACK)
0x7C34: MOV A, R1
0x7C35: JZ 0x7CAE            ; ★ ACK(R1=0) → 进入读循环 (跳到 0x7CAE 判断)
                              ;   dir0A=0 < dir0B=长度 → JC 0x7C3E 读循环
0x7C37: LCALL 0x18BA         ; NACK → START (结束)
0x7C3A: MOV R1, #00          ; R1=0 = 失败
0x7C3C: SJMP 0x7CBA          ; 返回

; ★ 读循环体 (每字节):
0x7C3E: MOV dir08, #00       ; bit 计数器清零
0x7C41: MOV dir09, #00       ; 数据寄存器清零
0x7C44: LCALL 0x7CBD         ; ★ sda_input (ANL P0DIR,#FE)
0x7C47: LCALL 0x1590         ; delay
; 8 位读循环:
0x7C4A: SETB P0.1            ; SCL hi
0x7C4C: MOV R2, #3C          ; delay(3C)
0x7C50: LCALL 0x1590
0x7C53: MOV C, P0.0          ; ★ 读 SDA (SCL 高时)
0x7C55: JNC 0x7C5D
0x7C57: MOV A, dir09
0x7C59: SETB ACC.0           ; bit=1
0x7C5B: MOV dir09, A
0x7C5D: MOV A, #07
0x7C5F: XRL A, dir08         ; 检查是否第8位
0x7C61: JZ 0x7C69            ; 是最后一位 → 跳过移位
0x7C63: MOV A, dir09
0x7C65: CLR C
0x7C66: RLC A                ; 左移
0x7C67: MOV dir09, A
0x7C69: MOV R2, #3C          ; delay(3C)
0x7C6D: LCALL 0x1590
0x7C70: CLR P0.1             ; SCL lo
0x7C72: MOV R2, #78          ; delay(78)
0x7C76: LCALL 0x1590
0x7C79: INC dir08
0x7C7B: MOV A, dir08
0x7C7E: CLR C
0x7C7F: SUBB A, #08
0x7C80: JC 0x7C4A            ; 8 位未读完 → 继续
; 8 位读完, 存字节:
0x7C82: LCALL 0x7E4A         ; 计算 DPTR = R6:R7 + dir0A
0x7C85: MOV A, dir09
0x7C87: MOVX @DPTR, A        ; 存到 XRAM

; ★ inter-byte ACK 脉冲 (每字节后都做, 包括最后一字节):
0x7C88: ORL P0DIR, #03       ; ★ 切回推挽!
0x7C8B: CLR P0.1             ; SCL lo
0x7C8D: CLR P0.0             ; SDA lo (ACK = 拉低)
0x7C8F: MOV R2, #3C          ; delay(3C)
0x7C93: LCALL 0x1590
0x7C96: SETB P0.1            ; SCL hi
0x7C98: CLR P0.0             ; SDA lo (保持)
0x7C9A: MOV R2, #78          ; delay(78)
0x7C9E: LCALL 0x1590
0x7CA1: CLR P0.1             ; SCL lo
0x7CA3: CLR P0.0             ; SDA lo
0x7CA5: MOV R2, #3C          ; delay(3C)
0x7CA9: LCALL 0x1590
0x7CAC: INC dir0A            ; 字节计数器++

; 循环判断:
0x7CAE: MOV A, dir0A
0x7CB0: CLR C
0x7CB1: SUBB A, dir0B        ; dir0A < dir0B(长度)?
0x7CB3: JC 0x7C3E            ; 未读完 → 继续
0x7CB5: LCALL 0x18BA         ; ★ START (结束, 非标准!)
0x7CB8: MOV R1, #01          ; R1=1 = 成功
0x7CBA: LJMP 0x7F22          ; 返回 (R7=4)
```

帧结构: `STOP → START → 读地址(0xAB) → ACK → [sda_input → 8位读 → 推挽ACK脉冲]×N → START`
★ 不发块地址! 块地址由外部先调 write_cmd 设入芯片内部地址指针
★ 每字节后发推挽 ACK 脉冲 (ORL P0DIR,#03 + SDA lo + SCL 脉冲)
★ **每字节都发 ACK, 无 NACK!** (包括最后一字节)
★ 结束用 START, 不用 STOP

### 5.3 batch_read (0x7DAD, stub 0x18E4)

两段式读 = write_cmd(设块地址) + read_cmd(读 16 字节):

```
入口: R1=块基址, R2=偏移, R4:R5=XRAM目标指针, R3=总长度
每次循环 16 字节:
  1. write_cmd(R1=块地址+dir0A, len=0)  ← 设块地址
  2. read_cmd(R1=16, XRAM缓冲)         ← 读 16 字节
  3. dir0A += 16, 循环
```

这是标准 I2C EEPROM "随机读" 模式: 先写地址指针, 再读数据。
16 字节 = Type 3 Tag 块大小, 强烈指向 MN63Y1213 原生能力。

---

## 六、协议总结

### write_cmd (写块地址 + 数据):
```
STOP(保险) → START → 写地址(0xAA) → ACK → 块地址 → ACK → 数据[0..len-1] → ACK → START(结束)
```
- 第一数据字节是块地址, 无命令码
- 结束用 START 而非 STOP

### read_cmd (读 N 字节):
```
STOP(保险) → START → 读地址(0xAB) → ACK → [sda_input → 8位读 → 推挽ACK脉冲]×N → START(结束)
```
- 不发块地址 (由 write_cmd 先设入)
- 每字节都发推挽 ACK 脉冲, 无 NACK
- 结束用 START

### batch_read = 两段式:
```
1. write_cmd(块地址, len=0)   ← 设芯片内部地址指针
2. read_cmd(buf, 16)          ← 读 16 字节
```

---

## 七、nfc.c 复刻状态 (v13)

### 已严格复刻的原厂原语:

| nfc.c 函数 | 原厂地址 | 复刻状态 |
|------------|----------|----------|
| `i2c_start` | 0x7BCC | ✅ 开头 `P0DIR\|=0x03` + 原厂序列 |
| `i2c_stop` | 0x7BEE | ✅ 开头 `P0DIR\|=0x01` + 原厂序列 |
| `i2c_tx8` | 0x7B8F | ✅ 开头 `P0DIR\|=0x03` + 8 位 MSB 序列 |
| `i2c_rx8` | 0x7C3E | ✅ sda_input 前置 + SCL 高采样 + 末尾不切推挽 |
| `i2c_read_ack` | 0x7B5F | ✅ sda_input + SCL 高采样 + 末尾不切推挽 |
| `i2c_rx_ack_pulse` | 0x7C88 | ✅ 推挽 ACK 脉冲 (每字节都 ACK, 无 NACK) |
| `i2c_write_cmd` | 0x7CC7 | ✅ STOP→START→写地址→块地址→数据→START |
| `i2c_read_cmd` | 0x7C10 | ✅ STOP→START→读地址→读N字节→START |

### 关键修正历程:

- **v11**: 初版按原厂重写, 但 i2c_rx8 缺 sda_input, i2c_read_ack 末尾多余 sda_output
- **v12**: 修正 i2c_rx8/i2c_read_ack/i2c_tx8, 用 i2c_rx_ack_pulse 替代 i2c_putack
  - 板端结果: probe 0x54=REAL, 但 write_cmd NACK
  - 根因: i2c_start/i2c_stop 缺开头 P0DIR 设置
- **v13**: i2c_start 开头加 `P0DIR|=0x03`, i2c_stop 开头加 `P0DIR|=0x01`
  - 待板端验证

---

## 八、待确认问题

1. **从地址 0x54 vs 0x55**: 原厂固件写 0x55, 但板端 probe 显示 0x54=REAL
   (用读地址 0xA9 能 ACK)。可能是原厂非标准 START 与标准 probe 产生的差异,
   修正位层后需重新观察。

2. **MN63Y1213 直接块寻址模式**: 原厂用块地址作为第一个数据字节, 不走手册标准
   命令协议。batch_read 以 16 字节为单位 = Type 3 Tag 块大小, 强烈指向这就是
   MN63Y1213 的原生能力。

3. **原厂延时 @16MHz**: 原厂 @32MHz 用 0x3C/0x78, 本工程 @16MHz 当前用减半值。
   如时序不稳可能需调大。
