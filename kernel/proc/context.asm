; mainDOB 1.1 — context switch cooperativo.
;
; void context_switch(context_t *old, context_t *new)
;   context_t: [0]=esp, [4]=cr3
; Salva i callee-saved + EFLAGS del thread uscente sul suo stack,
; scrive old->esp, carica new->cr3 se diverso, ripristina new->esp e
; i registri del thread entrante. IF viaggia dentro EFLAGS salvate:
; chi entra riprende ESATTAMENTE con lo stato IF che aveva.

[bits 32]

global context_switch
global kernel_thread_entry
global user_thread_entry
extern  thread_entry_trampoline

context_switch:
    mov eax, [esp + 4]                  ; old
    mov edx, [esp + 8]                  ; new

    pushfd
    push ebp
    push ebx
    push esi
    push edi

    mov [eax + 0], esp                  ; old->esp

    mov ecx, [edx + 4]                  ; new->cr3
    mov ebx, cr3
    cmp ecx, ebx
    je  .same_space
    mov cr3, ecx
.same_space:

    mov esp, [edx + 0]                  ; new->esp

    pop edi
    pop esi
    pop ebx
    pop ebp
    popfd
    ret

; Primo ingresso di un thread kernel appena creato. Lo stack e' stato
; preparato da context_setup con il layout che context_switch si
; aspetta; il ret finale atterra qui, che chiama il trampolino C
; (esegue entry(arg) e poi thread_exit — mai ritorna).
kernel_thread_entry:
    sti                                 ; i thread nascono con IRQ attivi
    call thread_entry_trampoline
.never:
    cli
    hlt
    jmp .never

; Primo ingresso di un thread UTENTE. seed_user_context ha preparato lo
; stack kernel cosi' (dal fondo): frame iret [ss][esp][eflags|IF][cs]
; [eip], poi [arg], poi il frame di context_switch. Il ret di
; context_switch atterra qui con ESP che punta ad [arg].
;
; ABI 1:1 col 1.0: l'argomento del thread utente arriva in ECX
; (dob_thread_spawn lo usa per recuperare fn+ctx); gli altri registri
; general-purpose entrano azzerati. Il frame iret ha gia' IF=1: gli
; interrupt si riaccendono ATOMICAMENTE col passaggio a ring 3 — mai
; prima, o un IRQ potrebbe corrompere il frame preparato.
USER_CS equ 0x1B                        ; user code, RPL 3
USER_DS equ 0x23                        ; user data, RPL 3

user_thread_entry:
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    pop ecx                             ; argomento utente (ABI: in ECX)
    xor eax, eax
    xor ebx, ebx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    iretd
