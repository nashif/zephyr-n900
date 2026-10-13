import os, re, subprocess, sys, time, serial
def present(): return '2fe3:' in subprocess.run(['lsusb'], capture_output=True, text=True).stdout
if '--now' not in sys.argv:
    t0 = time.time()
    while present(): time.sleep(0.2)
    while not present() or not os.path.exists('/dev/ttyACM0'):
        if time.time() - t0 > 900: sys.exit('no device')
        time.sleep(0.2)
    time.sleep(2.5)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3); s.dtr = True; time.sleep(1); s.read(65536)
B = 'i2c@48070000'
def sh(l, w=0.6):
    s.read(65536); s.write((l + '\r').encode()); time.sleep(w)
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
def rd(a, r):
    m = re.search(r'00000000: ([0-9a-f]{2})', sh(f'i2c read {B} 0x{a:02x} 0x{r:02x} 1')); return int(m.group(1), 16) if m else None
def wr(a, r, v): sh(f'i2c write_byte {B} 0x{a:02x} 0x{r:02x} 0x{v:02x}')
leden, mode, apll, ctl = rd(0x4a, 0xee), rd(0x49, 0x01), rd(0x49, 0x3a), rd(0x49, 0x45)
print(f'LEDEN {leden:02x} CODEC_MODE {mode:02x} APLL_CTL {apll:02x} VIBRA_CTL {ctl:02x}', flush=True)
wr(0x4a, 0xee, leden & ~0x03)
wr(0x49, 0x01, mode | 0x02)
wr(0x49, 0x3a, (apll & ~0x1f) | 0x10 | 0x05)
wr(0x49, 0x46, 256 - 200)
print('buzz', flush=True)
wr(0x49, 0x45, (ctl & ~0x33) | 0x01)
time.sleep(1.5)
wr(0x49, 0x45, ctl & ~0x01)
print(f'after: CODEC_MODE {rd(0x49, 0x01):02x} APLL_CTL {rd(0x49, 0x3a):02x} VIBRA_CTL {rd(0x49, 0x45):02x} SET {rd(0x49, 0x46):02x}', flush=True)
