ENTRY(_kernel_main)
SECTIONS
{
  . = 0x10000;
  .text :
  {
    KEEP(*(.text.entry))
    *(.text)
    *(.rdata*)
    *(.data)
    *(.bss)
    *(COMMON)
  }
  /DISCARD/ : { *(.eh_frame) *(.idata*) *(.reloc) *(.drectve) }
}
