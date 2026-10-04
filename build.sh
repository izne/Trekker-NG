#!/usr/bin/env bash
# One-command build for Trekker-NG inside the MSYS2 MINGW64 environment.
#   ./build.sh            build -> dist/trekker-ng.exe (runs tests when present)
#   ./build.sh release    build + pack dist/trekker-ng-<VERSION>-win64.zip
set -euo pipefail
cd "$(dirname "$0")"

# Make sure we use the MINGW64 toolchain even if the shell was started elsewhere.
if [ -d /d/msys64/mingw64/bin ] && [[ ":$PATH:" != *":/d/msys64/mingw64/bin:"* ]]; then
  export PATH="/d/msys64/mingw64/bin:$PATH"
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTREKKER_BUILD_TESTS=ON
cmake --build build

# Test suite (SPEC 9). Once CTest is wired up (M2), a failing test stops the
# build here, before dist/ is touched.
if [ -f build/CTestTestfile.cmake ]; then
  ctest --test-dir build --output-on-failure
fi

mkdir -p dist
cp -f build/trekker-ng.exe dist/trekker-ng.exe

echo
echo "build OK -> dist/trekker-ng.exe"
echo "DLL dependencies (should only be Windows system DLLs):"
objdump -p dist/trekker-ng.exe | grep "DLL Name" || true

# ---------------------------------------------------------------- release ---
if [ "${1:-}" = "release" ]; then
  VERSION="$(tr -d '[:space:]' < VERSION)"
  if ! [[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "error: VERSION file contains unexpected characters: '$VERSION'" >&2
    exit 1
  fi
  NAME="trekker-ng-${VERSION}-win64"
  STAGE="dist/${NAME}"
  ZIP="dist/${NAME}.zip"

  rm -rf "$STAGE"
  mkdir -p "$STAGE/docs"
  cp -f dist/trekker-ng.exe "$STAGE/"
  cp -f README.md "$STAGE/"
  cp -f docs/FORMAT.md docs/USAGE.md "$STAGE/docs/"
  rm -f "$ZIP"

  powershell.exe -NoProfile -Command \
    "Compress-Archive -Force -Path '${STAGE}/*' -DestinationPath '${ZIP}'"
  rm -rf "$STAGE"

  echo
  echo "release -> ${ZIP}"
  python -m zipfile -l "$ZIP" || echo "(python not available - list skipped)"
fi
