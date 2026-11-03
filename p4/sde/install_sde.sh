#!/usr/bin/env bash
# Install open-p4studio for bendis.p4: bf-p4c, tofino-model, and bf_switchd with BF-RT gRPC,
# Tofino-1 only, no kernel modules (profile.yaml). Run as a user with sudo; takes 1-3 hours.
#
#   p4/sde/install_sde.sh [SDE dir, default ~/open-p4studio] [jobs, default nproc]
#   source ~/setup-open-p4studio.bash
#
# Environment knobs, for hosts where the defaults fail:
#   PYTHON=python3.12        interpreter for p4studio (grpcio does not build on Python 3.13)
#   BOOST_FROM_GITHUB=1      fetch boost 1.83 from GitHub releases when archives.boost.io is unreachable
#   SKIP_SYSTEM_CHECK=1      skip p4studio's disk/memory/kernel-header checks
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDE=${1:-$HOME/open-p4studio}
JOBS=${2:-$(nproc)}
PYTHON=${PYTHON:-python3}

if [ ! -d "$SDE" ]; then
  git clone --depth 1 https://github.com/p4lang/open-p4studio.git "$SDE"
fi
cd "$SDE"

if [ "${BOOST_FROM_GITHUB:-0}" = 1 ]; then
  sed -i "s#url: 'https://archives.boost.io/release/1.74.0/source/boost_1_74_0.tar.bz2', version: 1.74.0#url: 'https://github.com/boostorg/boost/releases/download/boost-1.83.0/boost-1.83.0.tar.xz', version: 1.83.0#" \
    p4studio/dependencies/dependencies.yaml
fi

args=(profile apply --jobs "$JOBS")
[ "${SKIP_SYSTEM_CHECK:-0}" = 1 ] && args+=(--skip-system-check)
"$PYTHON" p4studio "${args[@]}" "$HERE/profile.yaml"

./create-setup-script.sh > "$HOME/setup-open-p4studio.bash"
echo "Installed. Run: source ~/setup-open-p4studio.bash"
