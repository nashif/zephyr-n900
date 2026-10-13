import os
import re
import subprocess
import sys
import time

import serial


def zephyr_present():
    out = subprocess.run(['lsusb'], capture_output=True, text=True).stdout
    return '2fe3:' in out


def cmd(s, line, wait=0.6):
    s.reset_input_buffer()
    s.write((line + '\r').encode())
    time.sleep(wait)
    out = s.read(16384).decode(errors='replace')
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out)
    return '\n'.join(x for x in out.splitlines() if 'uart:~' not in x and x.strip() != line)


if '--now' not in sys.argv:
    t0 = time.time()
    while zephyr_present():
        time.sleep(0.2)
    while not zephyr_present() or not os.path.exists('/dev/ttyACM0'):
        if time.time() - t0 > 900:
            sys.exit("no new device")
        time.sleep(0.2)
    time.sleep(2.5)

s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.dtr = True
time.sleep(1.0)
s.read(16384)

bus = 'i2c@48072000'
print('--- bq27200 0x00-0x2f')
print(cmd(s, f'i2c read {bus} 0x55 0x00 0x30', 1.0))
print('--- bq27200 EEPROM 0x60-0x7f')
print(cmd(s, f'i2c read {bus} 0x55 0x60 0x20', 1.0))
print('--- TSL2563 control/timing/id, data (command bit set)')
print(cmd(s, f'i2c read {bus} 0x29 0x80 0x10', 1.0))
for i in range(3):
    print(cmd(s, f'i2c read {bus} 0x29 0xac 4', 1.0))
    time.sleep(0.5)
print('--- sensor get')
print(cmd(s, 'sensor get light-sensor@29', 1.5))
s.close()
