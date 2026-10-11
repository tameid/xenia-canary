/*
 * probe.ld - flat Xenon image layout.
 *
 * XEX2 images are "flat": file offset == RVA == (address - image base).
 * xenon-gcc/xenon-ld still want an ELF to chew on, so we link at absolute
 * virtual addresses and then objcopy -O binary emits a blob whose offset 0
 * is IMAGE_BASE + 0x1000. mkxex.py prepends the PE headers.
 */

OUTPUT_FORMAT("elf32-powerpc")
ENTRY(_start)

SECTIONS
{
  . = 0x82001000;
  .text : { *(.text .text.*) *(.init .init.*) }

  /*
   * .ximports is fixed at 0x82002000: mkxex.py carries the authoritative
   * (ordinal, thunk address) table at that base, and thunks.S emits the slots
   * in that order. Moving it here breaks import binding. The cost is that
   * .text must stay under 0x1000 bytes, so a diagnostic that grows it past
   * that fails the link instead of silently landing on an import slot.
   */
  . = 0x82002000;
  .ximports : { *(.ximports .ximports.*) }

  . = 0x82003000;
  .rodata : { *(.rodata .rodata.*) *(.sdata2 .sdata2.*) }

  . = 0x82010000;
  .data : { *(.data .data.*) *(.sdata .sdata.*) }

  .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }

  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame.*)
                *(.gnu.attributes) *(.gnu.lto_*) *(.debug_*) }
}