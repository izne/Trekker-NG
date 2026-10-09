#!/usr/bin/env bash
# One-command build for Trekker-NG inside the MSYS2 MINGW64 environment.
#   ./build.sh            build -> dist/{trekker-ng,trekker-console}.exe + SDL2.dll
#   ./build.sh release    build + pack dist/trekker-ng-<VERSION>-win64.zip
set -euo pipefail
cd "$(dirname "$0")"

# Make sure we use the MINGW64 toolchain even if the shell was started elsewhere.
if [ -d /d/msys64/mingw64/bin ] && [[ ":$PATH:" != *":/d/msys64/mingw64/bin:"* ]]; then
  export PATH="/d/msys64/mingw64/bin:$PATH"
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTREKKER_BUILD_TESTS=ON
cmake --build build

# Test suite (SPEC 9): a failing test stops the build here, before dist/ is
# touched.
if [ -f build/CTestTestfile.cmake ]; then
  ctest --test-dir build --output-on-failure
fi

mkdir -p dist
cp -f build/trekker-ng.exe dist/trekker-ng.exe
cp -f build/trekker-console.exe dist/trekker-console.exe

# SDL2 runtime for the ImGui UI (SPEC §3: pacman-provided SDL2).
SDL2_DLL="$(ls /d/msys64/mingw64/bin/SDL2*.dll 2>/dev/null | head -n 1 || true)"
if [ -z "$SDL2_DLL" ]; then
  echo "error: SDL2.dll not found in /d/msys64/mingw64/bin (pacman -S mingw-w64-x86_64-SDL2)" >&2
  exit 1
fi
cp -f "$SDL2_DLL" dist/SDL2.dll

echo
echo "build OK -> dist/trekker-ng.exe + dist/trekker-console.exe + dist/SDL2.dll"
echo "DLL dependencies of the UI (should be SDL2.dll + Windows system DLLs):"
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
  cp -f dist/trekker-console.exe "$STAGE/"
  cp -f dist/SDL2.dll "$STAGE/"
  cp -f README.md "$STAGE/"
  # DSEG7 (VFD readouts) is SIL OFL 1.1 - the license must travel with it.
  # M5e screenshots are current; m4d/m5c ship too (UI history in docs/).
  cp -f docs/FORMAT.md docs/USAGE.md docs/ui-m4d.png docs/ui-m5c-settings.png \
        docs/ui-m5e.png docs/ui-m5e-single.png docs/ui-m5e-settings.png "$STAGE/docs/"
  cp -f third_party/fonts/OFL-DSEG.txt "$STAGE/docs/"
  rm -f "$ZIP"

  powershell.exe -NoProfile -Command \
    "Compress-Archive -Force -Path '${STAGE}/*' -DestinationPath '${ZIP}'"
  rm -rf "$STAGE"

  echo
  echo "release -> ${ZIP}"
  python -m zipfile -l "$ZIP" || echo "(python not available - list skipped)"
fi
