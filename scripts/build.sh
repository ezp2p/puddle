#!/usr/bin/env bash
# Build and test Puddle locally.
#   scripts/build.sh              configure, build, run tests
#   scripts/build.sh --clean      wipe build/ first
#   scripts/build.sh --no-gui     skip the Qt app (headless hosts)
#   scripts/build.sh --release    optimized build with static OpenSSL
set -euo pipefail
cd "$(dirname "$0")/.."

clean=0 gui=ON type=Debug static=OFF
for arg in "$@"; do
  case "$arg" in
    --clean)   clean=1 ;;
    --no-gui)  gui=OFF ;;
    --release) type=Release static=ON ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

[ "$clean" = 1 ] && rm -rf build

args=(-DCMAKE_BUILD_TYPE="$type" -DPUDDLE_STATIC_OPENSSL="$static" -DPUDDLE_BUILD_GUI="$gui")
# Homebrew's OpenSSL is keg-only; the system one is LibreSSL.
if [ "$(uname)" = Darwin ] && command -v brew >/dev/null; then
  args+=(-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)")
fi

# A job count, always: with Makefiles, a bare --parallel means no limit, and a full rebuild then starts dozens of
# compilers at once (enough to run a 32 GB machine out of memory). At most 8 unless CMAKE_BUILD_PARALLEL_LEVEL says.
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-$(( cores < 8 ? cores : 8 ))}

cmake -S . -B build "${args[@]}"
cmake --build build --parallel "$jobs"
ctest --test-dir build --output-on-failure

echo
echo "Built:"
ls -d build/puddled build/Puddle build/Puddle.app 2>/dev/null | sed 's/^/  /'  || true  # one of the app paths is always missing
