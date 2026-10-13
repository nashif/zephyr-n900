import re, time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.3); s.dtr = True; time.sleep(1); s.read(65536)
B = 'i2c@48060000'
def send(l): s.write((l + '\r').encode())
for d in (0.0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.8):
    s.read(65536)
    send('devmem 0x49054090 32 0x40'); time.sleep(0.3)
    send('devmem 0x49054094 32 0x40'); time.sleep(d)
    send(f'i2c direct_read {B} 0x3e 1'); time.sleep(0.8)
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
    res = 'NACK' if 'Failed to read' in out.split('direct_read')[-1] else 'ACK'
    print(f'read {d*1000:4.0f} ms after reset release (plus shell latency): {res}', flush=True)
