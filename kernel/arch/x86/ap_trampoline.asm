; =============================================================================
; mainDOB — trampolino di avvio AP (bring-up fino al check-in + halt)
; =============================================================================
; Questo blob e' un TEMPLATE. Vive, linkato normalmente, nell'immagine
; kernel higher-half (.rodata). Al bring-up di un'AP la BSP lo copia
; verbatim in una pagina fisica bassa fissa (AP_TRAMPOLINE_PHYS = 0x8000)
; e avvia l'AP li' via INIT-SIPI-SIPI. Il vettore SIPI e' il numero di
; pagina, quindi l'AP parte in modalita' reale 16 bit a
; CS:IP = 0x0800:0x0000 = fisico 0x8000.
;
; POSIZIONE-INDIPENDENZA. L'indirizzo di link del template (VMA alta) NON
; e' dove gira (0x8000). Per farlo funzionare senza trucchi del linker e
; senza rilocazione a runtime:
;   - i dati in modalita' reale si raggiungono come offset-da-start:
;     DS = CS = 0x0800, quindi DS:(etichetta - ap_trampoline_start)
;     risolve a fisico 0x8000 + offset;
;   - cio' che serve un indirizzo lineare assoluto (la base della GDT, il
;     target del far-jump in modalita' protetta) e' calcolato in fase di
;     ASSEMBLAGGIO come
;         AP_TRAMPOLINE_PHYS + (etichetta - ap_trampoline_start)
;     che e' corretto dopo la copia, quindi nulla lo ripatcha.
; Solo due valori genuinamente a runtime sono ripatchati dalla BSP nella
; COPIA:
;       ap_param_pd     <- fisico della page directory kernel (in CR3)
;       ap_param_stack  <- cima dello stack kernel di questa AP
; ap_main e' un extern semplice: la sua VMA alta e' una costante di link e
; la meta' alta e' mappata una volta acceso il paging, quindi vi si salta
; direttamente.
;
; Fasi:
;   1. modo reale @0x8000: cli, DS=CS, lgdt [GDT flat], CR0.PE=1, far-jump 32.
;   2. 32 bit, identity bassa: segmenti dati flat, CR3=PD kernel, CR0.PG=1.
;      L'EIP basso continua a risolvere perche' la BSP mappa temporaneamente
;      questa pagina in identity nel PD kernel prima del bring-up. Carica
;      ESP, salta a ap_main (meta' alta).
; Il PD kernel usa solo pagine 4KB, quindi PSE puo' restare spento sull'AP.
; =============================================================================

AP_TRAMPOLINE_PHYS  equ 0x8000

extern ap_main

section .rodata
global ap_trampoline_start
global ap_trampoline_end
global ap_param_pd
global ap_param_stack
global ap_param_apicid
global ap_param_ack

BITS 16
ap_trampoline_start:
    cli
    cld
    ; DS = CS = 0x0800 cosi' gli slot dati sotto sono raggiungibili in
    ; modalita' reale.
    mov     ax, cs
    mov     ds, ax

    ; Carica la GDT flat. L'operando e' un offset relativo a DS; il campo
    ; base della GDTR contiene gia' l'indirizzo lineare assoluto
    ; 0x8000+(ap_gdt-start), che e' < 2^24, quindi anche un LGDT a 16 bit
    ; (base a 24 bit) lo carica correttamente.
    lgdt    [ap_gdtr - ap_trampoline_start]

    ; Entra in modalita' protetta.
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax

    ; Far-jump nel segmento codice flat a 32 bit (selettore 0x08).
    ; Target assoluto calcolato in fase di assemblaggio.
    jmp     dword 0x08:(AP_TRAMPOLINE_PHYS + (ap_pm_entry - ap_trampoline_start))

BITS 32
ap_pm_entry:
    ; Dati flat ovunque (selettore 0x10).
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; GATE DI IDENTITA'. Gli slot di parametro sotto (stack, ack) sono
    ; ripatchati per-AP in questa pagina CONDIVISA. Un'AP che ha mancato
    ; la propria finestra di bring-up puo' svegliarsi TARDI, mentre la
    ; BSP sta gia' avviando la PROSSIMA AP — e caricherebbe lo stack
    ; della prossima: due CPU su una pila. Percio' ogni round la BSP
    ; ripatcha anche l'apic id del bersaglio; un'AP il cui id non
    ; corrisponde e' una straggler di un round precedente e si parcheggia
    ; per sempre. Il paging e' ancora spento qui, quindi il registro id
    ; del LAPIC si legge direttamente dal suo indirizzo MMIO fisico (ogni
    ; AP riparte in modalita' xAPIC con la base di default, id nei bit
    ; 31..24 di +0x20). Se la lettura va in garbage (hardware APIC
    ; disabilitato dal firmware — patologico), il confronto fallisce e
    ; l'AP si parcheggia: sicuro, e la BSP logga il mancato ack.
    mov     eax, [0xFEE00020]
    shr     eax, 24
    cmp     eax, [AP_TRAMPOLINE_PHYS + (ap_param_apicid - ap_trampoline_start)]
    jne     .park

    ; Carica la page directory kernel (ripatchata) e accende il paging.
    ; Da qui sia 0x8000 (identity, predisposta dalla BSP) sia la meta'
    ; alta sono mappate, quindi l'esecuzione continua qui e ap_main
    ; diventa raggiungibile.
    mov     eax, [AP_TRAMPOLINE_PHYS + (ap_param_pd - ap_trampoline_start)]
    mov     cr3, eax
    mov     eax, cr0
    or      eax, 0x80000000
    mov     cr0, eax

    ; Passa allo stack kernel di questa AP (ripatchato, indirizzo higher-half).
    mov     esp, [AP_TRAMPOLINE_PHYS + (ap_param_stack - ap_trampoline_start)]
    mov     ebp, esp

    ; ACK: gli slot di parametro per-AP sono ora completamente consumati
    ; (ESP caricato). La BSP gira su questo prima di ripatchare la
    ; pagina per la prossima AP, quindi gli slot non vengono mai
    ; sovrascritti sotto un'AP che sta ancora leggendo. La pagina e'
    ; identity-mapped nel PD kernel, quindi questa scrittura funziona
    ; col paging acceso.
    mov     dword [AP_TRAMPOLINE_PHYS + (ap_param_ack - ap_trampoline_start)], 1

    ; Nel C, meta' alta. L'indirizzo di ap_main e' una costante di link e
    ; la meta' alta e' mappata, quindi un salto indiretto assoluto e'
    ; corretto.
    mov     eax, ap_main
    jmp     eax

.hang:                              ; ap_main non ritorna mai; cintura e bretelle.
    cli
    hlt
    jmp     .hang

.park:                              ; straggler di un round di bring-up
    cli                             ; precedente (mismatch di apic-id): mai
    hlt                             ; toccare i parametri ripatchati, dormi per sempre.
    jmp     .park

; ---- GDT flat usata solo per raggiungere la modalita' protetta 32 bit ----
align 8
ap_gdt:
    dq 0x0000000000000000           ; 0x00 null
    dq 0x00CF9A000000FFFF           ; 0x08 codice flat (base 0, limite 4G, 32bit, ring0)
    dq 0x00CF92000000FFFF           ; 0x10 dati flat (base 0, limite 4G, 32bit, ring0)
ap_gdt_end:

ap_gdtr:
    dw  ap_gdt_end - ap_gdt - 1                              ; limite
    dd  AP_TRAMPOLINE_PHYS + (ap_gdt - ap_trampoline_start)  ; base (assoluta)

; ---- parametri a runtime, ripatchati dalla BSP nella copia a 0x8000 ----
ap_param_pd:     dd 0               ; fisico della page directory kernel (CR3)
ap_param_stack:  dd 0               ; cima dello stack kernel di questa AP
ap_param_apicid: dd 0               ; apic id bersaglio di questo round (gate identita')
ap_param_ack:    dd 0               ; l'AP scrive 1 quando stack/parametri sono consumati
ap_trampoline_end:
