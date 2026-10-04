#!/bin/bash
# Build RealRTCW-VR with MSYS2 MinGW64. Usage: ./build.sh [extra make args]
# Re-executes itself under MSYS2's bash when started from another shell (e.g. Git Bash).
if [ ! -x /mingw64/bin/gcc ] && [ -x /c/msys64/usr/bin/bash.exe ]; then
  exec /c/msys64/usr/bin/bash.exe -lc "\"$(cd "$(dirname "$0")" && pwd)/build.sh\" $*"
fi
export MSYSTEM=MINGW64
export PATH=/mingw64/bin:/usr/bin:$PATH
cd "$(dirname "$0")"
ARCH=x86_64 mingw32-make -j8 "$@"
