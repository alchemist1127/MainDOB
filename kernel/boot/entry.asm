; mainDOB 1.1 — ingresso multiboot e salto in higher-half.
;
; VINCOLO FERRO: GRUB salta a e_entry COSI' COM'E'. Se _start fosse
; linkato all'indirizzo virtuale (0xC01xxxxx) il salto avverrebbe in
; memoria non mappata (il paging non esiste ancora) -> triple fault
; sul primo Armada acceso. Percio' header multiboot, _start e il PD di
; boot vivono in sezioni .boot* linkate all'indirizzo FISICO 0x100000;
; solo higher_half in poi e' linkato a KERNEL_VMA.
;
; Contratto GRUB all'ingresso: EAX = magic 0x2BADB002, EBX = &mbi
; (fisico). Preservati in ESI/EDI prima di qualunque clobber.

[bits 32]

KERNEL_VMA      equ 0xC0000000
PDE_PRESENT     equ 1 << 0
PDE_WRITABLE    equ 1 << 1
PDE_4MB         equ 1 << 7
CR4_PSE         equ 1 << 4
CR0_PG          equ 1 << 31

MB_MAGIC        equ 0x1BADB002
MB_FLAGS        equ 0x00000003          ; ALIGN + MEMINFO
MB_CHECKSUM     equ -(MB_MAGIC + MB_FLAGS)

; ---------------- sezione BASSA: linkata a indirizzo fisico ----------------

section .boot.data
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

align 4096
boot_page_directory:
    times 1024 dd 0

section .boot.text
global _start
_start:
    cli
    mov esi, eax                        ; preserva il magic multiboot
    mov edi, ebx                        ; preserva &mbi (fisico)

    ; --- costruisci il PD di boot (tutto fisico: paging spento) ---
    mov edx, boot_page_directory
    xor ecx, ecx                        ; i = 0..3 (16 MB in 4 PDE)
.fill_pde:
    mov eax, ecx
    shl eax, 22                         ; i * 4 MB
    or  eax, PDE_PRESENT | PDE_WRITABLE | PDE_4MB
    mov [edx + ecx * 4], eax            ; identity    [i]
    mov [edx + (768 + ecx) * 4], eax    ; higher-half [768+i]
    inc ecx
    cmp ecx, 4
    jne .fill_pde

    ; --- attiva PSE + paging ---
    mov eax, cr4
    or  eax, CR4_PSE
    mov cr4, eax

    mov eax, boot_page_directory
    mov cr3, eax

    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax

    ; --- salto assoluto nell'alias higher-half ---
    mov eax, higher_half
    jmp eax

; ---------------- sezione ALTA: linkata a KERNEL_VMA -----------------------

section .bss
align 16
boot_stack_bottom:
    resb 16384
boot_stack_top:

section .text
extern kmain

higher_half:
    ; Rimuovi l'identity mapping. Il PD di boot viene toccato tramite
    ; il suo alias higher-half (sempre mappato), MAI tramite l'identity
    ; che stiamo smontando.
    mov edx, boot_page_directory + KERNEL_VMA
    mov dword [edx + 0 * 4], 0
    mov dword [edx + 1 * 4], 0
    mov dword [edx + 2 * 4], 0
    mov dword [edx + 3 * 4], 0
    mov eax, cr3
    mov cr3, eax                        ; flush TLB completo

    mov esp, boot_stack_top
    xor ebp, ebp                        ; termina le backtrace qui

    push edi                            ; arg2: mbi (fisico)
    push esi                            ; arg1: magic multiboot
    call kmain

.hang:                                  ; kmain non ritorna; difensivo
    cli
    hlt
    jmp .hang
