package com.example.cc2530nfc

import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.NfcB
import android.nfc.tech.NfcF
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * MN63Y1213 Tunnel 验证版 v3
 *
 * v3 改动：
 *   - 去掉 EXTRA_READER_PRESENCE_CHECK_DELAY（该参数会周期性中断 transceive，
 *     造成 onTagDiscovered 后 ~270ms 固定 TagLostException）
 *   - 贴卡时在 onTagDiscovered 内立即执行，无任何 delay
 *   - 新增“贴卡自动发 REQ”开关，减少中间环节
 */
class MainActivity : AppCompatActivity(), NfcAdapter.ReaderCallback {

    private var nfcAdapter: NfcAdapter? = null

    private lateinit var logView: TextView
    private lateinit var addrInput: EditText
    private lateinit var lenInput: EditText
    private lateinit var hintView: TextView

    private val logBuf = StringBuilder()
    private val ts = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    @Volatile private var pendingAction = 0   // 0=无 1=REQ 2=Tunnel写 3=Tunnel读 4=SELECT 5=信息
    @Volatile private var autoReq = true      // 贴卡自动发 REQ

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = ScrollView(this)
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        root.addView(container)

        container.addView(TextView(this).apply {
            text = "CC2530 / MN63Y1213 Tunnel v3"
            textSize = 18f
        })

        hintView = TextView(this).apply {
            text = "就绪：贴卡自动发 REQ"
            textSize = 15f
            setPadding(0, 8, 0, 8)
        }
        container.addView(hintView)

        fun addBtn(label: String, action: () -> Unit) {
            container.addView(Button(this).apply {
                text = label
                setOnClickListener { action() }
            })
        }

        addBtn("切换 自动REQ / 手动") {
            autoReq = !autoReq
            hintView.text = if (autoReq) "就绪：贴卡自动发 REQ" else "就绪：点按钮后贴卡"
        }
        addBtn("清空日志") { logBuf.setLength(0); logView.text = "" }

        container.addView(TextView(this).apply {
            text = "--- FeliCa (NfcF) ---"
            textSize = 15f
            setPadding(0, 24, 0, 0)
        })
        addBtn("F1. 发 REQ（拿 IDm）") { arm(1, "发 REQ") }
        addBtn("F2. Tunnel 写 1 块") { arm(2, "Tunnel 写") }
        addBtn("F3. Tunnel 读 1 块") { arm(3, "Tunnel 读") }

        container.addView(TextView(this).apply {
            text = "--- TypeB (NfcB) ---"
            textSize = 15f
            setPadding(0, 24, 0, 0)
        })
        addBtn("B1. 发 SELECT NDEF 应用") { arm(4, "SELECT") }

