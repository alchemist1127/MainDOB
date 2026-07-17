; SPDX-License-Identifier: GPL-2.0-or-later
;
; MainDOB x3100 — fast-path boomerang entry shim.
;
; Identical mechanism to drivers/bga/bga_fast_entry.asm.  The kernel
; int 0x85 boomerang switches CR3 to x3100.mdl and `call`s us with
; the dobVideo ABI registers:
;   EAX=opcode, EBX/ECX/EDX/ESI/EDI=a0..a4, EBP=caller_pid
; We marshal those to a cdecl call to x3100_fast_dispatch (defined
; in x3100_transport_fast.c) and return its int32_t in EAX so the
; kernel can iret it back to the caller.

section .text
extern x3100_fast_dispatch

global x3100_fast_entry
x3100_fast_entry:
    push ebp                          ; caller_pid (kernel-injected)
    push edi
    push esi
    push edx
    push ecx
    push ebx
    push eax                          ; opcode
    call x3100_fast_dispatch
    add  esp, 28
    int  0x86                         ; return to caller via the kernel (EAX = rc)
