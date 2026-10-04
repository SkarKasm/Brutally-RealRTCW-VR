#!/bin/bash
# Copy co-op build outputs into ./run (used by the co-op launchers).
cd "$(dirname "$0")"
B=build/release-mingw64-x86_64
mkdir -p run/coopmain
cp -u $B/RTCWCoop.x64.exe $B/RTCWCoopDED.x64.exe $B/renderer_coop_opengl1_x64.dll run/
cp -u code/libs/win64/SDL264.dll code/libs/win64/OpenAL64.dll run/ 2>/dev/null
cp -u $B/coopmain/*.dll run/coopmain/
for f in libopenxr_loader.dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll; do
  [ -f /c/msys64/mingw64/bin/$f ] && cp -u /c/msys64/mingw64/bin/$f run/
done
echo "deployed to $(pwd)/run"
