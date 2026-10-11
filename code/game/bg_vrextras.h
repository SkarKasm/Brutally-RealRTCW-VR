/*
===========================================================================

RealRTCW-VR
Copyright (C) 2026 RealRTCW-VR contributors

This file is part of RealRTCW-VR, a VR port of RealRTCW.

RealRTCW-VR is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// bg_vrextras.h -- the optional mechanics of GORE & EXTRAS, shared by game and cgame
//
// The game (g_vrextras.c) reads one cvar per setting and publishes them all in the
// serverinfo cvar g_vrx, a digit each, so every player's cgame does what the host chose.

#ifndef BG_VREXTRAS_H
#define BG_VREXTRAS_H

typedef enum {
	VRX_GORE,           // master switch for everything gory (0 off, 1 on)
	VRX_REACTIONS,      // shot reactions: 0 off, 1 normal, 2 strong
	VRX_WOUNDS,         // bullet wound decals on bodies and the living
	VRX_THROAT,         // throat slitting and disembowelment (killing blows)
	VRX_PROPGRAB,       // grab and throw props
	VRX_PROPHIT,        // thrown props: 0 nothing, 1 stun, 2 stun and damage
	VRX_DISMEMBER,      // 0 off, 1 explosions, 2 explosions and heavy weapons
	VRX_DECAP,          // knife decapitation of the dead
	VRX_HEADSCARE,      // thrown heads: 0 nothing, 1 scare, 2 scare and surrender
	VRX_STOCKGIBS,      // RTCW's own gibbing of soldiers (0 off: bodies stay for our gore)
	VRX_COUNT
} vrExtra_t;

#define VRX_DEFAULTS    "1111122120"     // a digit per setting, in that order

// EV_VR_GORE (eventParm): what happened to the body otherEntityNum, the blow along origin2
typedef enum {
	VRG_NONE,
	VRG_THROAT,         // throat cut
	VRG_BELLY,          // disembowelled
	VRG_DECAP,          // the head cut off a dead body
	VRG_LIMBS,          // limbs off (s.density the new VRX_GB_ bits, s.frame those cut high)
	VRG_HEADPOP,        // the head blown apart by a shot (or stamped on), at origin
	VRG_COUNT
} vrGore_t;

// our own entities, told apart by s.dmgFlags (ET_GENERAL); a gore marker's s.frame has the
// limbs cut high (at the shoulder / hip rather than the elbow / knee)
#define VRX_ENT_GORE    0x56524752      // a body's gore marker: s.otherEntityNum2 the body, s.density VRX_GB_*
#define VRX_ENT_HEAD    0x56524844      // a cut off head: s.otherEntityNum2 the body it came off

// gore marker bits
#define VRX_GB_HEAD         1       // head off
#define VRX_GB_LARM         2       // limbs off (dismemberment)
#define VRX_GB_RARM         4
#define VRX_GB_LLEG         8
#define VRX_GB_RLEG         16
#define VRX_GB_SCARED       32      // alive, running from a thrown head
#define VRX_GB_SURRENDER    64      // alive, weapon dropped, hands up
#define VRX_GB_CRIPPLED     128     // alive with an arm shot off: unarmed, running, bleeding out
#define VRX_GB_DOWNED       512     // alive, shot down to their knees: slumped, bleeding out, waiting to be finished
#define VRX_GB_BLOWN        256     // killed by a blast close by: torn open (skinned head, ribs, guts)

// the crypt's torn soldier pieces (z_vr_assets.pk3, cut from crypt1.bsp), and who can wear them:
// men in uniform or plain clothes, not women, monsters or the special ones
// (co-op: vanilla RTCW's characters, no Russians, mercenaries or trench soldiers)
#define VRX_HUMAN_MALE( c ) ( ( c ) == AICHAR_SOLDIER || ( c ) == AICHAR_AMERICAN || ( c ) == AICHAR_VENOM || \
							  ( c ) == AICHAR_BLACKGUARD || ( c ) == AICHAR_PARTISAN )

// the body's own skeleton, for hits only where the body really is (cgame CG_GetTag
// "vrcaps:N" gives points 4N..4N+3, in or.origin then or.axis[0..2], world space)
typedef enum {
	VRB_PELVIS, VRB_SPINE2, VRB_NECK, VRB_HEAD,
	VRB_LUA, VRB_LFA, VRB_LHAND,
	VRB_RUA, VRB_RFA, VRB_RHAND,
	VRB_LTHIGH, VRB_LCALF, VRB_LFOOT, VRB_LTOE,
	VRB_RTHIGH, VRB_RCALF, VRB_RFOOT, VRB_RTOE,
	VRB_COUNT
} vrBodyPoint_t;

#define VRX_DOWN_STYLES 8     // ways to go down: the SoF2 agony animations (cgame cg_vrsof2anims.h)
#define VRX_GROIN_MS 2800     // how long a shot in the balls doubles someone over
#define VRB_GROUPS      ( ( VRB_COUNT + 3 ) / 4 )

// where a shot met someone
typedef enum {
	VRH_NONE,
	VRH_HEAD,
	VRH_CHEST,
	VRH_BELLY,
	VRH_GROIN,
	VRH_LARM,
	VRH_RARM,
	VRH_LLEG,
	VRH_RLEG,
	VRH_COUNT
} vrHitRegion_t;

// body parts soaked in blood (cgame, vr_bodyblood: two bits each in the overlay's shaderRGBA)
typedef enum {
	VRW_REGION_CHEST,
	VRW_REGION_BELLY,
	VRW_REGION_LARM,
	VRW_REGION_RARM,
	VRW_REGION_LLEG,
	VRW_REGION_RLEG,
	VRW_REGION_COUNT
} vrWoundRegion_t;


#endif
