import os, sys, time
sys.argv = [sys.argv[0], 'none']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ccp2.py')).read().split("step = sys.argv")[0])
STRIDE, PING = 2592 * 2, 0x8A000000
LC0_PING, LC0_PONG, LC0_OFST = CCP2 + 0x70, CCP2 + 0x74, CCP2 + 0x78
marks = [2592 - 4, 2592 + 4, 3240 - 4, 3240 + 4, 5184 - 4, 5184 + 8]
for f in range(32):
    for m in marks: wr(PING + m, 0xDEADBEEF)
    wr(LC0_CTRL, rd(LC0_CTRL) & ~1)
    wr(LC0_CTRL, (f << 3) | (1 << 2) | (1 << 1))
    wr(LC0_PING, PING); wr(LC0_PONG, PING); wr(LC0_OFST, STRIDE)
    wr(LC0_DAT_START, 0); wr(LC0_DAT_SIZE, 1968 << 16)
    wr(LC01_IRQSTATUS, 0xFFFFFFFF); wr(LCM_IRQSTATUS, 0xFFFFFFFF)
    wr(LC0_CTRL, rd(LC0_CTRL) | 1); time.sleep(0.6)
    wr(LC0_CTRL, rd(LC0_CTRL) & ~1)
    st, lcm = rd(LC01_IRQSTATUS), rd(LCM_IRQSTATUS)
    written = ''.join('w' if rd(PING + m) != 0xDEADBEEF else '.' for m in marks)
    first = ' '.join(f'{rd(PING + 4 * i):08x}' for i in range(3))
    print(f'fmt {f:02x}: LC01 {st:04x} LCM {lcm:x} marks {written} first {first}', flush=True)
