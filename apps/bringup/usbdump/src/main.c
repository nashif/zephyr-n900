/* Dump the HS USB OTG (MUSB) state NOLO leaves behind */
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define MUSB 0x480AB000
#define OTG  (MUSB + 0x400)

int main(void)
{
	printk("ICLKEN1_CORE %08x (bit4 HSOTGUSB) IDLEST1_CORE %08x\n",
	       sys_read32(0x48004A10), sys_read32(0x48004A20));
	printk("OTG rev %08x sysconfig %08x sysstatus %08x interfsel %08x forcestdby %08x\n",
	       sys_read32(OTG + 0x0), sys_read32(OTG + 0x4), sys_read32(OTG + 0x8),
	       sys_read32(OTG + 0xC), sys_read32(OTG + 0x14));
	printk("FADDR %02x POWER %02x DEVCTL %02x INTRUSBE %02x INTRTXE %04x INTRRXE %04x\n",
	       sys_read8(MUSB + 0x00), sys_read8(MUSB + 0x01), sys_read8(MUSB + 0x60),
	       sys_read8(MUSB + 0x0B), sys_read16(MUSB + 0x06), sys_read16(MUSB + 0x08));
	printk("EPINFO %02x RAMINFO %02x CONFIGDATA %02x\n", sys_read8(MUSB + 0x78),
	       sys_read8(MUSB + 0x79), (sys_write8(0, MUSB + 0x0E), sys_read8(MUSB + 0x1F)));
	printk("GPIO67 (gpio3 pin3) OE %u DATAOUT %u DATAIN %u\n",
	       (sys_read32(0x49052034) >> 3) & 1, (sys_read32(0x4905203C) >> 3) & 1,
	       (sys_read32(0x49052038) >> 3) & 1);
	printk("pads hsusb0_clk %04x stp %04x dir %04x nxt %04x data0 %04x  gpio67 %04x\n",
	       sys_read16(0x480021A2), sys_read16(0x480021A4), sys_read16(0x480021A6),
	       sys_read16(0x480021A8), sys_read16(0x480021AA), sys_read16(0x480020D2));
	return 0;
}
