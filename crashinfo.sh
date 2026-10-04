#!/bin/bash
# Turn run/crash_report.txt into function names and source lines.
cd "$(dirname "$0")"
B=build/release-mingw64-x86_64-nosteam
A2L=/c/msys64/mingw64/bin/addr2line
grep -oE '[A-Za-z0-9_.]+\+0x[0-9a-f]+' run/crash_report.txt | while IFS=+ read mod off; do
  case "$mod" in
    RealRTCW.x64.exe|renderer_*) f=$B/$mod ;;
    cgame_*|qagame_*|ui_*) f=$B/main/$mod ;;
    *) echo "$mod+$off"; continue ;;
  esac
  base=$(/c/msys64/mingw64/bin/objdump -p "$f" | awk '/^ImageBase/ {print $2}')
  addr=$(printf '0x%x' $((0x$base + off)))
  echo "$mod+$off -> $($A2L -f -C -p -e "$f" $addr)"
done
