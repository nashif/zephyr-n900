import os, sys, time
fmts = [int(x, 16) for x in sys.argv[1:]] or [0x16]
sys.argv = [sys.argv[0], 'none']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ccp2.py')).read().split("step = sys.argv")[0])
ISP_CTRL = 0x480BC040
PING, PONG, STRIDE = 0x8A000000, 0x8B000000, 2592 * 2
LC0_PING, LC0_PONG, LC0_OFST = CCP2 + 0x70, CCP2 + 0x74, CCP2 + 0x78
probe = [PING, PING + 4, PING + STRIDE, PING + 100 * STRIDE, PING + 1000 * STRIDE + 2000, PONG, PONG + 1000 * STRIDE + 2000]
show('ISP_CTRL', ISP_CTRL); wr(ISP_CTRL, rd(ISP_CTRL) | (1 << 19)); show('ISP_CTRL', ISP_CTRL)
for f in fmts:
    for a in probe:
        wr(a, 0xDEADBEEF)
    wr(LC0_CTRL, rd(LC0_CTRL) & ~1)
    wr(LC0_CTRL, (f << 3) | (1 << 2) | (1 << 1))
    wr(LC0_PING, PING); wr(LC0_PONG, PONG); wr(LC0_OFST, STRIDE)
    wr(LC0_DAT_START, 0); wr(LC0_DAT_SIZE, 1968 << 16)
    wr(LC01_IRQSTATUS, 0xFFFFFFFF); wr(LCM_IRQSTATUS, 0xFFFFFFFF)
    wr(LC0_CTRL, rd(LC0_CTRL) | 1)
    time.sleep(1.0)
    st, lcm = rd(LC01_IRQSTATUS), rd(LCM_IRQSTATUS)
    words = ' '.join(f'{rd(a):08x}' for a in probe)
    print(f'format 0x{f:02x}: LC0_CTRL {rd(LC0_CTRL):08x} LC01 {st:08x} LCM {lcm:08x} | {words}', flush=True)
print('done', flush=True)
