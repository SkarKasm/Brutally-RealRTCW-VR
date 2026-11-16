# HitReact

Active ragdoll hit reactions on [Jolt Physics](https://github.com/jrouwe/JoltPhysics), as a plain C
library any game can call. It grew out of Brutally RealRTCW VR's ragdolls, and RealRTCW-VR now runs on
it: the same code, proven frame-for-frame identical to the version that was built into the game.

What a character does when hit:

| What happened | What he does |
|---|---|
| Shot and stunned, alive | Staggers under the bullets, steps to catch himself (capture-point balancer), clutches the wound, flails if he's going over, comes to on his feet when steady. Shoved over: tells the game (`HR_EV_FELL`). |
| Knocked down, alive | Staggers, goes over, breaks the fall, lies there hurting, then gets up as physics through kneel / press / tuck / sit poses (or the game's own get-up animation). |
| Killed | Fights for his feet according to where he was hit: doubled over a gut wound, reeling from a chest shot, hopping on a shot leg. Then gives out, throws his arms out to break the fall, writhes with fading gasps, goes limp. |
| Headshot, blast | Limp at once (a blast throws him first). |

## Files

| File | What it is |
|---|---|
| `hitreact.h` | The whole public API (C, `extern "C"` safe). Read its comments first. |
| `hr_humanoid.c` | The reaction brain: body building, muscles, balance, euphoria, get-up. Plain C99. |
| `hr_physics.cpp` | The Jolt layer (`HRP_*`): level, movers, ragdoll bodies and joints. C++17. |
| `test/hr_demo.c` | A complete host in 400 lines, with no game engine. Start here when porting. |
| `tools/hr_render.py` | Renders a demo run as a stick-figure GIF. |
| `tools/hr_compare.py` | Compares two demo runs (for example, inches vs metres). |
| `Makefile` | Builds the library, the DLL or .so, and the demo, on its own. |
| `hitreact.def`, `hitreact.map` | The exported names for the DLL / .so (the C API only). |

Licence: GPL v3 or later, like RealRTCW-VR. Jolt is MIT. C&C Renegade's released source is GPL v3, so the two can be combined.

## Building

```sh
make                                  # out/libhitreact.a and out/hr_demo
make shared                           # out/libhitreact.so (87 exports, Jolt hidden)
make CROSS=x86_64-w64-mingw32- all shared   # Windows: libhitreact.a, hitreact.dll + import lib, hr_demo.exe
make demo-run                         # runs the demo in inches/Z-up and metres/Y-up and compares them
python3 tools/hr_render.py out/frames-inches.csv out/hr_demo.gif out/timeline-inches.txt
```

Jolt is taken from `../JoltPhysics` (override with `JOLT=`) and built once into `out/jolt`.
Keep `-ffp-contract=off` and the SSE4.2 flags. Without them the results change.

For a 32-bit game (SoF2 and Renegade are both 32-bit Windows programs), build with a 32-bit compiler, e.g. `CROSS=i686-w64-mingw32-`.
Jolt supports x86 32-bit. Or drop the three source files and Jolt into the game's own project.
With MSVC, compile `hr_humanoid.c` as C and `hr_physics.cpp` as C++17, with `/fp:precise`.

## Concepts

**Spaces.** Inside, Z is up and 1 unit = 1 inch, as in Quake-engine games, and every tuning number is in those units.
Call `HR_SetHostSpace(hostUnitsPerInch, yUp)` once, and everything the host passes or gets back (points, hits, origins, events, gravity, level geometry) is in the host's own space:

| Game / engine | Call | Notes |
|---|---|---|
| RTCW, SoF2, Jedi Academy, Quake 3 | `HR_SetHostSpace(1, 0)` | Exact fast path: no conversion at all. |
| C&C Renegade (W3D) | `HR_SetHostSpace(0.0254f, 0)` | Metres, Z up. Gravity 400 in/s² = 10.16 m/s². |
| Metres, Y up, right-handed | `HR_SetHostSpace(0.0254f, 1)` | Host x, y, z = library x, z, −y. |
| Unity (metres, Y up, **left-handed**) | `HR_SetHostSpace(0.0254f, 1)` | Negate Z on the way in and out yourself (positions, velocities; for rotations negate the Z row and column). |
| Unreal (cm, Z up, **left-handed**) | `HR_SetHostSpace(2.54f, 0)` | Negate Y on the way in and out yourself. |

The demo runs the same scene in inches/Z-up and metres/Y-up. The axis conversion is bit-for-bit exact. Unit scaling only adds float rounding, which drifts no more than nudging the inch run by 0.00001 in does.

**The skeleton** goes in as 21 points (`HR_P_*`): pelvis, chest, neck, head, top of head, and shoulder / elbow / wrist / hand-end on each side, plus hip / knee / ankle / toe on each side.
It comes out as 15 rigid parts (`HR_S_*`): a box torso and chest, capsules for the head and limbs, swing-twist joints, and hinged elbows and knees.
A point the skeleton lacks can be left out (`hasPoint` 0), but the pelvis, neck and shoulders are needed.
For RTCW's 3ds Max Biped skeletons the mapping is in `cg_ragdoll.c` (`rdPointBones`): pelvis = `Bip01 Pelvis`, chest = `Spine2`, hand end = `Finger2`, toe = `Toe0`, and so on.

**Skinning** (drawing the mesh from the physics) is the host's job, and it is simple.
Each bone belongs to one segment. When the body is built, record each bone's world axis and position, `bone0Axis` and `bone0Pos`. Then every frame `HR_Frame` returns 1:

```c
int part = b->segPart[boneSegment];
boneAxis = partRot[part] * bone0Axis;                                          // HR_MatMul
bonePos  = partRot[part] * (bone0Pos - part0Pos[part]) + partPos[part];        // HR_MatVec
```

`RD_PhysPose` in `code/cgame/cg_ragdoll.c` is the reference implementation, including a smoothed neck. The `hrBody_t` fields are in library space. `HR_GetPart` and `HR_GetPartRotation` give the same values in host space.

**Determinism.** The library is single-threaded, with fixed steps. The same build with the same calls gives the same result every run.
Different C runtimes differ slightly (`rand()`, `sinf()`), so give `hrHost_t.random` your own generator if two platforms must agree.
Ragdolls are cosmetic and client-side in RTCW. Keep them that way in other games, because then they never need to agree over the network.

## Wiring it into a game: the checklist

Every port is the same eight steps. `test/hr_demo.c` does all of them except skinning, so read it alongside this.

1. **Start the physics and give it the level.** Call `HRP_Init(print)`, then `HRP_BeginWorld()`. Add each solid brush or convex piece with `HRP_AddWorldHull(points, n)`, add meshes triangle by triangle with `HRP_AddWorldTriangle`, then call `HRP_EndWorld()`.
   Moving things (doors, lifts, vehicles) are movers: `HRP_CreateMover`, `HRP_MoverAddHull`, `HRP_MoverFinish`, then `HRP_MoverPlace(m, origin, axis, teleport)` every frame. A mover stops where it was last placed, however many physics steps follow.
   RTCW reads the map's .bsp for this (`code/cgame/cg_physworld.c`, about 450 lines, and the model for any Quake-family game).
2. **Callbacks and options.** Call `HR_SetHost` with print, event, and optionally trace, get-up pose, get-up animation and random. Then call `HR_SetOptions` with gravity in host units, active, euphoria, strength, maxActive and debug.
   With no trace callback, HitReact traces the level it was given (`HRP_RayCast`). Give it the game's own trace if other things (characters, props) should count.
3. **Each frame, the clock.** Call `HR_SetTime(gameMs)` before anything else.
4. **A hit starts a reaction.** The first time a character is hit, fill an `hrBody_t`. It must be all zeros before its first use (`memset`, or static storage), and its memory must not move or be freed before `HR_BodyFree`:
   `HR_BodySetSkeleton(b, user, id, points, hasPoint, NULL, vel, push, origin)`, then `HR_BodyBuild(b, 1)`, then `part = HR_BodyShot(b, point, bulletVel)`.
   Then tell it what the game decided:
   - `HR_Stun(b)`: stunned, alive.
   - `HR_KnockDown(b)`: knocked down, alive.
   - `HR_Die(b, part, point)`: killed. Set `b->blasted = 1` first for an explosion death, and use `HR_BodyBlast` for the throw.
   - Pass `push`, a 110 in/s push away from the killer, for the dead. Without it they reel back, whatever direction the shot came from.
   - Later hits call `HR_BodyShot` again. Use `HR_Rehit` when stunned, and `HR_Killed` when a living body is killed.
   - Scale a living man's bullet velocity down (RTCW uses ×0.25). He's knocked about, not thrown.
5. **Each frame, per body,** in this order:
   - `HR_Living(b, stunnedNow, downedNow)` while he's alive. It returns 1 when he's himself again: give him back his animation and call `HR_BodyFree`.
   - `HR_Settle(b, grabbed, origin)`.
   - `HR_Frame(b, origin, grabbed, grabPoint, limbsOff, headOff)`. It returns 1 when there's a new pose to skin. `limbsOff` and `headOff` come from the game's dismemberment.
6. **Step the physics:** `HRP_Step(seconds, gravity)` in fixed steps. RTCW uses 8 ms steps, two per 16 ms frame (`vr_realWeights 1`), and a time budget. See `CG_Physics_Frame` in `cg_physworld.c`.
7. **Events change the game:**
   - `HR_EV_FELL`: the stagger shoved him over, so knock him down in game logic and stand him up later.
   - `HR_EV_POSITION`: his body staggered here, so move his game body (collision, AI) there.
   - `HR_EV_HOLD`: the game's stun is over but he's still reeling, so hold him a moment.
   - `HR_EV_GETUP_YAW`: the way he'll face when he's up.
8. **Skin the bones** from the parts (see Skinning). Free bodies with `HR_BodyFree` when the corpse goes away, and call `HRP_Shutdown` on map change.

Up to 1024 bodies can exist at once. `maxActive` (12) limits how many fight at once, and the rest are plain ragdolls.

## Soldier of Fortune II

Same units and axes as RTCW, so `HR_SetHostSpace(1, 0)` and every tuning number carries over unchanged. The work is in the game-side glue.

- **Which code to change.** The multiplayer SDK ships the game, cgame and ui source, so it's the practical first target. The ragdolls belong in cgame, as in RTCW.
  Single player was released as binaries only. Our reverse-engineering notes are in `/mnt/project-files/source/sof2-sp-re/`, and SP would mean hooking those DLLs. Do MP first.
- **Skeleton (Ghoul2).** SoF2 characters are Ghoul2 models. Read world bone matrices with the `G2API_GetBoltMatrix` family (cgame traps), mapping the 21 points to the skeleton's bone names as `rdPointBones` does for RTCW's Biped.
  Pose the ragdoll by overriding bones with the Ghoul2 bone-angle/matrix override calls (`G2API_SetBoneAnglesMatrix` style, world or model space), one per skinned bone, every frame `HR_Frame` returns 1. Check the exact trap names and flags in the SDK's `cg_public.h`.
- **Level.** The maps are Raven BSP (`RBSP`, version 1), close to Quake 3's IBSP. The brush, plane and model lumps used by `cg_physworld.c` are the same idea, but a brush side has a third int (`drawSurfNum`, 12 bytes instead of 8), and the surface and lightmap lumps differ.
  Port `PW_Build` with those structs, keeping `CONTENTS_SOLID | CONTENTS_PLAYERCLIP`. Movers are `ET_MOVER` entities with inline models, as in RTCW.
- **Hits.** SoF2 already sends hit locations (Ghoul2 hit zones) for its gore. Feed the impact point and bullet direction to `HR_BodyShot`, and turn its dismemberment state into `limbsOff`/`headOff`.
  Death: start the body where cgame switches the player model to its death animation, and pass the killer direction as `push`.
- **Stun, knockdown, events.** Plain SoF2 has no stun or knockdown state. Start with deaths only (`HR_Die`), which needs no server changes. Living reactions later need a small game-module change to carry a stunned / downed flag and to obey `HR_EV_POSITION`/`HR_EV_FELL`, the way RTCW's `vrdownpos`/`vrfell` commands do.

## Command & Conquer: Renegade

EA released Renegade's source under GPL v3 in February 2025. It is the W3D engine (WW3D2 for rendering, WWPhys for physics, and the game in `Combat/` and `Commando/`).

- **Space.** Metres, Z up: `HR_SetHostSpace(0.0254f, 0)`, and `hrOptions_t.gravity = 10.16f` (or the game's own gravity).
- **Skeleton.** Infantry are skinned W3D models on an `HTreeClass` hierarchy. Get bone world transforms from the render object (`Get_Bone_Transform`), and map the 21 points by bone name, after listing the commando's bones once.
  To pose the ragdoll, W3D render objects can take over bones from the animation (`Capture_Bone` / `Control_Bone` / `Release_Bone`, which the game uses for things like head turning). Drive every captured bone from the parts, using the Skinning formula.
- **Level.** Feed WWPhys's static collision meshes to `HRP_AddWorldTriangle` at level load (triangles, so no hulls are needed). Doors, elevators and vehicles become movers, placed each frame from their physics objects' transforms.
  Give `hrHost_t.trace` a WWPhys ray cast, so the get-up floor check sees everything.
- **Hits.** Soldiers take damage in the soldier and damageable game objects (`SoldierGameObj`, `DamageableGameObj::Apply_Damage`). Hit position and direction are available where bullets resolve.
  Renegade plays canned death animations. Replace that with `HR_Die` (plus `HR_BodyBlast` and `blasted` for explosive deaths, of which Renegade has plenty) on the client, cosmetic only, leaving the server's soldier logic alone.
- **First milestone.** Deaths only, on the client, no network changes. Then stagger-on-hit (`HR_Stun`), which also stays client side if `HR_EV_POSITION` is ignored, because the body returns to the game's position when he comes to.

These game notes are a plan written from the engines' structure. I haven't compiled against either game yet, so check the function names against the actual SDK / source before relying on them.

## Testing a port

- The library's own checks: `make test` (movers end where they are placed in both host spaces; parts of a body that start inside each other pass through instead of being thrown apart, in every ragdoll slot).
- The library's own behaviour: `make demo-run`, then render the GIF. You should see each man in the GIF behave as his label says.
- A port's behaviour: log every frame's bone positions, events and debug lines (`hrOptions_t.debug = 1`) and compare runs, as RealRTCW-VR's equivalence harness did.
  When you refactor the library, the same script on the same build must give byte-identical logs.
