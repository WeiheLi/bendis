#!/usr/bin/env bash
# Compiles bendis.p4 with bf-p4c for several table sizes and collects the average per-stage
# resource utilization (as in mau.resources.log) into results/tofino/resources.csv.
#   p4/sde/resources.sh [bits...]     (default: 9 10 11 12 13; CM_IDX_BITS = TAB_IDX_BITS = bits)
#   P4FLAGS="-DML_FLOWKEY ..." OUTNAME=resources_ml p4/sde/resources.sh   # other -D flags and output name
set -euo pipefail
: "${SDE_INSTALL:?source ~/setup-open-p4studio.bash first}"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$REPO/results/tofino
WORK=${WORK:-/tmp/bendis_resources}
mkdir -p "$OUT" "$WORK"
BITS=${*:-9 10 11 12 13}
NAME=${OUTNAME:-resources}
echo "bits,mem_kb,stages,exm_xbar,hash_bits,hash_dist,gateway,sram,map_ram,tcam,vliw,stateful_alu,logical_tables" > "$OUT/$NAME.csv"
for b in $BITS; do
    d=$WORK/b$b
    rm -rf "$d"
    "$SDE_INSTALL/bin/p4c" --std p4_16 --target tofino --arch tna -o "$d" -g \
        -DCM_IDX_BITS=$b -DTAB_IDX_BITS=$b ${P4FLAGS:-} "$REPO/p4/bendis.p4" > "$d.log" 2>&1
    cp "$d/pipe/logs/mau.resources.log" "$OUT/mau.$NAME.b$b.log"
    stages=$(grep -cE "^stage [0-9]+ ingress" "$d/pipe/bendis.bfa")
    avg=$(awk '/Stage Number \| Exact Match Input xbar/{n++} n==2 && /Average/' "$d/pipe/logs/mau.resources.log" \
          | tr -d ' %' | awk -F'|' '{print $3","$5","$6","$7","$8","$9","$10","$11","$12","$22}')
    echo "$b,$(( 26 * (1 << b) / 1024 )),$stages,$avg" >> "$OUT/$NAME.csv"
done
cat "$OUT/$NAME.csv"
