#!/usr/bin/env bash
# Runs INSIDE the ubuntu:22.04 build container. Do not run on a host with a
# newer glibc — the artifacts would not load on the Move (glibc 2.35).
set -euo pipefail
TARGET="${1:-all}"

# ---- Contract gates. Each one exists because its failure mode is invisible
# at runtime: a renamed parameter still "works", it just is not 4K EQ 2 any
# more; a stale module.json still loads, the host just never offers the knob.
# The DSP must BE Dusk Audio's, not resemble it. Byte-compares every file
# under src/ported/ against a fresh upstream clone; skips gracefully offline.
echo "=== vendored DSP is verbatim upstream ==="
python3 tools/check_vendored_upstream.py

echo "=== parameter contract vs upstream ==="
python3 tools/check_upstream_params.py

echo "=== module.json is in sync with the parameter table ==="
python3 tools/gen_module_json.py --check

# help.json fails silently in three separate ways on the device — wrong
# top-level key and the loader drops the whole file, over-long lines lose
# their tails at x=127, non-ASCII renders as a 1px gap. None of it logs.
echo "=== help.json is in the shape the device reads ==="
python3 tools/check_help.py

# ---- Native build + loadtest FIRST, in-container, before cross-compiling.
# A red test here fails the whole build.
echo "=== native build + loadtest ==="
mkdir -p build-native
g++ -O2 -std=c++17 -Wall -Wextra -shared -fPIC \
    src/dsp/fourk_eq_plugin.cpp src/ported/shared-daf/dsp/FourKEQDSP.cpp \
    -Isrc/dsp -Isrc/host -Isrc/ported/shared-daf/dsp -Isrc/ported/daf-plugin \
    -o build-native/4k-eq.so -lm -lpthread
g++ -O2 -std=c++17 -Wall tools/loadtest.cpp -Isrc/host \
    -o build-native/fkq_loadtest -ldl -lm
./build-native/fkq_loadtest ./build-native/4k-eq.so src/module.json src/help.json

# ---- Cross-compile for the device.
echo "=== cross-compile aarch64 ==="
cmake -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build --target "$TARGET" -j"$(nproc)"

# ---- The device is glibc 2.35. A newer symbol requirement makes the module
# fail to dlopen with an error the chain host swallows, so check it here
# rather than discovering it as a silent dead slot.
echo "=== glibc symbol check (must be <= 2.35) ==="
MAXV=$(aarch64-linux-gnu-objdump -T build/4k-eq.so \
        | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
echo "  highest required: ${MAXV:-none}"
if [ -n "${MAXV:-}" ]; then
    HIGH=$(printf '%s\nGLIBC_2.35\n' "$MAXV" | sort -uV | tail -1)
    [ "$HIGH" = "GLIBC_2.35" ] || { echo "FAIL: needs $MAXV, Move has 2.35"; exit 1; }
fi
echo "  ok"

# ---- Package for the Module Store ----
rm -rf dist/4k-eq
mkdir -p dist/4k-eq
cp build/4k-eq.so       dist/4k-eq/
cp src/module.json      dist/4k-eq/
cp src/help.json        dist/4k-eq/
cp src/web_ui.html      dist/4k-eq/
cp LICENSE              dist/4k-eq/
(cd dist && tar -czf 4k-eq-module.tar.gz 4k-eq/)
echo "Tarball: dist/4k-eq-module.tar.gz"

echo; echo "=== Build output ==="
find build -maxdepth 1 -type f \( -name "*.so" -o -name "fkq_*" \) \
    -exec sh -c 'printf "%s\n  " "$1"; file -b "$1"' _ {} \;
