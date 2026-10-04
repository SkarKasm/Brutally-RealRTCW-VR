#!/bin/bash
# Launch the monitor build for automated tests with its own profile (run_test/), so tests
# never change the player's settings in run/. Extra args are passed to the game.
cd "$(dirname "$0")/run"
export PATH="/c/Program Files (x86)/Steam/steamapps/common/RealRTCW:$PATH"
cp -u ../run/main/z_vr_assets.pk3 ../run_test/main/ 2>/dev/null
for f in main/*.cfg; do case "$f" in main/vr*.cfg) cp -u "$f" ../run_test/main/;; esac; done
./RealRTCW.x64.exe +set fs_basepath "C:/Program Files (x86)/Steam/steamapps/common/RealRTCW" \
  +set fs_homepath "D:/Dev/RealRTCW-VR/run_test" +set vr_enable 0 +set logfile 2 +set r_fullscreen 0 \
  +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 "$@" &
