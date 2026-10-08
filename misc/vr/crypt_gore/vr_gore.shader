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
