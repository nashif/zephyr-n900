import os
import subprocess
import sys
import time

import serial


def lsusb():
    return subprocess.run(['lsusb'], capture_output=True, text=True).stdout


t0 = time.time()
# The echo sample leaves, then a new Zephyr device with a serial port arrives
while 'CDC ACM sample' in lsusb():
    time.sleep(0.2)
while ('2fe3:' not in lsusb()) or not os.path.exists('/dev/ttyACM0'):
    if time.time() - t0 > 900:
        sys.exit("no new device")
    time.sleep(0.2)
print(lsusb())
time.sleep(1.5)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.dtr = True
time.sleep(1.0)
print(s.read(4096).decode(errors='replace'))
for cmd in ['', 'kernel version', 'kernel uptime', 'device list', 'kernel thread list', 'demo board']:
    s.write((cmd + '\r').encode())
    time.sleep(1.0)
    print(s.read(8192).decode(errors='replace'))
s.close()
