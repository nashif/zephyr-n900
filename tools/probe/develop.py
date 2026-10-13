import sys, numpy as np
from PIL import Image
a = np.load(sys.argv[1]).astype(np.float32) - 64.0      # (n, 2, W) GRBG
a = np.clip(a, 0, None)
g = (a[:, 0, 0::2] + a[:, 1, 1::2]) / 2; r = a[:, 0, 1::2]; b = a[:, 1, 0::2]
rgb = np.stack([r, g, b], -1)
rgb *= rgb[..., 1].mean() / rgb.reshape(-1, 3).mean(0)   # gray world
rgb = rgb / np.percentile(rgb, 99.5)
rgb = np.clip(rgb, 0, 1) ** (1 / 2.2) * 255
img = Image.fromarray(rgb.astype(np.uint8))
W = 2592; H = 1968
img = img.resize((648, int(648 * H / W)), Image.BILINEAR)
img.save(sys.argv[2]); print('saved', img.size)
