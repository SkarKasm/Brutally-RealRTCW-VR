#!/bin/bash
# Copy build outputs into ./run (the runtime folder used by run_vr.bat).
cd "$(dirname "$0")"
B=build/release-mingw64-x86_64-nosteam
mkdir -p run/main
cp -u $B/RealRTCW.x64.exe $B/renderer_sp_opengl1_x64.dll $B/SDL3.dll $B/OpenAL64.dll run/
cp -u $B/main/*.dll run/main/
for f in openxr_loader.dll libopenxr_loader.dll; do
  [ -f "$B/$f" ] && cp -u "$B/$f" run/
done
echo "deployed to $(pwd)/run"

# VR hand models, built from the RealRTCW install (not redistributable, so generated locally)
RRTCW_MAIN="/c/Program Files (x86)/Steam/steamapps/common/RealRTCW/Main"
if [ ! -f run/main/z_vr_assets.pk3 ] || [ misc/vr/make_vr_assets.py -nt run/main/z_vr_assets.pk3 ] || [ misc/vr/ui/vr_options.menu -nt run/main/z_vr_assets.pk3 ]; then
  python misc/vr/make_vr_assets.py "$RRTCW_MAIN" run/main/z_vr_assets.pk3
fi
