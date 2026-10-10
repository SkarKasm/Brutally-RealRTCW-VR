#!/bin/bash
# Copy co-op build outputs into ./run (used by the co-op launchers).
cd "$(dirname "$0")"
B=build/release-mingw64-x86_64
mkdir -p run/coopmain
cp -u $B/RTCWCoop.x64.exe $B/RTCWCoopDED.x64.exe $B/renderer_coop_opengl1_x64.dll run/
cp -u code/libs/win64/SDL264.dll code/libs/win64/OpenAL64.dll run/ 2>/dev/null
cp -u $B/coopmain/*.dll run/coopmain/
cp -n misc/vr/controls/vr_controls.cfg run/coopmain/   # VR button layout (the player's edits are kept)
for f in libopenxr_loader.dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll; do
  [ -f /c/msys64/mingw64/bin/$f ] && cp -u /c/msys64/mingw64/bin/$f run/
done

# VR assets (hands, weapon geometry, reload sounds, VR options menu), generated from the
# game's own files: original RTCW paks, then co-op's, then the Cursed Sands mod's
RTCW_MAIN="${RTCW_MAIN:-/d/SteamLibrary/steamapps/common/Return to Castle Wolfenstein/Main}"
COOP_DIR="${COOP_DIR:-/c/Program Files (x86)/Steam/steamapps/common/RealRTCW/coop}"
# one for plain co-op, one for the Cursed Sands mod (its own main menu gets the button)
mkdir -p run/EE
for target in coopmain EE; do
  PK3=run/$target/z_vr_assets.pk3
  DIRS=("$RTCW_MAIN" "$COOP_DIR/coopmain")
  [ -d "$COOP_DIR/../Main" ] && DIRS+=("$COOP_DIR/../Main")      # RealRTCW's remastered weapons
  [ $target = EE ] && DIRS+=("$COOP_DIR/EE")
  if [ ! -f $PK3 ] || [ misc/vr/make_vr_assets.py -nt $PK3 ] || [ misc/vr/vr_assets_common.py -nt $PK3 ] ||      [ misc/vr/ui/vr_options.menu -nt $PK3 ]; then
    python misc/vr/make_vr_assets.py $PK3 "${DIRS[@]}" || exit 1
  fi
done
echo "deployed to $(pwd)/run"
