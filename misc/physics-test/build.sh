#!/bin/sh
# Offline test for the vr_physics ragdolls (Linux, g++). Usage:
#   misc/physics-test/build.sh && PHYS_MAPS=/path/with/maps misc/physics-test/out/harness church xlabs
#   PHYS_MAPS=/path/with/maps misc/physics-test/out/stress church xlabs
# PHYS_MAPS is a folder holding maps/*.bsp, unzipped from the game's pk3 files.
set -e
cd "$(dirname "$0")"
C=../../code
J=$C/JoltPhysics
F="-DNDEBUG -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2 -msse4.2 -mpopcnt -O2 -ffp-contract=off -fno-strict-aliasing"
mkdir -p out/jolt
for f in $(find $J/Jolt -name '*.cpp' -not -path '*/Compute/DX12/*' -not -path '*/Compute/VK/*'); do
  o=out/jolt/$(echo "$f" | sed 's|.*/Jolt/||; s|/|_|g; s|\.cpp$|.o|')
  [ "$o" -nt "$f" ] || echo "g++ -std=c++17 -I$J $F -w -c $f -o $o"
done | xargs -P 8 -I{} sh -c {}
g++ -std=c++17 -I$J $F -DUSE_JOLT -c $C/cgame/cg_physics.cpp -o out/cg_physics.o
for f in harness.c stress.c $C/cgame/cg_ragdoll.c $C/cgame/cg_physworld.c $C/qcommon/q_math.c $C/qcommon/q_shared.c; do
  gcc -O1 -g -DCGAMEDLL -DCGAME -DARCH_STRING=\"x86_64\" -w -c $f -o out/$(basename $f .c).o
done
COMMON="out/cg_ragdoll.o out/cg_physworld.o out/q_math.o out/q_shared.o out/cg_physics.o out/jolt/*.o"
g++ -o out/harness out/harness.o $COMMON -lm
g++ -o out/stress out/stress.o $COMMON -lm
echo "built misc/physics-test/out/harness and out/stress (do bodies go flying?)"
