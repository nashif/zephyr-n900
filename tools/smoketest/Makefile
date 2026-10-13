CROSS ?= $(HOME)/zephyr-sdk-1.0.1/arm-zephyr-eabi/bin/arm-zephyr-eabi-
CFLAGS = -mcpu=cortex-a8 -marm -O2 -ffreestanding -fno-pic -nostdlib -Wall -Wextra

smoke.bin: smoke.elf
	$(CROSS)objcopy -O binary $< $@

smoke.elf: start.S main.c link.ld
	$(CROSS)gcc $(CFLAGS) -T link.ld -o $@ start.S main.c -lgcc

clean:
	rm -f smoke.elf smoke.bin
