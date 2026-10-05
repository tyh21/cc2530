# CC2530 / MN63Y1213 NFC Tunnel 传图

## 当前阶段：最小验证版（App 侧）

目标：验证 **手机 → MN63Y1213 RF 侧 → Tunnel 命令格式** 是否正确。
CC2530 固件的 Tunnel 从机响应尚未实现，因此：

- **Tunnel WRITE 预期返回 `50 00`**（host 无响应）—— 这恰好证明 RF 侧通了、命令格式对了。
- 若返回 `6A 86` → 模式位/P1 设错
- 若返回 `6D 00` → INS 错
- 若 `transceive` 抛 "not ISO14443-4" → 手机 NFC 栈不支持 APDU

## 目录结构

```
app/
├── settings.gradle.kts
├── build.gradle.kts
├── gradle.properties
├── img2tunnel.py                    # PC 端图片预处理
├── README.md
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        └── java/com/example/cc2530nfc/
            ├── MainActivity.kt      # UI + NFC 调度
            └── NfcTunnel.kt         # Tunnel APDU 构造/解析
```

## 使用方法

### 1. 编译安装 App
用 Android Studio 打开 `cc2530_RF_link/app`，Sync → Run。

### 2. 验证流程
1. 打开 App，确认日志显示"NFC 已启用"
2. 手机贴到价签板（NFC 天线位置）
3. 点"2. 显示当前标签信息" → 应显示 `NfcB` 可用
4. 点"3. 发 SELECT NDEF 应用" → 预期 `9000`（证明能进 ISO14443-4 协议态）
5. 点"4. Tunnel 写测试" → 预期 `5000`（证明 Tunnel 命令格式正确，只是 CC2530 还没实现从机）

### 3. PC 端图片预处理
```bash
cd cc2530_RF_link/app
pip install Pillow
python img2tunnel.py 你的图.png --w 212 --h 104
```
生成：
- `你的图.bin` — 2756 字节位流
- `你的图_chunks.txt` — 分块 Tunnel WRITE 数据

## Tunnel APDU 格式（手册依据）

### 写
```
[00] [D6] [P1] [P2] [Lc] [Data...]
 CLA  INS  ↑     ↑   1..248
           |     +-- 地址低字节
           +-------- 0x60 | 地址高6位
                    0x60 = bit6-4=110 → Tunnel 模式 + 明文（手册 Table 4-21）
```

### 读
```
[00] [B0] [P1] [P2] [Le]
                 1..251
```

### 状态字（手册 §4.3.8.7）
| SW | 含义 |
|---|---|
| 9000 | 正常 |
| 5000 | Tunnel 错误：host 无响应 |
| 5100 | Tunnel 错误：host 回错误 |
| 6A86 | P1-P2 错误（模式位/地址） |
| 6D00 | INS 错误 |
| 6E00 | CLA 错误 |

## 下一步（阶段 B）
1. 实现 CC2530 固件的 I²C 从机 + QUERY/ANSWER（Tunnel 从机侧）
2. App 增加：读 `_chunks.txt` → 连续发分块 → 显示进度
