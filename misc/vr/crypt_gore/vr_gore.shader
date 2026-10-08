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
