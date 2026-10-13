import os, subprocess, sys, time, serial
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 40
def present(): return '2fe3:' in subprocess.run(['lsusb'], capture_output=True, text=True).stdout
t0 = time.time()
while present(): time.sleep(0.2)
while not present() or not os.path.exists('/dev/ttyACM0'):
    if time.time() - t0 > 900: sys.exit('no device')
    time.sleep(0.2)
time.sleep(1.0)
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5); s.dtr = True
t1 = time.time()
while time.time() - t1 < dur:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode(errors='replace')); sys.stdout.flush()
