import re, sys, time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3); s.dtr = True; time.sleep(1); s.read(65536)
for line in sys.argv[1:]:
    s.read(65536)
    s.write((line + '\r').encode()); time.sleep(1.2)
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
    print('>>', line); print('   ' + ' / '.join(x.strip() for x in out.replace('\r','').split('\n') if x.strip() and 'uart:~' not in x))
