package com.example.cc2530nfc

/**
 * MN63Y1213 Tunnel 模式命令构造（ISO/IEC14443 TypeB，明文）
 *
 * 手册依据：
 *  - §4.3.8.5 Address：P1 的 bit6-4 设模式。bit6-4=110 → Tunnel 模式 + 明文
 *  - §7.4.1 / §7.4.2：Tunnel Read/Write 用标准 0xB0 / 0xD6 命令码
 *  - §4.3.9.6 WRITE：Lc 最大 248（0xE8）；§4.3.9.5 READ：Le 最大 251（0xFB）
 *
 * APDU 结构（装在 ISO14443-4 I-block 的 INF 里，NfcB.transceive 自动加 PCB/CRC）：
 *   Command: [CLA=00][INS][P1][P2][Lc][Data]
 *   Response:[Data...][SW1][SW2]
 */
object NfcTunnel {

    // INS
    const val INS_READ  = 0xB0
    const val INS_WRITE = 0xD6
    const val INS_SELECT = 0xA4

    // CLA
    const val CLA = 0x00

    /**
     * Tunnel 模式 P1 高字节：bit6-4 = 110（明文 Tunnel），bit7 = 0
     * 手册 Table 4-21：bit6 bit5 bit4 = 1 1 0 → Tunnel mode, plaintext
     */
    private const val P1_MODE_TUNNEL_PLAIN = 0x60

    /** 单次 Tunnel 写最大数据长度（手册 §7.4.2：Lc = 0x01..0xE8 = 248） */
    const val MAX_WRITE_LEN = 248

    /** 单次 Tunnel 读最大长度（手册 §4.3.9.5：Le = 0x01..0xFB = 251） */
    const val MAX_READ_LEN = 251

    /**
     * 构造 Tunnel 写命令。
     * @param addr 起始地址（16-bit，明文通信地址单位为 1 字节，见 §4.3.8.5 Table 4-22）
     * @param data 数据（≤248 字节）
     */
    fun buildTunnelWrite(addr: Int, data: ByteArray): ByteArray {
        require(data.size in 1..MAX_WRITE_LEN) { "数据长度必须在 1..$MAX_WRITE_LEN" }
        require(addr in 0..0xFFFF) { "地址必须在 0..0xFFFF" }
        val p1 = (P1_MODE_TUNNEL_PLAIN or ((addr shr 8) and 0x3F)).toByte()
        val p2 = (addr and 0xFF).toByte()
        val out = ByteArray(5 + data.size)
        out[0] = CLA.toByte()
        out[1] = INS_WRITE.toByte()
        out[2] = p1
        out[3] = p2
        out[4] = data.size.toByte()
        System.arraycopy(data, 0, out, 5, data.size)
        return out
    }

    /**
     * 构造 Tunnel 读命令。
     * @param addr 起始地址
     * @param len  读取长度（≤251）
     */
    fun buildTunnelRead(addr: Int, len: Int): ByteArray {
        require(len in 1..MAX_READ_LEN) { "读取长度必须在 1..$MAX_READ_LEN" }
        require(addr in 0..0xFFFF) { "地址必须在 0..0xFFFF" }
        val p1 = (P1_MODE_TUNNEL_PLAIN or ((addr shr 8) and 0x3F)).toByte()
        val p2 = (addr and 0xFF).toByte()
        return byteArrayOf(
            CLA.toByte(), INS_READ.toByte(), p1, p2, len.toByte()
        )
    }

    /** SELECT NDEF 应用（可选，用于验证 ISO14443-4 协议态） */
    fun buildSelectNdefApp(): ByteArray = byteArrayOf(
        0x00, 0xA4.toByte(), 0x04, 0x00, 0x07,
        0xD2.toByte(), 0x76, 0x00, 0x00, 0x85.toByte(), 0x01, 0x01,
        0x00
    )

    /** 解析响应：返回 (data, sw1, sw2)，响应至少 2 字节 */
    data class ApduResponse(val data: ByteArray, val sw1: Int, val sw2: Int) {
        val sw: Int get() = (sw1 shl 8) or sw2
        val isOk: Boolean get() = sw == 0x9000
        fun swHex(): String = String.format("%02X%02X", sw1, sw2)
    }

    fun parseResponse(resp: ByteArray): ApduResponse {
        require(resp.size >= 2) { "响应长度不足 2 字节: ${resp.size}" }
        val dataLen = resp.size - 2
        val data = ByteArray(dataLen)
        System.arraycopy(resp, 0, data, 0, dataLen)
        return ApduResponse(data, resp[resp.size - 2].toInt() and 0xFF,
            resp[resp.size - 1].toInt() and 0xFF)
    }

    /** 把状态字翻译成可读说明（手册 §4.3.8.7 / §7.4.1 §7.4.2） */
    fun describeSw(sw: Int): String = when (sw) {
        0x9000 -> "正常结束"
        0x5000 -> "Tunnel 模式错误：host(CC2530) 无响应"
        0x5100 -> "Tunnel 模式错误：host(CC2530) 回错误"
        0x6700 -> "Lc/Le 规格错误"
        0x6A86 -> "P1-P2 规格错误（模式位或地址非法）"
        0x6D00 -> "INS 规格错误（命令码不支持）"
        0x6E00 -> "CLA 规格错误"
        0x6F00 -> "自诊断错误"
        else -> "未知状态字"
    }
}
