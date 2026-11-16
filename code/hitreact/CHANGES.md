# HitReact changes

## 2026-11-11b (review fixes)

Behaviour:
- **Parts that start inside each other.** A ragdoll's collision group was its slot (0-based) while the
  contact listener looked it up as slot + 1. The first ragdoll never got its pass-through (its
  overlapping parts were thrown apart at once), and every other one used its neighbour's table.
  The group is now slot + 1. This is the one change that alters RealRTCW's recorded output; with the
  same one-line fix put into the pre-HitReact code, old and new are byte-identical again.
- **Movers ran on past where they were placed.** A kinematic body keeps its speed, so with more than
  one step per frame (8 ms steps) a moving door or lift overshot by up to a frame's travel and snapped
  back. HRP_Step now stops a mover once it has arrived; teleport and hide clear its speed too.
- Muscles switched off mid-stun (options, or the head gone) left the stun's safety net and pin on.
- HR_Die with no hit point used the pelvis, so every trunk hit read as a gut shot. It now uses the
  middle of the part hit.

Robustness (no change for RealRTCW):
- HR_TraceLine read an uninitialised end point when a host trace callback didn't write one.
- RD_EuLimb copied from a frame that may not have been built (the value was never used).
- HRP_AddWorldHull / HRP_MoverAddHull with fewer than 4 points or NULL return early.
- HR_GetPoint checks its index. hr_physics.cpp includes <cstring> itself.
- hitreact.h says an hrBody_t must be zeroed before first use and freed with HR_BodyFree.
- `make test`: test/hr_test_movers.c and test/hr_test_overlap.c.

Known, left as tuned:
- In RD_EuFrame the "shot limb is softer" test (`limb == rd->euHurtLimb`) is also true for the chest
  and head when no limb is hurt (both 0), so on trunk shots the spine and neck run at 0.6 of their
  tone while staggering and 0.4 lying. The reactions were tuned with this in place.
