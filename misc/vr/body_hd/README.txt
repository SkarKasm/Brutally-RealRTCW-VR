HD VR body (vr_fullBody) for RealRTCW's "player" model (z_zperson.pk3).

body_default.jpg (512px) is upscaled 4x with OpenCV's EDSR_x4 model, then bodytex.py
replaces the sleeve panels with the flatscreen first person sleeves
(models/weapons/hands/sleeve_*.jpg: their colour and weave on the body's own folds,
keeping the flag and rank patches) and adds a fine weave to the rest of the uniform.
Outputs body_vr_{default,green,snow}.jpg and boot_vr.jpg (night uses the default images,
like the flatscreen hands). The smartskins pick them per map, the same lists as the
flatscreen hands; they go in z_vr_assets.pk3 under models/players/player and models/vr.
