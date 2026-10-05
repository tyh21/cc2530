package com.tyh.nfcdump;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.nfc.NfcAdapter;
import android.nfc.Tag;
import android.nfc.TagLostException;
import android.nfc.tech.NfcF;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;

/**
 * NFCF Dumper - MN63Y1213/KM63Y1213 (FeliCa Type 3 Tag) FeRAM block dumper.
 *
 * Reads all 32 blocks (user area 0..26 + system area 27..31) via raw
 * JIS X6319-4 READ (0x06) commands and decodes the system area:
 *   block 30 (0x01E0): SC, IDM, PMM, AFI, FWI, HW1 (I2C slave addr)
 *   block 31 (0x01F0): RORF, ROSI, SECURITY, TNPRM, HW2, CONFIG tail
 *   blocks 27..29 (0x01B0..0x01DF): CONFIG body (NDA doc, raw hex shown)
 *
 * Service code auto-fallback: 0x000B -> 0x0009 -> 0x0010 -> 0x0001.
 */
public class MainActivity extends Activity implements NfcAdapter.ReaderCallback {

    private static final int[] SERVICE_CANDIDATES = {0x000B, 0x0009, 0x0010, 0x0001};
    // read order: verify comms with block 0, then system area, then rest
    private static final int[] READ_ORDER = {
        0, 30, 31, 27, 28, 29,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26
    };

    private NfcAdapter mAdapter;
    private TextView mLog;
    private ScrollView mScroll;
    private TextView mServiceLabel;
    private int mServiceIdx = 0;
    private boolean mAutoFallback = true;

