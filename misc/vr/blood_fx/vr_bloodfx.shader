// RealRTCW-VR vr_bloodFX (cgame cg_vrblood.c): blood in the air, drawn as polys the cgame
// shades itself (rgbGen vertex: it gives the colour and how faded). Both textures are cut from
// RTCW-VR's own bodyblood.tga (make_bloodfx_pk3.py), so it's the same blood as on the bodies.

// a stream of blood (an artery pumping, a wound running): a thin ribbon through its drops
vr_bloodstream
{
	nopicmip
	cull none
	{
		map models/vr_gore/bloodstream.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}

// one drop of it (mist, a splash, a drip), drawn out along the way it flies
vr_blooddrop
{
	nopicmip
	cull none
	{
		clampmap models/vr_gore/blooddrop.tga
		blendFunc blend
		rgbGen vertex
		alphaGen vertex
	}
}
