; mainDOB 1.1 — boomerang video int 0x85, dispatch in ring 3
; (modello migrating-thread).
;
; Su `int 0x85` il kernel switcha CR3 sul driver video registrato e fa
; IRET nel suo dispatcher A RING 3: il thread del chiamante esegue il
; driver nel suo address space, senza scheduler ne' secondo thread. Il
; dispatcher ritorna con `int 0x86`; il kernel ripristina CR3 e IRETa
; il risultato al chiamante. Un fault del dispatcher e' un'eccezione
; userspace, mai un fault kernel.
;
; Convenzione chiamante (ring 3, int 0x85):
;   EAX=opcode  EBX/ECX/EDX=argomenti  ESI=payload (AS chiamante, 0=nessuno)
;   EDI=size
; Convenzione dispatcher (ring 3, entrato via iret):
;   EAX=opcode  EBX/ECX/EDX=argomenti  ESI=payload (AS driver)  EDI=size
;   EBP=pid chiamante -> rc in EAX, ritorno con `int 0x86`.
;
; RIENTRANZA: lo stato di trasporto per-chiamata (frame iret, CR3,
; registri, pid, bounce del payload) vive in uno slot PER-CPU indicizzato
; da cpu_index: due core possono avere boomerang in volo. Lo stack di
; dispatch e il buffer del driver sono condivisi: g_boomerang_lock
; serializza la sola fase in-driver (l'hardware video e' uno).
; IF=0 dall'ingresso a tutto il dispatch ring-3 (EFLAGS.IF=0, IOPL=0:
; il ring 3 non puo' fare sti); il dispatcher NON deve bloccare.
;
; cpu_index via percpu_current() (lookup autoritativo LAPIC): [gs:0] NON
; e' usabile qui — il gate interrupt conserva il GS userspace del
; chiamante. Gli offset qui sotto sono blindati da _Static_assert in
; syscall/driver.c: se una struct cambia, la build si ferma.

%define OFFSET_PROC_PID         0
%define OFFSET_THREAD_OWNER     164
%define OFFSET_PERCPU_CURRENT   4
%define OFFSET_PERCPU_CPU_INDEX 8

%define GDT_UCODE_RPL3          0x1B        ; GDT_SEL_UCODE (0x18|3)
%define GDT_UDATA_RPL3          0x23        ; GDT_SEL_UDATA (0x20|3)

%define EFLAGS_RING3            0x00000002  ; bit riservato, IF=0
%define DV_ERR_NOTREADY         -4
%define DV_ERR_INVAL            -2
%define DV_ERR_BUSY             -10
%define G_PAYLOAD_MAX           16384

; Slot di trasporto per-CPU (scalari, poi il bounce del payload).
%define BM_EIP      0
%define BM_CS       4
%define BM_EFLAGS   8
%define BM_ESP      12
%define BM_SS       16
%define BM_CR3      20
%define BM_EBX      24
%define BM_ECX      28
%define BM_EDX      32
%define BM_ESI      36
%define BM_EDI      40
%define BM_EBP      44
%define BM_PID      48
%define BM_SCRATCH  52
%define BM_RC       56
%define BM_ACTIVE   60          ; 1 = trasporto in volo su questa CPU
                                ; (lock preso, chiamante parcheggiato):
                                ; letto dal recupero C su fault
                                ; (video_boomerang_fault_recover)
%define BM_BOUNCE   64
%define BM_SLOT     (BM_BOUNCE + G_PAYLOAD_MAX)
%define BM_MAX_CPUS 32                      ; = MAX_CPUS (percpu.h)

; Budget di spin sul lock. La fase in-driver legittima dura al massimo
; ~50 ms (budget vsync di bga); questo tetto e' un ordine di grandezza
; sopra. Se scatta, il lock e' irrecuperabile (dispatcher che blocca o
; stato corrotto): meglio un DV_ERR_BUSY al chiamante che una CPU murata
; a ring 0 con IF=0 per sempre.
%define BM_LOCK_SPIN_BUDGET     20000000

; ====================================================================
section .data
align 4
global g_video_driver_cr3
global g_video_driver_entry
global g_video_driver_pid
global g_dispatch_stack_top
global g_driver_payload_buf
g_video_driver_cr3:    dd 0
g_video_driver_entry:  dd 0
g_video_driver_pid:    dd 0
g_dispatch_stack_top:  dd 0    ; stack ring-3 del dispatcher (AS driver)
g_driver_payload_buf:  dd 0    ; buffer payload nel AS driver
global g_boomerang_lock        ; il recupero C su fault lo rilascia
g_boomerang_lock:      dd 0    ; serializza la fase in-driver condivisa

; ====================================================================
section .bss
align 16
global g_bm                    ; il recupero C indicizza gli slot per-CPU
g_bm: resb BM_SLOT * BM_MAX_CPUS

; ====================================================================
section .text

extern percpu_current

