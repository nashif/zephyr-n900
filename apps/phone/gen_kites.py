"""Turn the Zephyr kite artwork into LVGL ARGB8888 images at three sizes."""
import sys
from PIL import Image

src, out_c, out_h = sys.argv[1:4]
kite = Image.open(src).convert('RGBA')
sizes = {'kite_large': 276, 'kite_medium': 138, 'kite_small': 46}

c = ['/* Generated from the Zephyr kite artwork; do not edit */', '#include "kites.h"', '']
h = ['/* Generated; do not edit */', '#ifndef KITES_H_', '#define KITES_H_', '',
     '#include <lvgl.h>', '']
for name, width in sizes.items():
    height = round(kite.height * width / kite.width)
    im = kite.resize((width, height), Image.LANCZOS)
    data = bytearray()
    for r, g, b, a in im.getdata():
        data += bytes((b, g, r, a))
    c.append(f'static const uint8_t {name}_data[] = {{')
    for i in range(0, len(data), 16):
        c.append('\t' + ', '.join(f'0x{x:02x}' for x in data[i:i + 16]) + ',')
    c.append('};')
    c.append(f'''const lv_image_dsc_t {name} = {{
	.header = {{
		.magic = LV_IMAGE_HEADER_MAGIC,
		.cf = LV_COLOR_FORMAT_ARGB8888,
		.w = {width},
		.h = {height},
		.stride = {width * 4},
	}},
	.data_size = sizeof({name}_data),
	.data = {name}_data,
}};
''')
    h.append(f'extern const lv_image_dsc_t {name};')
h += ['', '#endif', '']
open(out_c, 'w').write('\n'.join(c) + '\n')
open(out_h, 'w').write('\n'.join(h))
