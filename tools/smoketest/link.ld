ENTRY(_start)
SECTIONS
{
	. = 0x80008000;
	.text : { KEEP(*(.text.start)) *(.text*) *(.rodata*) }
	. = ALIGN(4);
	_end = .;
	/DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) *(.comment) }
}
