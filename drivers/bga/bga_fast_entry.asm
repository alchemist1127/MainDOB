; MainDOB BGA — fast-path boomerang dispatch shim (ring 3).
;
; The kernel int 0x85 boomerang switches CR3 to bga and IRETs into us at
; ring 3 on the registered dispatch stack. Args per the dobVideo ABI:
;   EAX=opcode, EBX/ECX/EDX/ESI/EDI=a0..a4, EBP=caller_pid.
; We make a cdecl call to bga_fast_dispatch and return its int32_t to the
; kernel via the int 0x86 return trap (EAX = rc). Runs with IF=0
; (single-flight): must not enable interrupts and must not block.

section .text
extern bga_fast_dispatch

global bga_fast_entry
bga_fast_entry:
    push ebp                          ; caller_pid (kernel-injected)
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax                          ; opcode
    call bga_fast_dispatch
    add  esp, 28
    int  0x86                         ; return to caller via the kernel (EAX = rc)
