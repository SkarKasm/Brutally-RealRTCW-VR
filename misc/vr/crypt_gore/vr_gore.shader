// RealRTCW-VR: a bullet hole in a head, the blood running from it (models/vr_gore/headhole.md3)
vr_headhole
{
	cull none
	polygonOffset
	nopicmip
	{
		map models/vr_gore/headhole.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen lightingDiffuse
	}
}

// RealRTCW-VR: a flat cut through the neck, on cut off heads and their bodies (models/vr_gore/neck_flat.md3)
vr_neckflat
{
	cull none
	polygonOffset
	nopicmip
	{
		map models/vr_gore/neck_flat.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen lightingDiffuse
	}
}

// RealRTCW-VR: a shot-away jaw, on the head's own surface (the head is drawn again with this
// shader): jaw_wound.tga projected onto the front of the face from the head's frame (x the
// way the face looks, z up): s across the face, t down it, and further back the further off
// the texture it goes, so only the front of the face takes it
vr_jawwound
{
	nopicmip
	{
		clampmap models/vr_gore/jaw_wound.tga
		tcGen vector ( 0 0.142857 0 ) ( 0.2 0 -0.208333 )
		tcMod transform 1 0 0 1 0.5 -0.216667
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen lightingDiffuse
		depthFunc equal
	}
}

// RealRTCW-VR: the inside of a cut off head (the head is drawn again with this shader, which
// draws only the faces turned away from the eye, so the cut through the neck shows exactly
// where the head is open): neck_inside.tga projected up the head (x the way the face looks,
// y to its left) and centred on the neck, its meat running out to the edge of the opening.
// Sorted ahead of the head itself so the head's own two sided faces still win.
vr_neckinside_head
{
	cull back
	nopicmip
	sort 2
	{
		clampmap models/vr_gore/neck_inside.tga
		tcGen vector ( 0.25 0 0 ) ( 0 0.25 0 )
		tcMod transform 1 0 0 1 0.5 0.5
		rgbGen lightingDiffuse
	}
}

// RealRTCW-VR: the inside of a headless body (drawn again the same way): neck_meat.tga tiled
// across it, skewed so it never stretches along an axis
vr_neckinside_body
{
	cull back
	nopicmip
	sort 2
	{
		map models/vr_gore/neck_meat.tga
		tcGen vector ( 0.11 0 0.05 ) ( 0 0.11 0.07 )
		rgbGen lightingDiffuse
	}
}

// RealRTCW-VR: the blood spreading under a body (cg_vrextras.c CG_VRX_BloodPool, a mark),
// multiplied onto the floor so it takes the floor's own light; bloodpool.tga from bloodpool_tex.py
vr_bloodpool
{
	polygonOffset
	nopicmip
	{
		clampmap models/vr_gore/bloodpool.tga
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen identity
	}
}

// blood soaked into a body (cg_vrextras.c VRX_AddBodyBlood): the body drawn again over itself,
// the renderer setting each vertex's alpha by how hurt its part of the body is
vr_bodyblood
{
	nopicmip
	sort decal
	{
		map models/vr_gore/bodyblood.tga
		alphaFunc GE128
		depthFunc equal
		rgbGen lightingDiffuse
		alphaGen vertex
		tcMod scale 2 2
	}
}

// bullet holes on a body (cg_vrextras.c VRX_AddBodyWounds): the body's own triangles round each
// hole drawn again, the texture laid over them (tr_animation.c R_VRX_BodyWounds). The holes are
// Soldier of Fortune 2's own (Raven Software, models/characters/gore/gil_bullet_hole*), four of
// them, one shader each.
vr_bodywound
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/sof2_wound1.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

vr_bodywound2
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/sof2_wound2.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

vr_bodywound3
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/sof2_wound3.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

vr_bodywound4
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/sof2_wound4.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

// the guts out of an opened belly (models/vr_gore/guts.md3, sof2_guts.py): Soldier of Fortune 2's
// own (Raven Software, models/characters/gore/guts.md3, rib_cage.png), as its rib_cage shader
vr_sof2organs
{
	cull disable
	nopicmip
	{
		map models/vr_gore/sof2_organs.tga
		blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
		rgbGen lightingDiffuse
	}
}
