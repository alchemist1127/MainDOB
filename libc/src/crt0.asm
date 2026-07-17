; MainDOB CRT0 - Entry point for userspace programs
;
; Kernel jumps here after loading the ELF. At this point the user stack
; has been prepared by the kernel (see proc/user_argv.h) with:
;
;     [esp+0]   = argc               (uint32_t)
;     [esp+4]   = argv[0]            (char *, first argument pointer)
;     [esp+8]   = argv[1]
;     ...
;     [esp+4+4*argc] = NULL sentinel
;     ...string pool above...
;
; Note that argv *is* the array itself, inline on the stack — not a
; pointer to an array elsewhere. We pass argv to main() as (esp+4),
; which is the address of the first element, which decays to `char **`
; exactly as the POSIX main() prototype expects.
;
; When the kernel was given no arguments (e.g. a plain spawn_file with
; argv=NULL, or a boot-time module), argc=0 and argv[0]=NULL are still
; written, so main() can safely inspect argv[0] without special-casing
; the empty case.
;
; Programs whose main() is declared `int main(void)` simply ignore the
; extra stack arguments — i386 cdecl has the caller clean up, so the
; two pushes below don't affect them.

section .text
global _start
extern main

_start:
    ; Zero EBP to terminate stack trace
    xor ebp, ebp

    ; Fetch argc and compute argv = &(argv[0]) = esp + 4
    mov eax, [esp]
    lea edx, [esp + 4]

    ; Push in reverse order (cdecl): argv then argc
    push edx          ; arg2 = argv
    push eax          ; arg1 = argc

    ; Call main(argc, argv)
    call main

    ; main() returned: value is in EAX. Clean the two args we pushed
    ; (cdecl is caller-cleanup) before invoking SYS_EXIT. Strictly
    ; speaking SYS_EXIT never returns so we could skip this, but
    ; keeping the stack balanced makes this routine reusable if we
    ; ever insert pre-exit handlers.
    add esp, 8

    ; Call _exit(return_value) via syscall
    mov ebx, eax        ; arg1 = exit code
    mov eax, 0           ; SYS_EXIT = 0
    int 0x80

    ; Should never reach here
.hang:
    jmp .hang
