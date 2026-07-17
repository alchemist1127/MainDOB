; mainDOB 1.1 — stub ISR/IRQ generati a macro.
; Ogni stub normalizza il frame (error code fittizio dove la CPU non lo
; pusha), salva i registri e chiama isr_dispatch(regs).

[bits 32]

extern isr_dispatch

; GS caricato ad ogni ingresso in kernel. In build SMP (-DMAINDOB_SMP
; passato a NASM, vedi Makefile) e' GDT_SEL_PERCPU: this_cpu() risolve
; via %gs:0 anche in contesto interrupt (osservato dal kernel 1.0 — senza
; questo, ogni chiamata a this_cpu() dentro un handler ricadrebbe sempre
; sul fallback via APIC id, corretto ma piu' lento). In build UP resta
; GDT_SEL_KDATA: la voce GDT 6 non esiste li', caricare 0x30 farebbe #GP.
%ifdef MAINDOB_SMP
%define KERNEL_GS_SEL 0x30        ; GDT_SEL_PERCPU
%else
%define KERNEL_GS_SEL 0x10        ; GDT_SEL_KDATA
%endif

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push dword 0                        ; error code fittizio
    push dword %1
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push dword %1                       ; la CPU ha gia' pushato l'errore
    jmp  isr_common
%endmacro

; Eccezioni CPU 0..31 (8, 10..14, 17 pushano error code)
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; IRQ hardware rimappati a 32..47
%assign i 32
%rep 16
ISR_NOERR i
%assign i i+1
%endrep

; Vettori device allocabili (IOAPIC-routed + MSI): 0x50..0xDF. Stesso
; stub generico; l'EOI va al LAPIC (mai al PIC) — lo decide il
; dispatcher unificato in arch/x86/intr.c. Il range e i suoi confini
; sono blindati contro VEC_STUB_FIRST/LAST in intr.c.
%assign i 0x30
%rep (0xDF - 0x30 + 1)
ISR_NOERR i
%assign i i+1
%endrep

; Vettori Local APIC (milestone SMP, arch/x86/lapic.c): stesso stub
; generico, nessun error code hardware. 0xF0 timer, 0xF1 resched IPI,
; 0xF2 TLB shootdown IPI, 0xFF spurious.
ISR_NOERR 240
ISR_NOERR 241
ISR_NOERR 242
ISR_NOERR 255

isr_common:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10                        ; segmenti dati kernel
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ax, KERNEL_GS_SEL                ; per-CPU (SMP) / dati kernel (UP)
    mov gs, ax

    push esp                            ; isr_regs_t *regs
    call isr_dispatch
    add  esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 8                          ; vector + error code
    iretd

; --- syscall gate (int 0x80): niente error code, vettore 0x80 ---
global syscall_stub
extern isr_dispatch
syscall_stub:
    push dword 0
    push dword 0x80
    jmp  isr_common
