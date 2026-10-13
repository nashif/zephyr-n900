"""Freeze one RAW10 frame from the CCP2 memory port and pull a subsampled copy."""
import os, re, sys, time
step = int(sys.argv[1]) if len(sys.argv) > 1 else 32
FMT = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x15
out = sys.argv[2] if len(sys.argv) > 2 else 'frame.png'
sys.argv = [sys.argv[0], 'none']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ccp2.py')).read().split("step = sys.argv")[0])
import numpy as np
from PIL import Image
W, H, STRIDE, PING = 2592, 1968, 2592 * 2, 0x8A000000
LC0_PING, LC0_PONG, LC0_OFST = CCP2 + 0x70, CCP2 + 0x74, CCP2 + 0x78
wr(LC0_CTRL, rd(LC0_CTRL) & ~1)
wr(LC0_CTRL, (FMT << 3) | (1 << 2) | (1 << 1))
wr(LC0_PING, PING); wr(LC0_PONG, PING); wr(LC0_OFST, STRIDE)
wr(LC0_DAT_START, 0); wr(LC0_DAT_SIZE, H << 16)
wr(LC0_CTRL, rd(LC0_CTRL) | 1); time.sleep(0.5)
wr(LC0_CTRL, rd(LC0_CTRL) & ~1)   # freeze
def dump(addr, size):
    s.read(1 << 20); s.write(f'devmem dump -a 0x{addr:x} -s {size} -w 32\r'.encode())
    buf, t0 = b'', time.time()
    while time.time() - t0 < 30:
        chunk = s.read(1 << 16); buf += chunk
        if b'uart:~$' in buf[-200:] and buf.count(b'\n') > size // 16: break
    data = bytearray()
    for line in buf.decode(errors='replace').splitlines():
        m = re.match(r'\s*[0-9a-fA-F]{8}:\s+((?:[0-9a-fA-F]{2}\s+){1,16})', line)
        if m: data += bytes(int(b, 16) for b in m.group(1).split())
    return bytes(data[:size])
t0 = time.time(); rows = []
for y in range(0, H - 1, step):
    pair = dump(PING + y * STRIDE, 2 * STRIDE)
    if len(pair) != 2 * STRIDE: print('short row', y, len(pair), flush=True); continue
    rows.append(np.frombuffer(pair, dtype='<u2').reshape(2, W))
print(f'{len(rows)} row pairs in {time.time() - t0:.0f} s', flush=True)
np.save(out + '.npy', np.array(rows)); a = np.array(rows).astype(np.float32)
g = (a[:, 0, 0::2] + a[:, 1, 1::2]) / 2; r = a[:, 0, 1::2]; b = a[:, 1, 0::2]
print('raw range', a.min(), a.max(), 'mean', a.mean(), flush=True)
rgb = np.stack([r, g, b], -1)
rgb = rgb / max(np.percentile(rgb, 99.5), 1) * 255
img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
img = img.resize((W // 4, int(W // 4 * H / W)))
img.save(out); print('saved', out, img.size, flush=True)
