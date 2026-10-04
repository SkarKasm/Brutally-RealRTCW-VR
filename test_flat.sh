#!/bin/bash
# Flat (no headset) test run with a throwaway profile in ./run_test. Extra args go to the game.
cd "$(dirname "$0")"
COOP_DIR="${COOP_DIR:-C:/Program Files (x86)/Steam/steamapps/common/RealRTCW/coop}"
mkdir -p run_test/coopmain run_test/EE
cp -u run/coopmain/* run_test/coopmain/
cp -u run/EE/* run_test/EE/
cd run
./RTCWCoop.x64.exe +set fs_basepath "$COOP_DIR" +set fs_homepath "$(cygpath -w "$(pwd)/../run_test")" \
  +set vr_enable 0 +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  "$@" > ../run_test/stdout.log 2>&1
