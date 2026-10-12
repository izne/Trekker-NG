#!/usr/bin/env bash
# One-command build for Trekker-NG inside the MSYS2 MINGW64 environment.
#   ./build.sh            build -> dist/{trekker-ng,trekker-console}.exe
#                         + SDL2.dll (+ libmp3lame-0.dll when installed, M7)
#                         + libSoundTouch-1.dll + the MinGW runtime it needs
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

# M7: MP3 recording loads libmp3lame-0.dll at runtime (LoadLibrary - no
# link-time dependency). Stage it for the zip; without it the recorder
# falls back to WAV, so a missing DLL is a warning, not an error.
LAME_DLL="/d/msys64/mingw64/bin/libmp3lame-0.dll"
if [ -f "$LAME_DLL" ]; then
  cp -f "$LAME_DLL" dist/libmp3lame-0.dll
else
  echo "warning: libmp3lame-0.dll missing - recordings fall back to WAV" \
       "(pacman -S mingw-w64-x86_64-lame)" >&2
fi

# M9: BPM auto-detection links SoundTouch (LGPL 2.1) - a build dependency
# (CMake fails without it) and a runtime DLL like SDL2. CMake already
# copies it next to the exes in build/; mirror it into dist/. The pacman
# build of SoundTouch links the MinGW C++ runtime dynamically (our exes
# link it statically), so its three runtime DLLs travel too - without
# them the shipped dist/ fails to start on a machine that has no MSYS2
# on PATH.
ST_DLL="/d/msys64/mingw64/bin/libSoundTouch-1.dll"
if [ -f "$ST_DLL" ]; then
  cp -f "$ST_DLL" dist/libSoundTouch-1.dll
else
  echo "error: libSoundTouch-1.dll missing (pacman -S mingw-w64-x86_64-soundtouch)" >&2
  exit 1
fi
for rt in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
  if [ -f "/d/msys64/mingw64/bin/$rt" ]; then
    cp -f "/d/msys64/mingw64/bin/$rt" "dist/$rt"
  else
    echo "error: $rt missing in /d/msys64/mingw64/bin (pacman -S mingw-w64-x86_64-toolchain)" >&2
    exit 1
  fi
done

echo
echo "build OK -> dist/trekker-ng.exe + dist/trekker-console.exe + runtime DLLs:" \
     "SDL2, libSoundTouch-1, libstdc++-6, libgcc_s_seh-1, libwinpthread-1"
echo "DLL dependencies of the UI:"
objdump -p dist/trekker-ng.exe | grep "DLL Name" || true
echo "DLL dependencies of libSoundTouch-1.dll:"
objdump -p dist/libSoundTouch-1.dll | grep "DLL Name" || true

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
  if [ -f dist/libmp3lame-0.dll ]; then
    cp -f dist/libmp3lame-0.dll "$STAGE/"
  fi
  cp -f dist/libSoundTouch-1.dll "$STAGE/"
  cp -f dist/libstdc++-6.dll dist/libgcc_s_seh-1.dll dist/libwinpthread-1.dll "$STAGE/"
  cp -f README.md "$STAGE/"
  # DSEG7 (VFD readouts) is SIL OFL 1.1 - the license must travel with it.
  # libmp3lame and SoundTouch are LGPL 2.1 - same rule, see SPEC §3.
  # One current screenshot (ui-m5g.jpg) - the single image the README shows.
  cp -f docs/FORMAT.md docs/USAGE.md docs/ui-m5g.jpg "$STAGE/docs/"
  cp -f third_party/fonts/OFL-DSEG.txt "$STAGE/docs/"
  if [ -f docs/LGPL-2.1-libmp3lame.txt ]; then
    cp -f docs/LGPL-2.1-libmp3lame.txt "$STAGE/docs/"
  fi
  # SoundTouch's LGPL text as installed by pacman (M9).
  if [ -f /d/msys64/mingw64/share/licenses/soundtouch/COPYING.TXT ]; then
    cp -f /d/msys64/mingw64/share/licenses/soundtouch/COPYING.TXT \
          "$STAGE/docs/LGPL-2.1-soundtouch.txt"
  fi
  rm -f "$ZIP"

  powershell.exe -NoProfile -Command \
    "Compress-Archive -Force -Path '${STAGE}/*' -DestinationPath '${ZIP}'"
  rm -rf "$STAGE"

  echo
  echo "release -> ${ZIP}"
  python -m zipfile -l "$ZIP" || echo "(python not available - list skipped)"
fi
