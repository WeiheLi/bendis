#!/usr/bin/env bash
# Replays the first N packets of a trace through bendis.p4 on tofino-model and compares the
# digests with the P4 golden model (bin/p4_expect). Needs setup-open-p4studio.bash and root.
#
#   p4/sde/replay.sh <trace.t21> [N=262144] [WIN_BITS=12] [BITS=10] [H=10]
set -euo pipefail
: "${SDE:?source ~/setup-open-p4studio.bash first}"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TRACE=$1 N=${2:-262144} WB=${3:-12} BITS=${4:-10} H=${5:-10}
OUT=${OUT:-$REPO/results/tofino/replay}
LOGDIR=${LOGDIR:-/tmp/bendis_replay}
mkdir -p "$OUT" "$LOGDIR"
PREFIX=$OUT/replay_n${N}_w${WB}_b${BITS}_h${H}

"$REPO/bin/p4_expect" "$TRACE" tuple21 "$N" "$PREFIX" --win-bits "$WB" --bits "$BITS" --h "$H" | tee "$PREFIX.model.txt"
P4PPFLAGS="-DWINDOW_BY_COUNT -DWIN_COUNT_BITS=$WB -DCM_IDX_BITS=$BITS -DTAB_IDX_BITS=$BITS -DBURST_H=$H" \
    "$REPO/p4/sde/build.sh" > "$LOGDIR/build.log" 2>&1

sudo "$SDE_INSTALL/bin/veth_setup.sh" 64 > "$LOGDIR/veth.log" 2>&1 || true
cd "$SDE"
./run_tofino_model.sh -p bendis --arch tofino ${MODEL_OPTS--q} > "$LOGDIR/model.log" 2>&1 &
MODEL=$!
./run_switchd.sh -p bendis --arch tofino > "$LOGDIR/switchd.log" 2>&1 &
SWITCHD=$!
trap 'kill $SWITCHD $MODEL 2>/dev/null; pkill -f bf_switchd 2>/dev/null; pkill -f tofino-model 2>/dev/null' EXIT
for _ in $(seq 120); do (exec 3<>/dev/tcp/127.0.0.1/50052) 2>/dev/null && break; sleep 2; done

printf '{"ids": "%s", "out": "%s", "gap": %s}\n' "$PREFIX.ids" "$PREFIX.tofino.csv" "${GAP:-0.0005}" \
    > /tmp/bendis_replay.json
./run_p4_tests.sh -p bendis --arch tofino -t "$REPO/p4/test" -s test_replay > "$LOGDIR/ptf.log" 2>&1 || true
tail -3 "$LOGDIR/ptf.log"

python3 - "$PREFIX.expected.csv" "$PREFIX.tofino.csv" <<'EOF' | tee "$PREFIX.compare.txt"
import sys
from collections import Counter
rd = lambda p: Counter(tuple(map(int, l.split(','))) for l in open(p).read().split('\n')[1:] if l)
exp, got = rd(sys.argv[1]), rd(sys.argv[2])
print(f"expected {sum(exp.values())} digests, tofino-model {sum(got.values())}, identical={exp == got}, "
      f"missing {sum((exp - got).values())}, extra {sum((got - exp).values())}")
EOF
