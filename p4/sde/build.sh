#!/usr/bin/env bash
# Compile bendis.p4 with bf-p4c and install it into $SDE_INSTALL (needs setup-open-p4studio.bash).
#
#   p4/sde/build.sh          # default parameters (p4/README.md)
#   p4/sde/build.sh test     # small count windows expected by p4/test/test_bendis.py
#   P4PPFLAGS="-DCM_IDX_BITS=12" p4/sde/build.sh   # any other -D overrides
#
# The resource report is written to $SDE/build/p4-build/bendis/bendis/tofino/pipe/logs/.
set -euo pipefail
: "${SDE:?source ~/setup-open-p4studio.bash first}"
: "${SDE_INSTALL:?source ~/setup-open-p4studio.bash first}"

P4=$(cd "$(dirname "$0")/.." && pwd)/bendis.p4
FLAGS=${P4PPFLAGS:-}
if [ "${1:-}" = test ]; then
  FLAGS="$FLAGS -DWINDOW_BY_COUNT -DWIN_COUNT_BITS=8 -DCM_IDX_BITS=6 -DTAB_IDX_BITS=6 -DBURST_H=20"
fi

B=$SDE/build/p4-build/bendis
rm -rf "$B"
mkdir -p "$B"
cd "$B"
cmake "$SDE/p4studio" \
  -DCMAKE_MODULE_PATH="$SDE/cmake" \
  -DCMAKE_INSTALL_PREFIX="$SDE_INSTALL" \
  -DP4C="$SDE_INSTALL/bin/p4c" \
  -DP4_PATH="$P4" \
  -DP4_NAME=bendis \
  -DP4_LANG=p4_16 \
  -DTOFINO=ON -DTOFINO2=OFF \
  -DP4PPFLAGS="$FLAGS"
make -j install

find "$B" -path '*pipe/logs*' -name '*.log' | sort
