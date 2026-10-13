import glob
import os
import subprocess
import sys
import time

import serial


def zephyr_present():
    out = subprocess.run(['lsusb'], capture_output=True, text=True).stdout
    return '2fe3:' in out


t0 = time.time()
# Wait for the running image to go away, then for the new one
while zephyr_present():
    time.sleep(0.2)
while not zephyr_present() or not os.path.exists('/dev/ttyACM0'):
    if time.time() - t0 > 900:
        sys.exit("no new device")
    time.sleep(0.2)
time.sleep(1.5)

for d in glob.glob('/sys/bus/usb/devices/*'):
    try:
        if open(os.path.join(d, 'idVendor')).read().strip() == '2fe3':
            serial_path = os.path.join(d, 'serial')
            sn = open(serial_path).read().strip() if os.path.exists(serial_path) else '(none)'
            print('USB serial number:', sn)
    except OSError:
        pass

s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.dtr = True
time.sleep(1.0)
s.read(8192)
for cmd in ['hwinfo devid', 'hwinfo reset_cause', 'hwinfo reset_cause supported']:
    s.write((cmd + '\r').encode())
    time.sleep(1.0)
    print(s.read(8192).decode(errors='replace'))
s.close()