; --------------------------------------------------------------------
;  int_85_entry — ingresso. Dopo il gate: ring 0, IF=0, stack kernel,
;  [esp] = frame iret del chiamante (EIP,CS,EFLAGS,ESP,SS), CR3 = suo.
; --------------------------------------------------------------------
global int_85_entry
int_85_entry:
    cld
    ; Metti da parte opcode e i tre registri che percpu_current puo'
    ; sporcare (EAX/ECX/EDX) + EBX (diventera' la base dello slot).
    ; Dopo: [esp+0]=edx [+4]=ecx [+8]=ebx [+12]=opcode, frame a [+16..].
    push eax
    push ebx
    push ecx
    push edx

    call percpu_current                 ; EAX = percpu_t*
    test eax, eax
    jz   .pc_null
    mov  edx, [eax + OFFSET_PERCPU_CURRENT]
    mov  ecx, [eax + OFFSET_PERCPU_CPU_INDEX]
    jmp  .pc_ok
.pc_null:
    xor  edx, edx
    xor  ecx, ecx
.pc_ok:
    imul ecx, ecx, BM_SLOT
    lea  ebx, [ecx + g_bm]              ; EBX = slot di questa CPU

    ; pid del chiamante -> slot
    test edx, edx
    jz   .pid0
    mov  edx, [edx + OFFSET_THREAD_OWNER]
    test edx, edx
    jz   .pid0
    mov  edx, [edx + OFFSET_PROC_PID]
    mov  [ebx + BM_PID], edx
    jmp  .pid_done
.pid0:
    mov  dword [ebx + BM_PID], 0
.pid_done:

    ; opcode + registri del chiamante -> slot
    mov  eax, [esp + 12]
    mov  [ebx + BM_SCRATCH], eax
    mov  eax, [esp + 8]
    mov  [ebx + BM_EBX], eax
    mov  eax, [esp + 4]
    mov  [ebx + BM_ECX], eax
    mov  eax, [esp + 0]
    mov  [ebx + BM_EDX], eax
    mov  [ebx + BM_ESI], esi
    mov  [ebx + BM_EDI], edi
    mov  [ebx + BM_EBP], ebp

    ; frame iret + CR3 del chiamante -> slot (l'int 0x86 sovrascrivera'
    ; il frame sullo stack kernel: va salvato qui)
    mov  eax, [esp + 16]
    mov  [ebx + BM_EIP], eax
    mov  eax, [esp + 20]
    mov  [ebx + BM_CS], eax
    mov  eax, [esp + 24]
    mov  [ebx + BM_EFLAGS], eax
    mov  eax, [esp + 28]
    mov  [ebx + BM_ESP], eax
    mov  eax, [esp + 32]
    mov  [ebx + BM_SS], eax
    mov  eax, cr3
    mov  [ebx + BM_CR3], eax

    add  esp, 16                        ; [esp] = frame del chiamante

    mov  eax, [g_video_driver_cr3]
    test eax, eax
    jz   .no_driver

    ; ---- copy-in payload: VA chiamante -> bounce per-CPU (CR3 suo) ----
    mov  ecx, [ebx + BM_EDI]
    test ecx, ecx
    jz   .skip_copy_in
    cmp  ecx, G_PAYLOAD_MAX
    ja   .invalid
    mov  esi, [ebx + BM_ESI]
    lea  edi, [ebx + BM_BOUNCE]
    rep  movsb
.skip_copy_in:

    ; ---- lock della fase in-driver (stack dispatch + buffer condivisi) ----
    ; Spin LIMITATO: questo giro avviene a ring 0 con IF=0. Con un budget
    ; infinito, un lock irrecuperabile murerebbe la CPU per sempre. EDX
    ; e' morto qui (ricaricato dallo slot dopo).
    mov  edx, BM_LOCK_SPIN_BUDGET
.lock_try:
    mov  eax, 1
    xchg eax, [g_boomerang_lock]
    test eax, eax
    jz   .locked
.lock_wait:
    pause
    dec  edx
    jz   .lock_bail
    cmp  dword [g_boomerang_lock], 0
    jne  .lock_wait
    jmp  .lock_try
.lock_bail:
    ; Lock mai libero entro il budget: bail con errore, sistema vivo.
    ; Registri del chiamante ripristinati dallo slot (EBX conterrebbe
    ; un indirizzo kernel).
    mov  eax, DV_ERR_BUSY
    mov  ecx, [ebx + BM_ECX]
    mov  edx, [ebx + BM_EDX]
    mov  esi, [ebx + BM_ESI]
    mov  edi, [ebx + BM_EDI]
    mov  ebp, [ebx + BM_EBP]
    mov  ebx, [ebx + BM_EBX]
    iretd
.locked:
    ; Trasporto in volo: da qui fino al ritorno 0x86 (o al recupero su
    ; fault) questa CPU detiene il lock e ha il chiamante parcheggiato
    ; nello slot. Il flag copre anche le copie ring-0 in CR3 driver.
    mov  dword [ebx + BM_ACTIVE], 1

    ; ---- switch CR3 (chiamante -> driver) ----
    mov  eax, [g_video_driver_cr3]
    mov  cr3, eax

    ; ---- payload: bounce per-CPU -> buffer del driver (CR3 driver) ----
    mov  ecx, [ebx + BM_EDI]
    test ecx, ecx
    jz   .skip_copy_drv
    lea  esi, [ebx + BM_BOUNCE]
    mov  edi, [g_driver_payload_buf]
    rep  movsb
.skip_copy_drv:

    ; ---- registri d'ingresso del dispatcher ----
    mov  ecx, [ebx + BM_ECX]
    mov  edx, [ebx + BM_EDX]
    mov  edi, [ebx + BM_EDI]
    test edi, edi
    jz   .no_payload_esi
    mov  esi, [g_driver_payload_buf]
    jmp  .esi_ready
.no_payload_esi:
    xor  esi, esi
.esi_ready:
    mov  ebp, [ebx + BM_PID]
    mov  eax, [ebx + BM_SCRATCH]
    mov  ebx, [ebx + BM_EBX]

    ; ---- frame iret ring-3 verso il dispatcher, sul suo stack ----
    push dword GDT_UDATA_RPL3           ; SS
    push dword [g_dispatch_stack_top]   ; ESP
    push dword EFLAGS_RING3             ; EFLAGS (IF=0)
    push dword GDT_UCODE_RPL3           ; CS
    push dword [g_video_driver_entry]   ; EIP
    iretd

.no_driver:
    mov  eax, DV_ERR_NOTREADY
    iretd
.invalid:
    mov  eax, DV_ERR_INVAL
    iretd

; --------------------------------------------------------------------
;  int_86_return — trap di ritorno. Ring 0, IF=0, CR3 = driver,
;  EAX = rc, [esp] = frame ring-3 del dispatcher (sara' sovrascritto
;  con quello del chiamante). Stesso core dell'ingresso: slot uguale.
; --------------------------------------------------------------------
global int_86_return
int_86_return:
    cld
    push eax
    call percpu_current
    test eax, eax
    jz   .pc_null
    mov  ecx, [eax + OFFSET_PERCPU_CPU_INDEX]
    jmp  .pc_ok
.pc_null:
    xor  ecx, ecx
.pc_ok:
    imul ecx, ecx, BM_SLOT
    lea  ebx, [ecx + g_bm]
    pop  eax
    mov  [ebx + BM_RC], eax

    ; ---- copy-out: buffer del driver -> bounce per-CPU (CR3 driver) ----
    mov  ecx, [ebx + BM_EDI]
    test ecx, ecx
    jz   .skip_out_drv
    mov  esi, [g_driver_payload_buf]
    lea  edi, [ebx + BM_BOUNCE]
    rep  movsb
.skip_out_drv:

    ; ---- rilascio del lock (buffer/stack del driver non piu' usati) ----
    ; ACTIVE va spento PRIMA del rilascio: tenuto acceso oltre, un fault
    ; successivo (copy-out verso un puntatore chiamante marcio) farebbe
    ; rilasciare al recupero un lock gia' libero o ripreso da un'altra
    ; CPU. Spento qui, quel fault segue il percorso normale (colpa del
    ; chiamante) senza leak: il lock e' gia' libero.
    mov  dword [ebx + BM_ACTIVE], 0
    mov  dword [g_boomerang_lock], 0

    ; ---- switch CR3 (driver -> chiamante) ----
    mov  eax, [ebx + BM_CR3]
    mov  cr3, eax

    ; ---- copy-out: bounce per-CPU -> VA del chiamante (CR3 suo) ----
    mov  ecx, [ebx + BM_EDI]
    test ecx, ecx
    jz   .skip_out_caller
    lea  esi, [ebx + BM_BOUNCE]
    mov  edi, [ebx + BM_ESI]
    rep  movsb
.skip_out_caller:

    ; ---- ripristino registri del chiamante ----
    mov  esi, [ebx + BM_ESI]
    mov  edi, [ebx + BM_EDI]
    mov  ebp, [ebx + BM_EBP]

    ; ---- sovrascrivi il frame del dispatcher con quello del chiamante ----
    mov  eax, [ebx + BM_EIP]
    mov  [esp], eax
    mov  eax, [ebx + BM_CS]
    mov  [esp + 4], eax
    mov  eax, [ebx + BM_EFLAGS]
    mov  [esp + 8], eax
    mov  eax, [ebx + BM_ESP]
    mov  [esp + 12], eax
    mov  eax, [ebx + BM_SS]
    mov  [esp + 16], eax

    mov  eax, [ebx + BM_RC]
    mov  ebx, [ebx + BM_EBX]
    iretd
