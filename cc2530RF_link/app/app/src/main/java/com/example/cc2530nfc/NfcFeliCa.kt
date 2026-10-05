package com.example.cc2530nfc

/**
 * MN63Y1213 Tunnel 模式命令构造（JISX6319-4 / FeliCa 路径）
 *
 * 手册依据：
 *  - §7.2.1：JISX6319-4 下 Tunnel 也用标准 READ(0x06)/WRITE(0x08) 命令
 *  - §4.2.5.6 + Table 4-6：3 字节块元素格式，D2 的 bit2-0 设模式
 *      000 = RF 通信模式 + 明文
 *      110 = Tunnel 模式 + 明文
 *      111 = Tunnel 模式 + 加密（私钥）
 *      101 = Tunnel 模式 + 加密（族钥）
 *  - §4.2.5.7：Tunnel 模式下块号必须递增
 *  - §4.2.6.2 READ / §4.2.6.3 WRITE：命令帧结构
 *
 * FeliCa 帧（NfcF.transceive 自动加 LEN 和 CRC）：
 *   REQ:   [0x00][SC hi][SC lo][req][timeslot]
 *   READ:  [0x06][IDm x8][k][service x2k][m][blocklist]
 *   WRITE: [0x08][IDm x8][k][service x2k][m][blocklist][blockdata]
 *
 * 块元素 3 字节格式（D0 D1 D2）：
 *   D0 = 0x80（bit7=1 固定，bit6-4=access mode=000，bit3-0=服务码顺序 don't care）
 *   D1 = 块号
 *   D2 = 0x00 | mode（bit2-0）
 *       Tunnel 明文 mode = 110b = 0x06
 */
object NfcFeliCa {

    const val CMD_REQ   = 0x00
    const val CMD_READ  = 0x06
    const val CMD_WRITE = 0x08

    /** 隧道明文模式：D2 bit2-0 = 110b */
    const val BLOCK_MODE_TUNNEL_PLAIN = 0x06

    /** 服务码：MN63Y1213 不使用 service 概念，给示例值 0x0900 */
    private const val SERVICE_CODE_HI = 0x09
    private const val SERVICE_CODE_LO = 0x00

    /** FeliCa 单块 = 16 字节 */
    const val BLOCK_SIZE = 16

    /** REQ 命令：SC=0xFFFF（识别所有） */
    fun buildReq(): ByteArray = byteArrayOf(
        CMD_REQ.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0x00, 0x00
    )

    /**
     * 构造 Tunnel WRITE（FeliCa）
     * @param idm    8 字节 PICC 识别符（REQ 响应里拿）
     * @param blocks 块号数组（Tunnel 模式必须递增）
     * @param data   每个块 16 字节，总长 = blocks.size * 16
     */
    fun buildTunnelWrite(idm: ByteArray, blocks: IntArray, data: ByteArray): ByteArray {
        require(idm.size == 8) { "IDm 必须 8 字节" }
        require(blocks.isNotEmpty()) { "至少 1 个块" }
        require(data.size == blocks.size * BLOCK_SIZE) {
            "数据长度 ${data.size} != 块数 ${blocks.size} * 16"
        }
        // [08][IDm x8][k=1][service x2][m][blocklist 3*m][data 16*m]
        val out = ArrayList<Byte>(8 + 1 + 2 + 1 + blocks.size * 3 + data.size)
        out.add(CMD_WRITE.toByte())
        idm.forEach { out.add(it) }
        out.add(0x01)                          // k=1 个服务
        out.add(SERVICE_CODE_HI.toByte())
        out.add(SERVICE_CODE_LO.toByte())
        out.add(blocks.size.toByte())          // m 个块
        for (b in blocks) {
            out.add(0x80.toByte())             // D0: bit7=1, access mode=000
            out.add((b and 0xFF).toByte())     // D1: 块号
            out.add(BLOCK_MODE_TUNNEL_PLAIN.toByte()) // D2: mode = Tunnel 明文
        }
        data.forEach { out.add(it) }
        return out.toByteArray()
    }

    /**
     * 构造 Tunnel READ（FeliCa）
     */
    fun buildTunnelRead(idm: ByteArray, blocks: IntArray): ByteArray {
        require(idm.size == 8)
        require(blocks.isNotEmpty())
        val out = ArrayList<Byte>(8 + 1 + 2 + 1 + blocks.size * 3)
        out.add(CMD_READ.toByte())
        idm.forEach { out.add(it) }
        out.add(0x01)
        out.add(SERVICE_CODE_HI.toByte())
        out.add(SERVICE_CODE_LO.toByte())
        out.add(blocks.size.toByte())
        for (b in blocks) {
            out.add(0x80.toByte())
            out.add((b and 0xFF).toByte())
            out.add(BLOCK_MODE_TUNNEL_PLAIN.toByte())
        }
        return out.toByteArray()
    }

    /** 解析 REQ 响应，取 IDm（响应格式：[01][IDm x8][PMm x8][reqdata]） */
    fun parseIdm(resp: ByteArray): ByteArray? {
        if (resp.size < 10) return null
        if (resp[0].toInt() and 0xFF != 0x01) return null
        return resp.copyOfRange(1, 9)
    }

    /** 解析 READ 响应：[07][IDm x8][SF1][SF2][m][blockdata] */
    fun parseReadResponse(resp: ByteArray): Pair<Int, ByteArray>? {
        if (resp.size < 13) return null
        if (resp[0].toInt() and 0xFF != 0x07) return null
        val sf1 = resp[9].toInt() and 0xFF
        val sf2 = resp[10].toInt() and 0xFF
        val status = (sf1 shl 8) or sf2
        val m = resp[11].toInt() and 0xFF
        val data = resp.copyOfRange(12, resp.size)
        return status to data
    }

    /** 解析 WRITE 响应：[09][IDm x8][SF1][SF2] */
    fun parseWriteResponse(resp: ByteArray): Int? {
        if (resp.size < 11) return null
        if (resp[0].toInt() and 0xFF != 0x09) return null
        val sf1 = resp[9].toInt() and 0xFF
        val sf2 = resp[10].toInt() and 0xFF
        return (sf1 shl 8) or sf2
    }

    /** 状态字说明（手册 Table 4-8） */
    fun describeStatus(status: Int): String = when (status) {
        0x0000 -> "正常结束"
        0xFF50 -> "Tunnel 模式错误：host(CC2530) 无响应"
        0xFF51 -> "Tunnel 模式错误：host 回错误"
        0xFFA1 -> "服务数规格错误"
        0xFFA2 -> "块数规格错误"
        0xFFA3 -> "服务设置不一致"
        0xFFA5 -> "块规格错误（模式位/块号/递增顺序）"
        0xFF60 -> "自诊断错误"
        else -> "未知状态字"
    }
}
