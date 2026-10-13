"""CCP2 receiver bring-up over the USB shell; sensor stream on; watch LC0 events."""
import re, sys, time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3); s.dtr = True; time.sleep(1); s.read(65536)
def sh(line, wait=0.35):
    s.read(65536); s.write((line + '\r').encode()); time.sleep(wait)
    return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
def rd(a, w=32):
    m = re.search(r'Read value 0x([0-9a-fA-F]+)', sh(f'devmem 0x{a:08x} {w}')); return int(m.group(1), 16) if m else None
def wr(a, v, w=32): sh(f'devmem 0x{a:08x} {w} 0x{v:x}')
def show(n, a, w=32):
    v = rd(a, w); print(f'{n:22s} {a:08x} = ' + (f'{v:08x}' if v is not None else 'ERR'), flush=True); return v
I2C3 = 'i2c@48060000'
def sensor_wr(reg, val):
    o = sh(f'i2c write {I2C3} 0x3e 0x{reg >> 8:02x} 0x{reg & 0xff:02x} 0x{val:02x}', 0.5)
    print(f'sensor {reg:04x} <- {val:02x}', 'FAIL' if 'Failed' in o else 'ok', flush=True)
def sensor_rd(reg, n=1):
    sh(f'i2c write {I2C3} 0x3e 0x{reg >> 8:02x} 0x{reg & 0xff:02x}', 0.5)
    o = sh(f'i2c direct_read {I2C3} 0x3e {n}', 0.6)
    m = re.search(r'00000000: ([0-9a-f ]+)', o); return m.group(1).strip() if m else 'FAIL'

CSIRXFE = 0x480022DC
CCP2 = 0x480BC400
SYSCONFIG, SYSSTATUS = CCP2 + 0x04, CCP2 + 0x08
LC01_IRQENABLE, LC01_IRQSTATUS = CCP2 + 0x0C, CCP2 + 0x10
LCM_IRQSTATUS = CCP2 + 0x30
CTRL, DBG, GNQ = CCP2 + 0x40, CCP2 + 0x44, CCP2 + 0x48
LC0_CTRL, LC0_CODE, LC0_STAT_START, LC0_STAT_SIZE = CCP2 + 0x50, CCP2 + 0x54, CCP2 + 0x58, CCP2 + 0x5C
LC0_DAT_START, LC0_DAT_SIZE = CCP2 + 0x68, CCP2 + 0x6C

step = sys.argv[1] if len(sys.argv) > 1 else 'all'
if step in ('all', 'setup'):
    print('sensor 0x0000:', sensor_rd(0x0000, 8))
    show('CCP2 REVISION', CCP2)
    for pad in (0x48002122, 0x48002124, 0x48002126, 0x48002128):
        print(f'pad {pad:08x} was {rd(pad, 16):04x}', flush=True); wr(pad, 0x0100, 16)
    show('CSIRXFE before', CSIRXFE); wr(CSIRXFE, 0x3400); show('CSIRXFE', CSIRXFE)
    wr(SYSCONFIG, 0x2); time.sleep(0.05); show('SYSSTATUS', SYSSTATUS)
    wr(SYSCONFIG, 1 << 12); show('SYSCONFIG', SYSCONFIG)
    # CCP2 mode, data/strobe PHY, CCP2 output select
    wr(CTRL, (1 << 4) | (1 << 2) | (1 << 1)); show('CTRL', CTRL)
    # LC0: RAW10 + VP format (0x16), CRC on, region on
    wr(LC0_CTRL, (0x16 << 3) | (1 << 2) | (1 << 1)); show('LC0_CTRL', LC0_CTRL)
    wr(LC0_DAT_START, 0); wr(LC0_DAT_SIZE, 1968 << 16)
    wr(LC01_IRQSTATUS, 0xFFFFFFFF); wr(LC01_IRQENABLE, 0xFFF)
    wr(LC0_CTRL, rd(LC0_CTRL) | 1); wr(CTRL, rd(CTRL) | 1)
    show('CTRL enabled', CTRL); show('LC0_CTRL enabled', LC0_CTRL)
    show('LC01_IRQSTATUS idle', LC01_IRQSTATUS)
    sensor_wr(0x1252, 0xb0)
if step in ('all', 'poll'):
    for i in range(6):
        time.sleep(0.5)
        v = rd(LC01_IRQSTATUS)
        print(f'poll {i}: LC01_IRQSTATUS {v:08x}  GNQ {rd(GNQ):08x}  DBG {rd(DBG):08x}', flush=True)
        wr(LC01_IRQSTATUS, 0xFFFFFFFF)
    print('sensor 0x1252:', sensor_rd(0x1252), ' 0x1200:', sensor_rd(0x1200, 4))
if step == 'stop':
    sensor_wr(0x1252, 0x30); wr(CTRL, rd(CTRL) & ~1)
print('done', flush=True)
