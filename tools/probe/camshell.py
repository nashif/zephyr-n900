"""Camera bring-up over the USB shell: power, clocks, xclk check, probe."""
import os, re, subprocess, sys, time, serial

def present(): return '2fe3:' in subprocess.run(['lsusb'], capture_output=True, text=True).stdout

if '--now' not in sys.argv:
    t0 = time.time()
    while present(): time.sleep(0.2)
    while not present() or not os.path.exists('/dev/ttyACM0'):
        if time.time() - t0 > 900: sys.exit('no device')
        time.sleep(0.2)
    time.sleep(2.5)

s = serial.Serial('/dev/ttyACM0', 115200, timeout=1); s.dtr = True; time.sleep(1); s.read(65536)

def sh(line, wait=0.4):
    s.reset_input_buffer(); s.write((line + '\r').encode()); time.sleep(wait)
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
    return '\n'.join(x for x in out.splitlines() if 'uart:~' not in x and x.strip() and x.strip() != line).strip()

def rd(addr, w=32):
    o = sh(f'devmem 0x{addr:08x} {w}')
    m = re.search(r'0x([0-9a-fA-F]+)\s*$', o)
    return int(m.group(1), 16) if m else None

def wr(addr, val, w=32):
    sh(f'devmem 0x{addr:08x} {w} 0x{val:x}')

def show(name, addr, w=32):
    v = rd(addr, w)
    print(f'{name:24s} {addr:08x} = ' + (f'{v:0{w//4}x}' if v is not None else 'ERR'), flush=True)
    return v

I2C1, I2C3 = 'i2c@48070000', 'i2c@48060000'
def scan(tag): print(f'scan {tag}:', ' '.join(re.findall(r'\b[0-7][0-9a-f]\b', sh(f'i2c scan {I2C3}', 2.0).split('devices')[0])) , flush=True)
def scan_raw(tag): print(f'scan {tag}:\n' + sh(f'i2c scan {I2C3}', 2.0), flush=True)

PAD_XCLKA, PAD_XCLKB, PAD_D3, PAD_I2C3_SCL = 0x48002110, 0x4800212E, 0x4800211C, 0x480021C2
CM_FCLKEN_CAM, CM_ICLKEN_CAM, CM_IDLEST_CAM = 0x48004F00, 0x48004F10, 0x48004F20
CM_CLKSEL_CAM, CM_CLKSTCTRL_CAM = 0x48004F40, 0x48004F48
CM_CLKEN_PLL, CM_IDLEST_CKGEN, CM_CLKSEL2_PLL = 0x48004D00, 0x48004D20, 0x48004D44
PM_PWSTCTRL_CAM, PM_PWSTST_CAM = 0x48306FE0, 0x48306FE4
PM_WKEN_WKUP, PM_WKST_WKUP = 0x48306CA0, 0x48306CB0
ISP = 0x480BC000; ISP_SYSCONFIG, ISP_TCTRL = ISP + 4, ISP + 0x50
GPIO4 = 0x49054000

print('=== state left by NOLO'); scan_raw('unpowered')
for n, a, w in [('pad xclka', PAD_XCLKA, 16), ('pad xclkb', PAD_XCLKB, 16), ('pad cam_d3/gpio102', PAD_D3, 16),
                ('pad i2c3_scl', PAD_I2C3_SCL, 16), ('CM_FCLKEN_CAM', CM_FCLKEN_CAM, 32), ('CM_ICLKEN_CAM', CM_ICLKEN_CAM, 32),
                ('CM_CLKSEL_CAM', CM_CLKSEL_CAM, 32), ('CM_CLKSTCTRL_CAM', CM_CLKSTCTRL_CAM, 32), ('CM_CLKEN_PLL', CM_CLKEN_PLL, 32),
                ('CM_IDLEST_CKGEN', CM_IDLEST_CKGEN, 32), ('CM_CLKSEL2_PLL', CM_CLKSEL2_PLL, 32), ('PM_PWSTST_CAM', PM_PWSTST_CAM, 32),
                ('PM_WKEN_WKUP', PM_WKEN_WKUP, 32), ('PM_WKST_WKUP', PM_WKST_WKUP, 32)]:
    show(n, a, w)
