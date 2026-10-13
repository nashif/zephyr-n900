"""Private experiment: write the Linux et8ek8 power-on list (parsed from ~/linux at
run time, not copied) over the USB shell, stream on, and watch the CCP2 receiver."""
import os, re, sys, time
sys.argv = [sys.argv[0], 'none']
exec(open(os.path.join(os.path.dirname(__file__), 'ccp2.py')).read().split("step = sys.argv")[0])

src = open(os.path.expanduser('~/linux/drivers/media/i2c/et8ek8/et8ek8_mode.c')).read()
block = re.search(r'ET8EK8_REGLIST_POWERON.*?ET8EK8_REG_TERM', src, re.S).group(0)
regs = [(int(a, 16), int(v, 16)) for a, v in re.findall(r'ET8EK8_REG_8BIT,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)', block)]
print(f'{len(regs)} power-on writes', flush=True)

# Sensor reset: GPIO 102 low, high
wr(0x49054090, 0x40); time.sleep(0.05); wr(0x49054094, 0x40); time.sleep(0.05)
fails = 0
for a, v in regs:
    o = sh(f'i2c write {I2C3} 0x3e 0x{a >> 8:02x} 0x{a & 0xff:02x} 0x{v:02x}', 0.3)
    fails += 'Failed' in o
print(f'written, {fails} NACKed', flush=True)
print('readback 0x1252:', sensor_rd(0x1252), ' 0x1200:', sensor_rd(0x1200, 4), ' 0x0000:', sensor_rd(0x0000, 8))
wr(LC01_IRQSTATUS, 0xFFFFFFFF)
sensor_wr(0x1252, 0xb0)
for i in range(8):
    time.sleep(0.4)
    v = rd(LC01_IRQSTATUS)
    print(f'poll {i}: LC01_IRQSTATUS {v:08x}  GNQ {rd(GNQ):08x}  DBG {rd(DBG):08x}', flush=True)
    wr(LC01_IRQSTATUS, 0xFFFFFFFF)
print('done', flush=True)
