#!/bin/bash
# count_probe.sh ADDR N1 N2 N3 ... — 对 ADDR 用各 ignore-count 测"第 N+1 次是否到达"
PROJ="C:/Users/tyh21/Desktop/cc2530/cc2530_RF_link/cc2530_LG290_nfc_mac"
CSPYBAT="C:/Program Files (x86)/IAR Systems/Embedded Workbench 8.0/common/bin/CSpyBat.exe"
MACTMP="$PROJ/tools/cspy/_probe_gen.mac"
GEN="$PROJ/settings/project.Debug.general.xcl"
DRV="$PROJ/settings/project.Debug.driver.xcl"
ROUNDS=${ROUNDS:-3}
TIMEOUT=${TIMEOUT:-20}
ADDR="$1"; shift
for N in "$@"; do
  hits=0
  for r in $(seq 1 $ROUNDS); do
    cat > "$MACTMP" << EOF
execUserSetup() { }
execUserReset() { __setCodeBreak("$ADDR", $N, "on_h()", "TRUE", ""); }
on_h() { __message "HIT\n"; }
EOF
    out=$("$CSPYBAT" --silent --timeout $TIMEOUT --macro "$MACTMP" -f "$GEN" --backend -f "$DRV" 2>&1)
    if echo "$out" | grep -q "HIT"; then hits=$((hits+1)); fi
  done
  printf "ignore=%5s -> call #%-5s  %d/%d\n" "$N" "$((N+1))" "$hits" "$ROUNDS"
done
