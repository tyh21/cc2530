#!/bin/bash
# probe_chain.sh — 对一串地址逐个跑 ignore-count=0 到达探针 (每地址 3 轮)
# 用法: ./probe_chain.sh addr1=name1 addr2=name2 ...
PROJ="C:/Users/tyh21/Desktop/cc2530/cc2530_RF_link/cc2530_LG290_nfc_mac"
CSPYBAT="C:/Program Files (x86)/IAR Systems/Embedded Workbench 8.0/common/bin/CSpyBat.exe"
MACTMP="$PROJ/tools/cspy/_probe_gen.mac"
GEN="$PROJ/settings/project.Debug.general.xcl"
DRV="$PROJ/settings/project.Debug.driver.xcl"
ROUNDS=${ROUNDS:-3}
TIMEOUT=${TIMEOUT:-12}

for item in "$@"; do
  addr="${item%%=*}"
  name="${item##*=}"
  hits=0
  for r in $(seq 1 $ROUNDS); do
    cat > "$MACTMP" << EOF
execUserSetup() { }
execUserReset() { __setCodeBreak("$addr", 0, "on_h()", "TRUE", ""); }
on_h() { __message "HIT\n"; }
EOF
    out=$("$CSPYBAT" --silent --timeout $TIMEOUT --macro "$MACTMP" -f "$GEN" --backend -f "$DRV" 2>&1)
    if echo "$out" | grep -q "HIT"; then hits=$((hits+1)); fi
  done
  printf "%-8s %-22s %d/%d\n" "$addr" "$name" "$hits" "$ROUNDS"
done
