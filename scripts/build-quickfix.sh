#!/usr/bin/env bash
# build-quickfix.sh: build QuickFIX from the maintained fork at a ref, and install it under a prefix (#178).
#
# The server image's builder stage and CI's FIX-on jobs both build QuickFIX with this, at the ref
# docker/Dockerfile.oms-server pins (QUICKFIX_REF), so what CI tests is what the image ships.
# Before #178, CI used whatever QuickFIX 22 the runner host had in /usr/local.
#
# usage: scripts/build-quickfix.sh REF PREFIX
#   REF     a tag of https://github.com/dudleylane/quickfix, such as v22.1.0
#   PREFIX  where to install it. A build finds it with -DCMAKE_PREFIX_PATH=PREFIX.
#
# Environment:
#   CC    the C compiler (default: gcc-toolset-15's gcc). CMake takes the C++ compiler from CXX.
#   JOBS  parallel build jobs (default: nproc)
#
# It needs git, cmake, a C++ compiler, openssl-devel and network access to github.com. The last
# thing it writes is PREFIX/quickfix-ref, which names the ref and its commit: a prefix without it
# holds an interrupted build.
#
# Exit codes: 0 = installed, 1 = a step failed, 2 = usage error

set -euo pipefail

if (( $# != 2 )) || [[ -z "$1" || -z "$2" ]]; then
    echo "usage: $0 REF PREFIX" >&2
    exit 2
fi
REF=$1
PREFIX=$2
CC="${CC:-/opt/rh/gcc-toolset-15/root/usr/bin/gcc}"
JOBS="${JOBS:-$(nproc)}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

git clone --quiet --depth 1 --branch "$REF" https://github.com/dudleylane/quickfix.git "$WORK/src"
commit="$(git -C "$WORK/src" rev-parse HEAD)"
# The gateway needs only the library: no tests, examples, Python binding or TBB allocator
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_C_COMPILER="$CC" -DHAVE_SSL=ON -DENABLE_TBB_ALLOCATOR=OFF -DQUICKFIX_TESTS=OFF \
    -DQUICKFIX_EXAMPLES=OFF -DHAVE_PYTHON3=OFF
cmake --build "$WORK/build" -j"$JOBS"
cmake --install "$WORK/build"
printf '%s %s\n' "$REF" "$commit" > "$PREFIX/quickfix-ref"
echo "build-quickfix: QuickFIX $REF ($commit) is in $PREFIX"