    private final StringBuilder mText = new StringBuilder();
    private final byte[][] mBlocks = new byte[32][];
    // 0 = ok, -1 = tag lost / io, other = (st1<<8)|st2
    private final int[] mStatus = new int[32];

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(0x10, 0x10, 0x12));

        TextView title = new TextView(this);
        title.setText("NFCF Dumper  —  MN63Y1213 系统区取证");
        title.setTextColor(Color.rgb(0x00, 0xE0, 0x00));
        title.setTypeface(Typeface.MONOSPACE);
        title.setTextSize(15);
        title.setPadding(24, 40, 24, 8);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("打开本应用后，将价签贴到手机背面 NFC 感应区。\n自动读取全部 32 个 FeRAM 块（含系统区）。\n服务码自动降级: 0x000B→0x0009→0x0010→0x0001");
        hint.setTextColor(Color.rgb(0x90, 0x90, 0x90));
        hint.setTypeface(Typeface.MONOSPACE);
        hint.setTextSize(12);
        hint.setPadding(24, 0, 24, 16);
        root.addView(hint);

        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setPadding(16, 0, 16, 8);

        Button btnMode = new Button(this);
        btnMode.setText("服务码:自动");
        btnMode.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                mAutoFallback = !mAutoFallback;
                if (mAutoFallback) {
                    mServiceIdx = 0;
                    ((Button) v).setText("服务码:自动");
                } else {
                    mServiceIdx = (mServiceIdx + 1) % SERVICE_CANDIDATES.length;
                    if (mServiceIdx == 0) mServiceIdx = 1;
                    ((Button) v).setText(String.format("服务码:0x%04X", SERVICE_CANDIDATES[mServiceIdx]));
                }
                log(String.format("服务码模式 => %s%n",
                    mAutoFallback ? "自动(0B→09→10→01)" : String.format("固定 0x%04X", SERVICE_CANDIDATES[mServiceIdx])));
            }
        });
        btnRow.addView(btnMode);

        Button btnCopy = new Button(this);
        btnCopy.setText("复制全部");
        btnCopy.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                cm.setPrimaryClip(ClipData.newPlainText("nfcdump", mText.toString()));
                toast("已复制 " + mText.length() + " 字符");
            }
        });
        btnRow.addView(btnCopy);

        Button btnClear = new Button(this);
        btnClear.setText("清空");
        btnClear.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                mText.setLength(0);
                mLog.setText("");
            }
        });
        btnRow.addView(btnClear);
        root.addView(btnRow);

        mScroll = new ScrollView(this);
        mLog = new TextView(this);
        mLog.setTypeface(Typeface.MONOSPACE);
        mLog.setTextSize(12);
        mLog.setTextColor(Color.rgb(0x00, 0xE0, 0x00));
        mLog.setPadding(24, 8, 24, 32);
        mScroll.addView(mLog);
        root.addView(mScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));

        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        btnMode.setLayoutParams(bp);
        btnCopy.setLayoutParams(bp);
        btnClear.setLayoutParams(bp);

        setContentView(root);
        log("等待标签... 请贴上价签。\n");
    }

    @Override
    protected void onResume() {
        super.onResume();
        mAdapter = NfcAdapter.getDefaultAdapter(this);
        if (mAdapter == null) {
            toast("本机无 NFC");
            finish();
            return;
        }
        mAdapter.enableReaderMode(this, this, NfcAdapter.FLAG_READER_NFC_F, null);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (mAdapter != null) mAdapter.disableReaderMode(this);
    }

    private void toast(final String s) {
        runOnUiThread(new Runnable() {
            public void run() {
                android.widget.Toast t = android.widget.Toast.makeText(MainActivity.this, s, android.widget.Toast.LENGTH_SHORT);
                t.setGravity(Gravity.TOP, 0, 60);
                t.show();
            }
        });
    }

    private void log(final String s) {
        mText.append(s);
        runOnUiThread(new Runnable() {
            public void run() {
                mLog.setText(mText.toString());
                mScroll.post(new Runnable() {
                    public void run() { mScroll.fullScroll(View.FOCUS_DOWN); }
                });
            }
        });
    }

    // ---------------- NFC ----------------

    @Override
    public void onTagDiscovered(Tag tag) {
        String[] techs = tag.getTechList();
        boolean hasF = false;
        for (String t : techs) if (t.equals("android.nfc.tech.NfcF")) hasF = true;
        log("\n===== 标签发现 =====\n");
        log("tech: " + join(techs) + "\n");

        if (!hasF) {
            log("非 NFC-F 标签，跳过。\n");
            return;
        }

        int[] servicePlan;
        if (mAutoFallback) {
            servicePlan = SERVICE_CANDIDATES;
        } else {
            servicePlan = new int[]{SERVICE_CANDIDATES[mServiceIdx]};
        }

        for (int sc : servicePlan) {
            log(String.format("== 尝试服务码 0x%04X ==%n", sc));
            NfcF f = NfcF.get(tag);
            try {
                f.connect();
                f.setTimeout(500);
                byte[] idm = tag.getId();
                log("IDm : " + hex(idm) + "\n");

                if (dumpAll(f, idm, sc)) {
                    f.close();
                    decodeAndReport(sc);
                    return;
                }
            } catch (TagLostException e) {
                log("!! 标签离开\n");
            } catch (IOException e) {
                log("!! IO: " + e.getMessage() + "\n");
            } catch (Exception e) {
                log("!! " + e.getClass().getSimpleName() + ": " + e.getMessage() + "\n");
            }
            try { if (f.isConnected()) f.close(); } catch (Exception ignore) {}
            if (mAutoFallback) log("-- 降级到下一服务码 --\n");
        }
        log("\n所有服务码均失败。可尝试固定服务码模式。\n");
    }

    /** Returns true if at least block 0 succeeded. */
    private boolean dumpAll(NfcF f, byte[] idm, int serviceCode) throws IOException {
        for (byte[] bb : mBlocks) bb = null;
        for (int i = 0; i < 32; i++) { mBlocks[i] = null; mStatus[i] = -1; }

        int okCount = 0;
        boolean block0ok = false;
        for (int blk : READ_ORDER) {
            byte[] resp;
            try {
                resp = f.transceive(buildRead(idm, serviceCode, blk));
            } catch (TagLostException e) {
                throw e;
            }
            Thread.yield();
            if (resp == null || resp.length < 13) {
                mStatus[blk] = -2;
                log(String.format("blk%02d: 响应过短(%d字节)%n", blk, resp == null ? 0 : resp.length));
                continue;
            }
            int st = ((resp[10] & 0xFF) << 8) | (resp[11] & 0xFF);
            if (st != 0) {
                mStatus[blk] = st;
                log(String.format("blk%02d: ST=0x%02X%02X %s%n", blk, resp[10] & 0xFF, resp[11] & 0xFF, statusText(st)));
                continue;
            }
            int nb = resp[12] & 0xFF;
            if (nb < 1 || resp.length < 13 + nb * 16) {
                mStatus[blk] = -2;
                log(String.format("blk%02d: 块数/长度异常%n", blk));
                continue;
            }
            byte[] data = new byte[16];
            System.arraycopy(resp, 13, data, 0, 16);
            mBlocks[blk] = data;
            mStatus[blk] = 0;
            okCount++;
            if (blk == 0) block0ok = true;
            log(String.format("blk%02d: %s%n", blk, hex(data)));
            try { Thread.sleep(15); } catch (InterruptedException ignore) {}
        }
        log(String.format("成功 %d/32 块%n", okCount));
        return block0ok || okCount >= 4; // block0 is the comms check, but accept partial
    }

    private byte[] buildRead(byte[] idm, int serviceCode, int block) {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        b.write(0);                       // length placeholder
        b.write(0x06);                    // READ
        b.write(idm, 0, 8);               // IDm
        b.write(1);                       // 1 service
        b.write(serviceCode & 0xFF);      // service code little-endian
        b.write((serviceCode >> 8) & 0xFF);
        b.write(1);                       // 1 block
        b.write(0x80);                    // 2-byte element, service order 0, access 0
        b.write(block);                   // block number
        byte[] out = b.toByteArray();
        out[0] = (byte) out.length;
        return out;
    }

    private String statusText(int st) {
        switch (st) {
            case 0xFFA1: return "服务数错误";
            case 0xFFA2: return "块数错误";
            case 0xFFA3: return "服务设置不一致";
            case 0xFFA5: return "块规格错误(超范围/受保护)";
            case 0xFF60: return "诊断错误(SECURITY禁明文/只读)";
            case 0xFF50: return "tunnel:无主机应答";
            case 0xFF51: return "tunnel:主机应答错误";
            default:     return "";
        }
    }

    // ---------------- system area decode ----------------

    private void decodeAndReport(int usedService) {
        log("\n===== 系统区解析 (服务码 0x" + String.format("%04X", usedService) + ") =====\n");

        byte[] b30 = mBlocks[30];
        if (b30 == null) {
            log("块30 未读到 — 无法解析 HW1/I2C_SLV。\n");
        } else {
            log("[块30 0x01E0..0x01EF]\n");
            log(String.format("  SC   (0x01E0): %02X %02X   系统码=0x%02X%02X (0x12FC=NDEF标准)%n",
                    b30[0], b30[1], b30[0], b30[1]));
            StringBuilder idm2 = new StringBuilder();
            for (int i = 2; i <= 9; i++) idm2.append(String.format("%02X", b30[i]));
            log(String.format("  IDM  (0x01E2): %s%n", idm2));
            log(String.format("  PMM  (0x01EA): %02X %02X%n", b30[10], b30[11]));
            log(String.format("  AFI  (0x01EC): %02X%n  FWI  (0x01ED): %02X%n", b30[12], b30[13]));
            int rftype = (b30[14] >> 4) & 0x03;
            int idmsel = b30[14] & 0x01;
            int i2cslv = b30[15] & 0x7F;
            String rft;
            switch (rftype) {
                case 0: rft = "自动(FeliCa+TypeB)"; break;
                case 1: rft = "仅FeliCa"; break;
                case 2: rft = "仅TypeB"; break;
                default: rft = "保留(用默认)";
            }
            log(String.format("  HW1  (0x01EE): %02X %02X%n" +
                              "        RFTYPE=%d(%s) IDMSEL=%d(%s)%n",
                    b30[14], b30[15], rftype, rft, idmsel,
                    idmsel == 1 ? "IDm用系统区值" : "IDm全0"));
            log(String.format("        I2C_SLV=0x%02X  (7位从地址 0x%02X)%n", b30[15] & 0xFF, i2cslv));
            if (i2cslv == 0x54) {
                log("        => 从地址为默认 0x54（与实测 0x54/0x55 无应答矛盾指向串口被禁用）\n");
            } else {
                log(String.format("        => 从地址非默认! 0x%02X%n", i2cslv));
            }
        }

        byte[] b31 = mBlocks[31];
        if (b31 == null) {
            log("块31 未读到 — 无法解析 RORF/ROSI/SECURITY。\n");
        } else {
            log("[块31 0x01F0..0x01FF]\n");
            log(String.format("  RORF    (0x01F0): %02X %02X %02X %02X  RF侧只读位图(块0-26)%n",
                    b31[0], b31[1], b31[2], b31[3]));
            log(String.format("  ROSI    (0x01F4): %02X %02X %02X %02X  串行侧只读位图(块0-26)%n",
                    b31[4], b31[5], b31[6], b31[7]));
            log(String.format("  SECURITY(0x01F8): %02X %02X %02X %02X  RF明文访问许可位图(块0-26)%n",
                    b31[8], b31[9], b31[10], b31[11]));
            log(String.format("  TNPRM   (0x01FC): %02X%n", b31[12]));
            log(String.format("  HW2     (0x01FD): %02X%n", b31[13]));
            log(String.format("  CONFIG尾 (0x01FE): %02X %02X%n", b31[14], b31[15]));
            if (isAllZero(b31, 0, 4) && isAllZero(b31, 8, 4)) {
                log("        RORF/SECURITY 全 0 => 系统区参数处于默认状态或 CFEN 未启用\n");
            } else {
                log("        RORF/SECURITY 有非零位 => 产线写保护配置在生效!\n");
            }
        }

        boolean anyCfg = mBlocks[27] != null || mBlocks[28] != null || mBlocks[29] != null;
        if (anyCfg) {
            log("\n[CONFIG 区块 27-29 (0x01B0..0x01DF) 原始字节]\n");
            log("  (布局属 NDA 文档,逐字节现场取证 — CFEN/BCC/串口使能位应在此)\n");
            for (int blk = 27; blk <= 29; blk++) {
                byte[] d = mBlocks[blk];
                if (d != null) {
                    log(String.format("  blk%02d (0x%04X): %s%n", blk, 0x01B0 + (blk - 27) * 16, hex(d)));
                } else {
                    log(String.format("  blk%02d: 读取失败 ST=0x%04X%n", blk, mStatus[blk]));
                }
            }
            if (mBlocks[27] != null && isAllZero(mBlocks[27], 0, 16)
                    && isAllZero(mBlocks[28], 0, 16) && isAllZero(mBlocks[29], 0, 16)) {
                log("  CONFIG 全 0 => 未启用(CFEN无效),芯片全部参数用默认值\n");
            }
        }

        // NDEF sanity check on block 0
        if (mBlocks[0] != null) {
            byte[] a = mBlocks[0];
            log(String.format("%n[NDEF属性块0] ver=%02X nbr=%02X nbw=%02X max=%02X rw=%02X%n",
                    a[0], a[1], a[2], a[3], a[4]));
        }
        log("\n===== 完成 — 点「复制全部」发回分析 =====\n");
        toast("Dump 完成");
    }

    private static boolean isAllZero(byte[] d, int off, int len) {
        if (d == null) return false;
        for (int i = off; i < off + len; i++) if (d[i] != 0) return false;
        return true;
    }

    private static String join(String[] arr) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < arr.length; i++) {
            if (i > 0) sb.append(", ");
            sb.append(arr[i].replace("android.nfc.tech.", ""));
        }
        return sb.toString();
    }

    private static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder();
        for (byte x : d) sb.append(String.format("%02X ", x));
        return sb.toString().trim();
    }
}
