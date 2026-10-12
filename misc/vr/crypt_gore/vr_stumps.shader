// RealRTCW-VR: Soldier of Fortune 2's bloody stumps (Raven Software), cut out of SoF2 by
// sof2_stumps.py into z_vr_stumps.pk3; cg_vrextras.c VRX_AddSofStump, CG_VRX_HeadClip.

// the torn flesh closing a stump, and where the jaw was shot off (SoF2's cap_*_off surfaces,
// models/characters/caps/caps as its own shader has it, seen from both sides)
vr_sof2cap
{
	cull none
	nopicmip
	{
		map models/vr_gore/sof2_caps.tga
		rgbGen lightingDiffuse
	}
}

// the bone sticking out of a stump (SoF2's models/characters/gore/bone_small, bone_long.png)
vr_sof2bone
{
	cull none
	nopicmip
	{
		map models/vr_gore/sof2_bone.tga
		alphaFunc GE128
		rgbGen lightingDiffuse
	}
}

// the inside of a cut up body, limb or head (drawn again with this shader, which draws only the
// faces turned away from the eye, so where the cut leaves it open it's meat, not hollow):
// SoF2's cap meat (sof2_meat.tga, out of caps.jpg, mirrored so it tiles) laid across it,
// skewed so it never stretches along an axis. Sorted ahead of the body itself, like
// vr_neckinside_body.
vr_stumpinside
{
	cull back
	nopicmip
	sort 2
	{
		map models/vr_gore/sof2_meat.tga
		tcGen vector ( 0.16 0 0.07 ) ( 0 0.16 0.1 )
		rgbGen lightingDiffuse
	}
}
