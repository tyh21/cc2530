# -*- coding: utf-8 -*-
import fitz
doc = fitz.open(r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG290_nfc_mac\doc\mn63y1213.pdf")
for idx in [12, 18, 19]:
    print("="*20, "PDF PAGE", idx+1, "="*20)
    print(doc[idx].get_text())