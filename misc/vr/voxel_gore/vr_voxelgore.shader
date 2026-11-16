// RealRTCW-VR voxel gore (cgame cg_vrgore.c, renderer tr_animation.c R_VRG_Flesh): the wound
// walls inside a body's bullet holes. Lit as the body is (so it never glows in the dark), then
// the colour the cgame gives each vertex by how deep it is (raw flesh, dark muscle, bone,
// brains), then a little meaty texture: neck_meat.tga made grey (make_voxelgore_pk3.py).
vr_goreflesh
{
	nopicmip
	cull none
	{
		map $whiteimage
		rgbGen lightingDiffuse
	}
	{
		map $whiteimage
		blendFunc GL_DST_COLOR GL_ZERO
		rgbGen vertex
	}
	{
		map models/vr_gore/flesh_detail.tga
		blendFunc GL_DST_COLOR GL_SRC_COLOR
		tcGen vector ( 0.2 0 0.09 ) ( 0 0.2 0.13 )
		rgbGen identity
	}
}

// the inside of a wounded body's skin (cg_vrgore.c, the body drawn again with its holes cut): only
// the faces turned away from the eye (as vr_neckinside_body), so through a hole the inside of the
// body shows meat, never the world behind it, wherever the wound walls don't cover it
vr_goreinside
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

// the blood round the voxel gore's wounds (cg_vrgore.c VRG_BloodHoles, laid on the skin by
// tr_animation.c R_VRX_Decals): a stain soaking out round each hole, and blood running down
// from it. Both cut from RTCW's own bodyblood.tga (make_voxelgore_pk3.py).
vr_gorebloodsoak
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/bloodsoak.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

vr_gorebloodtrail
{
	nopicmip
	nomipmaps
	polygonOffset
	{
		clampmap models/vr_gore/bloodtrail.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

// blood in the air as a solid rope (cg_vrblood.c VRB_Rope): the cgame gives each corner its
// colour, lit from above
vr_bloodrope
{
	nopicmip
	cull none
	{
		map $whiteimage
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

// a drop of blood that flew into someone, where it hit (cg_vrextras.c VRX_SplatAdd). Its name
// has to start vr_goreblood, as the others': that is what tells the renderer to lay it on the
// skin (as vr_bloodsplat the whole body was drawn in it: the same red shapes on everyone)
vr_gorebloodsplat
{
	nopicmip
	nomipmaps
	polygonOffset
	{
		clampmap models/vr_gore/bloodsplat.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

vr_gorebloodrun
{
	nopicmip
	nomipmaps
	polygonOffset
	{
		clampmap models/vr_gore/bloodrun.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}

// all of the above (and the rim splash round a hole, vr_bodywound) as one draw: every mark
// names its picture in the atlas (tr_animation.c vrxDecalCell, make_bloodatlas.py). Two
// textures of the one layout, each picture in one of them and the other clear there: the
// stains are mipmapped and the thin lines (runs, streaks) are not, as each always was
// (nomipmaps counts for the textures named after it).
vr_gorebloodatlas
{
	nopicmip
	polygonOffset
	{
		clampmap models/vr_gore/bloodatlas.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
	nomipmaps
	{
		clampmap models/vr_gore/bloodlines.tga
		blendFunc blend
		rgbGen lightingDiffuse
	}
}
