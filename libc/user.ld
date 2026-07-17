/* MainDOB Userspace Linker Script
 * Programs are loaded at 0x00400000 (4MB, above null guard page area).
 * Stack is set up separately by the kernel at ~0xBFFF0000.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x00400000;

    .text ALIGN(4096) :
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4096) :
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4096) :
    {
        _bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        _bss_end = .;
    }

    _end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
    }
}
