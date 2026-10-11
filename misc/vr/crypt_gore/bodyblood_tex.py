# bodyblood.tga for vr_bodyblood (vr_gore.shader): blood soaked into clothes and skin, tiling.
# The alpha is a noise ranked evenly 0..255: the shader cuts it at 128 after the renderer has
# scaled it by how hurt that part of the body is (tr_animation.c R_VRX_BodyBlood), so the
# stains spread as the part takes more hits.
# python3 bodyblood_tex.py out.tga
import struct, sys
import numpy as np

N = 256
rng = np.random.default_rng(11)

def noise(cut, power):
    f = np.fft.fftfreq(N) * N
    fx, fy = np.meshgrid(f, f)
    r = np.hypot(fx, fy)
    amp = np.where(r > 0, 1.0 / np.maximum(r, 1) ** power, 0) * np.exp(-(r / cut) ** 2)
    spec = amp * np.exp(2j * np.pi * rng.random((N, N)))
    n = np.real(np.fft.ifft2(spec))
    return (n - n.mean()) / n.std()

blob = noise(10, 1.2) + 0.35 * noise(40, 0.8)
rank = blob.ravel().argsort().argsort().reshape(N, N) / (N * N - 1)
alpha = (rank * 255).astype(np.uint8)
# darker where it's thickest, wetter red toward the edges
depth = np.clip((rank - 0.5) * 2, 0, 1)
grain = noise(80, 0.5) * 0.04
r = np.clip(0.42 - 0.22 * depth + grain, 0, 1)
g = np.clip(0.03 - 0.02 * depth + grain * 0.2, 0, 1)
b = np.clip(0.03 - 0.02 * depth + grain * 0.2, 0, 1)
pix = np.dstack([b, g, r]) * 255
pix = np.dstack([pix.astype(np.uint8), alpha])
out = sys.argv[1] if len(sys.argv) > 1 else 'bodyblood.tga'
with open(out, 'wb') as fh:
    fh.write(struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, N, N, 32, 8))
    fh.write(pix[::-1].tobytes())
