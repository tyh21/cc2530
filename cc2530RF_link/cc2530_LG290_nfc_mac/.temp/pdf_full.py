# -*- coding: utf-8 -*-
import fitz

doc = fitz.open(r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG290_nfc_mac\doc\KM63Y1213_EN_Rev1.0.pdf")
for idx in [27, 28]:
    print("=" * 25, "PAGE", idx + 1, "=" * 25)
    print(doc[idx].get_text()[:3600])
    print()