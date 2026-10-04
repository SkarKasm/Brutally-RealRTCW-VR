#!/bin/bash
# Builds dist/RealRTCW-VR-Coop.zip: the co-op VR build for sharing with a friend who owns
# RealRTCW (with its co-op mode) and Return to Castle Wolfenstein. Run deploy.sh first.
cd "$(dirname "$0")/../.."
OUT=dist/RealRTCW-VR-Coop
rm -rf dist && mkdir -p $OUT/game/coopmain $OUT/game/EE
cp run/RTCWCoop.x64.exe run/RTCWCoopDED.x64.exe run/renderer_coop_opengl1_x64.dll run/SDL264.dll run/OpenAL64.dll \
   run/libopenxr_loader.dll run/libgcc_s_seh-1.dll run/libwinpthread-1.dll run/libstdc++-6.dll $OUT/game/
cp run/coopmain/*.dll run/coopmain/z_vr_assets.pk3 $OUT/game/coopmain/
cp run/EE/z_vr_assets.pk3 $OUT/game/EE/
cp misc/package/find_coop.cmd misc/package/find_coop.ps1 $OUT/game/
mk() {  # name title args
  sed -e "s/@NAME@/$1/" -e "s/@TITLE@/$2/" -e "s/@ARGS@/$3/" misc/package/launcher.cmd.in > "$OUT/$1"
}
mk "Cursed Sands VR.bat" "Cursed Sands co-op in VR (falls back to the monitor without a headset)" "+set fs_game EE"
mk "Cursed Sands (no VR).bat" "Cursed Sands co-op on the monitor" "+set fs_game EE +set vr_enable 0"
mk "Classic co-op VR.bat" "the original campaign in co-op, in VR" '+set fs_game ""'
mk "Classic co-op (no VR).bat" "the original campaign in co-op on the monitor" '+set fs_game "" +set vr_enable 0'
cp misc/package/README.txt $OUT/
for f in $OUT/*.bat $OUT/game/*.cmd $OUT/README.txt; do sed -i 's/\r\?$/\r/' "$f"; done
(cd dist && powershell -NoProfile -Command "Compress-Archive -Path RealRTCW-VR-Coop -DestinationPath RealRTCW-VR-Coop.zip -Force")
ls -la dist