        val paramRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        addrInput = EditText(this).apply { hint = "块号(hex)"; setText("0000") }
        lenInput = EditText(this).apply { hint = "长度(dec)"; setText("16") }
        paramRow.addView(addrInput, LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        paramRow.addView(lenInput, LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        container.addView(paramRow)

        logView = TextView(this).apply {
            textSize = 12f
            setPadding(0, 24, 0, 0)
        }
        container.addView(logView)

        setContentView(root)

        nfcAdapter = NfcAdapter.getDefaultAdapter(this)
        log("App 启动。NFC 适配器: " + (if (nfcAdapter != null) "有" else "无"))
        if (nfcAdapter == null) log("!! 本机无 NFC 硬件")
    }

    override fun onResume() {
        super.onResume()
        val adapter = nfcAdapter ?: return
        val flags = NfcAdapter.FLAG_READER_NFC_F or
                NfcAdapter.FLAG_READER_NFC_B or
                NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK or
                NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS
        // v3: 不传 EXTRA_READER_PRESENCE_CHECK_DELAY，避免周期性中断 transceive
        adapter.enableReaderMode(this, this, flags, null)
        log("Reader Mode 已开启（无 presence check 参数）")
    }

    override fun onPause() {
        super.onPause()
        nfcAdapter?.disableReaderMode(this)
    }

    private fun arm(action: Int, name: String) {
        pendingAction = action
        hintView.text = "待执行: " + name + " —— 请贴卡"
        log("待执行: " + name + "，请贴卡...")
    }

    override fun onTagDiscovered(tag: Tag) {
        val action = if (pendingAction != 0) pendingAction else if (autoReq) 1 else 0
        log("-------- onTagDiscovered (action=" + action + ") --------")
        if (action == 0) return
        when (action) {
            1 -> doFelicaReq(tag)
            2 -> doFelicaTunnelWrite(tag)
            3 -> doFelicaTunnelRead(tag)
            4 -> doTypebSelect(tag)
            5 -> doShowTagInfo(tag)
        }
        pendingAction = 0
        runOnUiThread { hintView.text = if (autoReq) "就绪：贴卡自动发 REQ" else "就绪：点按钮后贴卡" }
    }

    private fun doShowTagInfo(tag: Tag) {
        log("Tag ID: " + tag.id.joinToString("") { "%02X".format(it) })
        log("Tech: " + tag.techList.joinToString())
    }

    private fun doFelicaReq(tag: Tag) {
        val nfcF = NfcF.get(tag) ?: run { log("!! 无 NfcF"); return }
        try {
            nfcF.connect()
            val cmd = NfcFeliCa.buildReq()
            log("TX REQ: " + cmd.toHex())
            val resp = nfcF.transceive(cmd)
            log("RX REQ (" + resp.size + "B): " + resp.toHex())
            val idm = NfcFeliCa.parseIdm(resp)
            if (idm != null) log("  => IDm = " + idm.toHex()) else log("  => 解析 IDm 失败")
        } catch (e: Exception) {
            log("!! REQ 异常: " + e.javaClass.simpleName + ": " + e.message)
        } finally {
            try { nfcF.close() } catch (_: Exception) {}
        }
    }

    private fun doFelicaTunnelWrite(tag: Tag) {
        val nfcF = NfcF.get(tag) ?: run { log("!! 无 NfcF"); return }
        try {
            nfcF.connect()
            val reqResp = nfcF.transceive(NfcFeliCa.buildReq())
            val idm = NfcFeliCa.parseIdm(reqResp)
            if (idm == null) { log("!! 未拿到 IDm"); return }
            log("IDm = " + idm.toHex())
            val blockNum = parseHex(addrInput.text.toString()) ?: 0
            val data = ByteArray(16) { 0xA5.toByte() }
            val cmd = NfcFeliCa.buildTunnelWrite(idm, intArrayOf(blockNum), data)
            log("TX TunnelWRITE: " + cmd.toHex())
            val resp = nfcF.transceive(cmd)
            log("RX TunnelWRITE (" + resp.size + "B): " + resp.toHex())
            val status = NfcFeliCa.parseWriteResponse(resp)
            if (status != null) log("  => status=" + String.format("0x%04X", status) + " " + NfcFeliCa.describeStatus(status))
        } catch (e: Exception) {
            log("!! TunnelWRITE 异常: " + e.javaClass.simpleName + ": " + e.message)
        } finally {
            try { nfcF.close() } catch (_: Exception) {}
        }
    }

    private fun doFelicaTunnelRead(tag: Tag) {
        val nfcF = NfcF.get(tag) ?: run { log("!! 无 NfcF"); return }
        try {
            nfcF.connect()
            val reqResp = nfcF.transceive(NfcFeliCa.buildReq())
            val idm = NfcFeliCa.parseIdm(reqResp)
            if (idm == null) { log("!! 未拿到 IDm"); return }
            log("IDm = " + idm.toHex())
            val blockNum = parseHex(addrInput.text.toString()) ?: 0
            val cmd = NfcFeliCa.buildTunnelRead(idm, intArrayOf(blockNum))
            log("TX TunnelREAD: " + cmd.toHex())
            val resp = nfcF.transceive(cmd)
            log("RX TunnelREAD (" + resp.size + "B): " + resp.toHex())
            val parsed = NfcFeliCa.parseReadResponse(resp)
            if (parsed != null) {
                log("  => status=" + String.format("0x%04X", parsed.first) + " " + NfcFeliCa.describeStatus(parsed.first))
                if (parsed.second.isNotEmpty()) log("  => data: " + parsed.second.toHex())
            }
        } catch (e: Exception) {
            log("!! TunnelREAD 异常: " + e.javaClass.simpleName + ": " + e.message)
        } finally {
            try { nfcF.close() } catch (_: Exception) {}
        }
    }

    private fun doTypebSelect(tag: Tag) {
        val nfcB = NfcB.get(tag) ?: run { log("!! 无 NfcB"); return }
        try {
            nfcB.connect()
            val cmd = NfcTunnel.buildSelectNdefApp()
            log("TX SELECT: " + cmd.toHex())
            val resp = nfcB.transceive(cmd)
            log("RX (" + resp.size + "B): " + resp.toHex())
        } catch (e: Exception) {
            log("!! SELECT 异常: " + e.javaClass.simpleName + ": " + e.message)
        } finally {
            try { nfcB.close() } catch (_: Exception) {}
        }
    }

    private fun parseHex(s: String): Int? {
        return try { Integer.parseInt(s.trim().removePrefix("0x").removePrefix("0X"), 16) }
        catch (e: Exception) { log("!! 解析失败: " + s); null }
    }

    private fun log(msg: String) {
        val line = ts.format(Date()) + "  " + msg
        logBuf.append(line).append('\n')
        runOnUiThread { logView.text = logBuf.toString() }
        android.util.Log.d("CC2530NFC", msg)
    }

    private fun ByteArray.toHex(): String = joinToString(" ") { "%02X".format(it) }
}
