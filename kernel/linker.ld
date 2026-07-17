/* MainDOB 1.1 — linker script del kernel.
 * Higher-half: linkato a 0xC0000000, caricato a 0x00100000 (1 MB).
 *
 * L'header multiboot vive in .boot.data (entry.asm) e DEVE stare nei
 * primi 8 KB del file: .boot.data e' la prima sezione. KEEP su tutte
 * le sezioni di boot: _start e le sue strutture (PD di avvio, header)
 * sono raggiunte solo dall'asm — con --gc-sections sparirebbero e GRUB
 * non riconoscerebbe piu' il kernel. */

ENTRY(_start)

KERNEL_VMA = 0xC0000000;
KERNEL_LMA = 0x00100000;

SECTIONS
{
    . = KERNEL_LMA;

    /* Sezione BASSA: header multiboot + codice pre-paging, a indirizzi
     * fisici. .boot.data per primo (header entro 8 KB). */
    .boot ALIGN(4) :
    {
        KEEP(*(.boot.data))
        KEEP(*(.boot.text))
        KEEP(*(.boot))
    }

    /* Da qui tutto e' nello spazio virtuale alto. */
    . += KERNEL_VMA;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA)
    {
        _kernel_text_start = .;
        *(.text)
        *(.text.*)
        _kernel_text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        _kernel_rodata_start = .;
        *(.rodata)
        *(.rodata.*)

        /* Tabella fixup delle copie userspace (krt/uaccess.c): coppie
         * (insn_faulting, fixup) emesse da .pushsection .ex_table. Il
         * page-fault handler la consulta in uaccess_fault_recover. */
        . = ALIGN(4);
        _ex_table_start = .;
        KEEP(*(.ex_table))
        _ex_table_end = .;

        _kernel_rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA)
    {
        _kernel_data_start = .;
        *(.data)
        *(.data.*)
        _kernel_data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        _kernel_bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        _kernel_bss_end = .;
    }

    _kernel_end = .;
    _kernel_phys_end = _kernel_end - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
    }
}
