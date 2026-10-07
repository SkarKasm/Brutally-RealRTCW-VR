#!/bin/bash
# Builds dist/RealRTCW-VR.zip: the single player VR build for sharing with a friend who owns
# RealRTCW and Return to Castle Wolfenstein. Run deploy.sh first.
cd "$(dirname "$0")/../.."
OUT=dist/RealRTCW-VR
rm -rf $OUT dist/RealRTCW-VR.zip && mkdir -p $OUT/game/main
cp run/RealRTCW.x64.exe run/renderer_sp_opengl1_x64.dll run/SDL3.dll run/OpenAL64.dll \
   run/libopenxr_loader.dll run/libgcc_s_seh-1.dll run/libwinpthread-1.dll run/libstdc++-6.dll $OUT/game/
cp run/main/*.dll run/main/z_vr_assets.pk3 $OUT/game/main/
cp misc/package/find_realrtcw.cmd misc/package/find_realrtcw.ps1 $OUT/game/
cp misc/package/README.txt $OUT/
cp "misc/package/RealRTCW VR.bat" "misc/package/RealRTCW (no VR).bat" $OUT/
for f in $OUT/*.bat $OUT/game/*.cmd $OUT/README.txt; do sed -i 's/\r\?$/\r/' "$f"; done
(cd dist && python -c "import shutil; shutil.make_archive('RealRTCW-VR', 'zip', '.', 'RealRTCW-VR')")
ls -la dist
