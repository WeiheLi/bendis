#!/usr/bin/env bash
# Run p4/test/test_bendis.py on tofino-model. Build first with: p4/sde/build.sh test
# Needs root for the veth pairs; logs go to $LOGDIR (default /tmp/bendis_ptf). The model runs with -q
# (MODEL_OPTS), as its verbose log slows it down enough to lose digests.
set -euo pipefail
: "${SDE:?source ~/setup-open-p4studio.bash first}"
: "${SDE_INSTALL:?source ~/setup-open-p4studio.bash first}"

TEST_DIR=$(cd "$(dirname "$0")/../test" && pwd)
LOGDIR=${LOGDIR:-/tmp/bendis_ptf}
mkdir -p "$LOGDIR"

sudo "$SDE_INSTALL/bin/veth_setup.sh" 64 > "$LOGDIR/veth.log" 2>&1
cd "$SDE"
./run_tofino_model.sh -p bendis --arch tofino ${MODEL_OPTS--q} > "$LOGDIR/model.log" 2>&1 &
MODEL=$!
./run_switchd.sh -p bendis --arch tofino > "$LOGDIR/switchd.log" 2>&1 &
SWITCHD=$!
trap 'kill $SWITCHD $MODEL 2>/dev/null; pkill -f bf_switchd 2>/dev/null; pkill -f tofino-model 2>/dev/null' EXIT

# wait for the BF-RT gRPC server
for _ in $(seq 120); do
  (exec 3<>/dev/tcp/127.0.0.1/50052) 2>/dev/null && break
  sleep 2
done

./run_p4_tests.sh -p bendis --arch tofino -t "$TEST_DIR" -s test_bendis 2>&1 | tee "$LOGDIR/ptf.log"
