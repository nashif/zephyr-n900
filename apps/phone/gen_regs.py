"""Extract the power-on and VGA register lists from the Linux et8ek8 driver."""
import re, sys, os
src = open(sys.argv[1]).read()
lists = {'poweron': 'mode1_poweron_mode2_16vga_2592x1968_12_07fps',
         'vga': 'mode5_vga_648x492_29_93fps'}
out = ['/* Generated from %s at build time; do not distribute */' % os.path.basename(sys.argv[1]),
       '#include <stdint.h>', 'struct cam_reg { uint16_t addr; uint8_t val; };']
for name, sym in lists.items():
    body = re.search(r'struct et8ek8_reglist %s = \{(.*?)ET8EK8_REG_TERM' % sym, src, re.S).group(1)
    regs = re.findall(r'ET8EK8_REG_8BIT,\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)', body)
    out.append('static const struct cam_reg %s_regs[] = {' % name)
    out += ['\t{0x%s, 0x%s},' % (a, v) for a, v in regs]
    out.append('};')
os.makedirs(os.path.dirname(sys.argv[2]), exist_ok=True)
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