print(sh(f'i2c read {I2C1} 0x4b 0x76 12'))

print('=== power and clocks')
sh(f'i2c write_byte {I2C1} 0x4b 0x81 0x09'); sh(f'i2c write_byte {I2C1} 0x4b 0x7e 0x20')
print('VAUX4', sh(f'i2c read {I2C1} 0x4b 0x7e 4'))
wr(PM_PWSTCTRL_CAM, (rd(PM_PWSTCTRL_CAM) & ~3) | 3)
wr(CM_CLKSTCTRL_CAM, 2)
wr(CM_ICLKEN_CAM, 1); wr(CM_FCLKEN_CAM, 3)
wr(CM_CLKEN_PLL, rd(CM_CLKEN_PLL) & ~(1 << 30))
wr(CM_CLKSEL_CAM, (rd(CM_CLKSEL_CAM) & ~0x1F) | 5)
time.sleep(0.05)
for n, a in [('PM_PWSTST_CAM', PM_PWSTST_CAM), ('CM_IDLEST_CAM', CM_IDLEST_CAM), ('CM_CLKEN_PLL', CM_CLKEN_PLL),
             ('CM_CLKSEL_CAM', CM_CLKSEL_CAM), ('ISP_REVISION', ISP), ('ISP_SYSCONFIG', ISP_SYSCONFIG)]:
    show(n, a)
wr(ISP_SYSCONFIG, 1 << 12)
wr(ISP_TCTRL, (rd(ISP_TCTRL) & ~0x1F) | 18); show('ISP_TCTRL', ISP_TCTRL)
wr(PAD_XCLKA, 0x0000, 16); wr(PAD_D3, 0x0004, 16)
wr(GPIO4 + 0x34, rd(GPIO4 + 0x34) & ~(1 << 6))
wr(GPIO4 + 0x90, 1 << 6); time.sleep(0.02); wr(GPIO4 + 0x94, 1 << 6); time.sleep(0.02)
show('GPIO4 DATAOUT', GPIO4 + 0x3C)
scan_raw('powered, xclka 9.6 MHz')

print('=== pad wake-up event check')
wr(PM_WKEN_WKUP, rd(PM_WKEN_WKUP) | (1 << 8) | (1 << 16)); time.sleep(0.01)
show('PM_WKEN_WKUP', PM_WKEN_WKUP); show('PM_WKST_WKUP', PM_WKST_WKUP)
scl = rd(PAD_I2C3_SCL, 16)
wr(PAD_I2C3_SCL, (scl | 0x4000) & 0x7FFF, 16); show('i2c3_scl before scan', PAD_I2C3_SCL, 16)
sh(f'i2c scan {I2C3}', 2.0); show('i2c3_scl after scan', PAD_I2C3_SCL, 16)
wr(PAD_I2C3_SCL, scl & 0x3FFF, 16)
for diva in (0, 1, 0, 18, 31):
    wr(ISP_TCTRL, (rd(ISP_TCTRL) & ~0x1F) | diva)
    wr(PAD_XCLKA, 0x4100, 16)   # mode 0, input buffer, wake-up enable; clears the event
    time.sleep(0.05)
    show(f'xclka DIVA {diva:2d}', PAD_XCLKA, 16)
wr(ISP_TCTRL, (rd(ISP_TCTRL) & ~0x1F) | 18); wr(PAD_XCLKA, 0x0000, 16)
show('PM_WKST_WKUP', PM_WKST_WKUP)
wr(PM_WKEN_WKUP, rd(PM_WKEN_WKUP) & ~((1 << 8) | (1 << 16)))
print('done', flush=True)
