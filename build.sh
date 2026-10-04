#!/usr/bin/env bash
# One-command rebuild for StemDeck inside the MSYS2 MINGW64 environment.
# Usage: ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

# Make sure we use the MINGW64 toolchain even if the shell was started elsewhere.
if [ -d /d/msys64/mingw64/bin ] && [[ ":$PATH:" != *":/d/msys64/mingw64/bin:"* ]]; then
  export PATH="/d/msys64/mingw64/bin:$PATH"
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

mkdir -p dist
cp -f build/stemdeck.exe dist/stemdeck.exe

echo
echo "build OK -> dist/stemdeck.exe"
echo "DLL dependencies (should only be Windows system DLLs):"
objdump -p dist/stemdeck.exe | grep "DLL Name" || true
