import re, sys, time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1); s.dtr = True; time.sleep(1); s.read(65536)
def sh(line, wait=0.4):
    s.reset_input_buffer(); s.write((line + '\r').encode()); time.sleep(wait)
    out = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', s.read(65536).decode(errors='replace'))
    return ' | '.join(x.strip() for x in out.splitlines() if 'uart:~' not in x and x.strip() and x.strip() != line and 'Mapped' not in x and 'Using data' not in x)
def rd(a, w=32):
    m = re.search(r'Read value 0x([0-9a-fA-F]+)', sh(f'devmem 0x{a:08x} {w}')); return int(m.group(1), 16) if m else None
def wr(a, v, w=32): sh(f'devmem 0x{a:08x} {w} 0x{v:x}')
I2C3 = 'i2c@48060000'
def probe(tag):
    r = {a: sh(f'i2c read_byte {I2C3} 0x{a:02x} 0x00') for a in (0x0c, 0x1d, 0x3e)}
    print(tag, '|', '  '.join(f'{a:02x}: {v}' for a, v in r.items()), flush=True)
for cmd in sys.argv[1:]:
    if cmd == 'probe': probe('probe')
    elif cmd.startswith('rd:'):
        a, w = (cmd[3:].split(',') + ['32'])[:2]; v = rd(int(a, 16), int(w)); print(f'{a} = {v:x}' if v is not None else f'{a} ERR', flush=True)
    elif cmd.startswith('wr:'):
        a, v, w = (cmd[3:].split(',') + ['32'])[:3]; wr(int(a, 16), int(v, 16), int(w)); print(f'{a} <- {v}', flush=True)
    elif cmd.startswith('sh:'): print(sh(cmd[3:], 1.5), flush=True)
    elif cmd.startswith('sleep:'): time.sleep(float(cmd[6:]))
