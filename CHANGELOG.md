# MainDOB — consegna stratificata eventi-prima: re-kick sul wake ridondante; la cintura declassata a ultima rete (b147)

Obiezione di design accolta: la cintura del sonno e' TIME-driven — non
e' polling (nessuna scansione di stato, nessun ciclo, nessuna CPU
accesa: un auto-risveglio da microsecondi ogni 2 s armato dal
dormiente per se stesso), ma resta un residuo del mondo periodico in
un sistema che si dichiara a eventi. E il design a blocchi aveva un
canale event-driven inutilizzato proprio sul punto cieco: il WAKE
RIDONDANTE.

RADICE STRUTTURALE (gia' notata in b144+, ora riparata): con una IPI
persa sul ferro, il thread resta READY in coda e la home in hlt — e
OGNI evento successivo che prova a svegliarlo (ogni pacchetto del
mouse) usciva dal gate "wake ridondante" di scheduler_unblock senza
fare NULLA. Il sistema VEDEVA la patologia decine di volte al secondo
e la ignorava per design.

RIPARO EVENT-DRIVEN (la via primaria, scheduler_unblock): sul ramo
ridondante — freddo per definizione — se il thread e' READY, ancora
accodato, e la sua home risulta ADESSO in hlt (la firma esatta della
consegna mai atterrata), si RITENTA l'IPI. Guarigione alla latenza del
prossimo evento naturale (millisecondi per l'input), zero periodicita',
zero costi sul percorso caldo. Letture advisory fuori dai lock: al
peggio una IPI spuria che il giudice smonta sotto il proprio lock.

LA CINTURA RESTA, declassata a ULTIMA RETE, per il solo caso che
nessun evento puo' riparare: la conversazione SINCRONA (A chiama B,
l'IPI verso B si perde, A e' bloccato in attesa della reply — nessuno
rinvochera' mai un wake su B; sagoma compatibile col freeze originale
del CQ62). Il limite e' teorico, non implementativo: contro la perdita
su canale hardware un sistema a soli eventi e' indimostrabile (ARQ /
due generali — ogni consegna affidabile su canale con perdita esige un
timeout da qualche parte). Il tempo entra quindi UNA volta, nel punto
piu' basso e quieto del sistema, e la gerarchia e' dichiarata nel
codice: eventi prima, tempo come ultima ratio.

File: proc/scheduler.c. [ABI]/[PX] intatti.

---

# MainDOB — cintura del sonno v2: pavimento sul deadline LOCALE (fine dell'"elettrocardiogramma"); direct-map a 768 MB (b146)

DUE difetti della cintura b144+, entrambi DIAGNOSTICATI DAI LOG DI
CAMPO (QEMU, -smp 2) — e un tetto RAM troppo caro.

1) LA TEMPESTA DI EPISODI (e gli spike a intervalli regolari).
Sintomo: migliaia di "consegna recuperata" gia' al boot, su QEMU dove
una IPI non si perde MAI; task manager a elettrocardiogramma.
RADICE: il verbo della cintura armava min(AGENDA GLOBALE, now+2s).
Ogni CPU dormiente, a ogni ciclo di sonno, si risincronizzava sulla
testa dell'agenda di TUTTO il sistema — e diventava co-sparatrice di
ogni timer altrui sotto i 2 s: typed-text a 33 ms, blink, tick GUI.
Due CPU sveglie a ogni scadenza, gare di drain, e la firma della gara:
l'enqueue remoto diventa visibile PRIMA che la sua IPI atterri, il
fuoco locale arriva in mezzo, il giudice trova lavoro senza impronte
-> falso positivo dell'audit a raffica. La valanga di kprintf su
seriale 115200 (~8 ms a riga, attesa TX bounded ma reale) era essa
stessa il grosso degli spike.
FIX (event.c): registro per-CPU del deadline ARMATO (s_armed_ns,
single-writer: refresh e cintura girano sulla CPU che armano). La
cintura diventa un PAVIMENTO sul SOLO deadline locale: se quello
armato e' piu' vicino resta sovrano, se la sorgente e' disarmata si
arma il solo pavimento, e l'agenda globale NON si rilegge — la
scadenza di un'altra CPU resta responsabilita' di chi l'ha armata,
esattamente come prima della cintura. Costo a regime: una sveglia da
microsecondi ogni 2 s per CPU dormiente, e basta.
Audit ritoccato di conseguenza: la condizione e' dichiarata compatibile
anche con la gara benigna (finestra di microsecondi, ora rara), log
"consegna senza impronte" con rate-limit severo (2 iniziali, poi ogni
1024). Il segnale vero e' un contatore che cresce STABILMENTE a
sistema quieto, non l'episodio sporadico.

2) "VEDE 256 MB DI RAM".
Due concause. La VM: QEMU_MEMORY era 256M nel Makefile (ora 1G, che
esercita anche il tetto). Il kernel: il tetto allocabile min(RAM,
direct-map) ratificato in b145 e' CORRETTO (mai distribuire frame
irraggiungibili a KERNEL_VMA+F) ma il direct-map fermo a 256 MB lo
rendeva sproporzionato: un CQ62 da 2-4 GB troncato a 256 MB.
FIX: direct-map esteso a 768 MB — tutto il budget VA kernel non
impegnato. Nuovo layout (paging.c): C000_0000..F000_0000 direct-map;
F000_0000..FF00_0000 finestra heap (KHEAP_VBASE/VLIMIT ricollocate,
240 MB di VA); FF00_0000..FF80_0000 transienti (smoke test spostato a
FF000000); PD_TEMP e recursive intatti. Il pre-allocate delle PT della
finestra segue il nuovo range (lo snapshot PDE resta immutabile e
completo). RAM oltre i 768 MB resta dichiaratamente ignorata e
DENUNCIATA dal log PMM: la via vera e' un highmem con finestre mobili
— decisione separata, non un rattoppo.

File: time/event.{c,h}, proc/scheduler.c, mm/paging.c, mm/kheap.c,
mm/pmm.c (commenti), Makefile. [ABI]/[PX] intatti: gli indirizzi
toccati sono tutti interni al kernel (nessun VA condiviso con
userspace nel range mosso).

---

# MainDOB — audit dei fix non documentati: regressione mirata (valvola RT, firma "inceppato" + offerte donatore), il resto ratificato (b145)

CONTESTO. Confronto sistematico con l'albero b144 (content_blit
fastpath): l'albero corrente porta un corpo ampio di modifiche kernel
SENZA voce di changelog. Difetto di PROCESSO prima che di codice: ogni
cambio di comportamento va documentato, o diventa impossibile separare
i fix reattivi (bug osservati) dai preventivi (ipotesi). Questo audit
li ha ripassati uno a uno, giudicandoli nel merito.

RATIFICATI (restano, con questa voce come documentazione postuma —
tutti chiudono classi di bug reali o osservate):
- workqueue sotto spinlock MPMC (irq_save da solo su ring condiviso
  fra CPU e' rotto su SMP per costruzione);
- canali NON-PERDIBILI di teardown/respawn (teardown_q intrusiva,
  respawn_pending) al posto della workqueue droppabile per il lavoro
  di recupero critico; fault path di isr.c allineato;
- process_get_ref/process_put + process_reclaim (lookup PID pinnato:
  chiude la finestra use-after-free del puntatore nudo);
- copie utente fault-safe via .ex_table (copy_from_user/copy_to_user):
  chiude il TOCTOU check-poi-unmap di un thread fratello;
- discovery.c: expires_at in ms a 64 bit (il wrap dei 49,7 giorni);
- attese temporizzate fail-closed su heap timer saturo (wait, futex):
  timeout immediato invece del sonno eterno;
- PMM: tetto allocabile = min(RAM, direct-map) — mai distribuire frame
  irraggiungibili a KERNEL_VMA+F;
- buffer IPC per-processo a frame INDIPENDENTI (map_anon_range, con
  azzeramento anti info-leak e rollback atomico): lo spawn non fallisce
  piu' con RAM libera ma frammentata;
- FPU lazy-restore per proprietario (fpu_owner): AUDITATO — il reap
  invalida l'owner e nel modello a spawn nessun percorso rifonda lo
  stato FP di un thread vivo in memoria; guadagno netto sul pattern
  dominante A -> idle -> A;
- cpu_hungry + donazione solo-compute (lezione b132, osservata);
- crescita/ritiro dell'heap timer, coalescenza VA del kheap, trim del
  depot IPC, coordinatore reclaim, dma_pool: l'igiene del "boot
  eterno" — preventivi di principio, costo a regime nullo;
- saturazione refcount: resta (protegge dal wrap -> UAF, costo un
  confronto predetto), MA saturava e assorbiva l'underflow IN
  SILENZIO, contro la debuggabilita' dichiarata: aggiunte
  refcount_saturation_note / refcount_underflow_note (fuori-linea in
  krt/refcount.c, rate-limited) — le patologie restano assorbite, ma
  ora si VEDONO.

REGREDITI (preventivi netti: costo su percorsi caldi contro rischi
ipotetici o coperture ridondanti):
- VALVOLA DI EQUITA' RT (rt_burst_ns, rt_relief, rt_valve_account,
  ramo nel pick): difendeva da un thread prio-0 in busy-loop MAI
  osservato; la motivazione "watchdog compresi" non regge — il
  watchdog kernel uccide via callback timer in contesto IRQ, non
  dipende dallo scheduling. In cambio caricava il pick (il percorso
  piu' caldo) e violava in un angolo il contratto di priorita' stretta
  dichiarato. Un RT rotto si diagnostica e si fixa; non si ammortizza
  per sempre nel pick di tutti.
- FIRMA "INCEPPATO" nel pull + OFFERTE DEL DONATORE in slice_check
  (contended_since_ns, last_offer_ns, isteresi 50 ms, offerte ogni
  100 ms): reintroduceva l'inseguimento della coda-a-1 che b118 aveva
  escluso come rumore (sia pure con isteresi) e aggiungeva lavoro a
  OGNI slice check con potenziale churn di migrazioni/IPI sul
  dual-core. Il buco che copriva — core sovraccarico DOPO che l'altro
  s'e' addormentato, nessuna rivalutazione — e' oggi coperto DALLA
  CINTURA DEL SONNO: al suo fuoco (<= 2 s) la CPU dormiente ripassa da
  idle_try_pull e vede il sovraccarico profondo. Stessa copertura per
  un evento raro, zero costo a regime. Il pull torna alla sola firma
  PROFONDA (>= PULL_MIN_READY).

NOTA di onesta' sul sintomo riportato ("peggiorato per prestazioni e
affidabilita'"): dei regrediti, solo donor/inceppato poteva incidere
sulle prestazioni a regime (churn di migrazioni sul carico
conversazionale reale); la valvola RT era inerte ma caricava il pick.
NESSUNO dei due spiega il freeze del CQ62 sotto screensaver (con
l'idle in corso non girano slice check): la diagnosi della cintura del
sonno (b144+) resta in piedi e la riga di audit in seriale e' la
verifica sul campo.

File: proc/scheduler.c, krt/refcount.{h,c} (nuovo .c). [ABI]/[PX]
intatti.

---

# MainDOB — cintura del sonno: risveglio hardware garantito alle CPU in hlt (chiude il buco IPI-persa di b93)

Sintomo sul campo (CQ62, live CD): screensaver attivo da tempo, al
ritorno schermo nero con la sola freccia (HW cursor statico nei
registri), nessuna reazione a mouse o tastiera. Niente schermata di
panic (b113 l'avrebbe dipinta): kernel plausibilmente vivo, consegna
degli eventi morta.

RADICE (il buco DICHIARATO dalla nota di onesta' di b93 e mai chiuso):
da b93 non esiste piu' alcuna rete periodica — il protocollo cpu_idle
+ barriere simmetriche e' corretto per costruzione contro ogni gara
SOFTWARE (ri-verificato riga per riga in questa indagine: regge), ma
una IPI di resched che il FERRO non consegna lascia il thread READY in
coda e la home in hlt per sempre. Aggravante strutturale: ogni wake
successivo dello stesso thread esce al gate "wake ridondante" di
scheduler_unblock senza mai ritentare l'IPI — una perdita = consegna
morta definitiva (ogni mossa del mouse sveglia inputd... che e' gia'
READY in coda: niente nuovo kick). Su QEMU una IPI non si perde mai;
il CQ62 dual-core e' il primo ferro vero a dormire a lungo (merito
dello screensaver nuovo) e quindi il primo esposto.

IL MODELLO, sistemico e fedele alla separazione — non un floor
reintrodotto, non un canale di consegna nuovo:
- INVARIANTE: nessuna CPU dorme in hlt oltre SLEEP_BELT_NS (2 s) senza
  un risveglio garantito dal PROPRIO silicio.
- ESECUTIVO (time/event.c): verbo time_event_arm_sleep_belt(period) —
  arma la sorgente LOCALE al minimo tra agenda e now+period.
  IRQ-atomico al proprio interno; nessuna policy. time_event_refresh
  resta orologio puro, intatto: il primo dispatch dopo il risveglio
  (switch_to -> refresh) riassorbe la cintura da se'. Sul fallback PIT
  periodico e' un no-op dichiarato (il tick E' gia' la cintura).
- LOGICA (scheduler_idle_block, l'unico punto in cui si dorme): arma
  la cintura come ultima parola prima di sti;hlt, solo SMP (su UP non
  esistono IPI: ogni enqueue e' locale e l'epilogo IRQ consegna).
- CONSEGNA: la macchina che gia' esiste. Al fuoco della cintura,
  event_fire -> preempt_if_needed trova need_resched alzato
  dall'enqueue orfano e consegna dal giudice, come ogni altro wake.

AUDIT (massima debuggabilita', tre campi per-CPU single-writer):
wake_ipi_seen (timbrato dal nuovo ingresso scheduler_resched_ipi, che
incapsula il giudice: arch/lapic resta ignaro dello stato di
scheduling) e wake_local_seen (timbrato dall'enqueue con home locale)
sono l'impronta dell'episodio di sonno, azzerati dal blocco idle prima
di pubblicare cpu_idle. Il giudice, quando strappa la CPU dall'idle
con lavoro in coda e NESSUNA impronta, ha davanti l'esatta patologia:
log rate-limited "[SCHD] cintura del sonno: consegna recuperata su cpu
N (probabile IPI persa sul ferro)" + contatore belt_recoveries. Sul
CQ62 quella riga in seriale e' la prova sul campo. Advisory puri: mai
una decisione di scheduling ne dipende.

COSTO a sistema sano: mezzo risveglio al secondo per CPU dormiente, un
event_fire vuoto (un confronto), zero tocchi sul percorso caldo dei
wake, zero letture cross-CPU. Latenza massima di recupero da una IPI
persa: un periodo. SCARTATO il retry dell'IPI in enqueue_home_and_kick
per thread gia' READY: secondo meccanismo per lo stesso invariante,
stato in piu' sul percorso caldo — la cintura da sola limita il danno
e il design resta a un solo proprietario per responsabilita'.

Finestra residua dichiarata (commento nel verbo): un fuoco timer
interposto tra l'arm della cintura e l'hlt puo' disarmare la sorgente
(refresh su agenda vuota) e quel singolo sonno parte scoperto —
microsecondi su una protezione probabilistica, il sonno successivo si
riallaccia.

File: time/event.{c,h}, proc/scheduler.c. [ABI]/[PX] intatti: nessuna
syscall, nessun opcode, nessun layout condiviso toccato (i campi nuovi
sono in coda a sched_cpu_t, struttura senza consumatori ASM).

---

# MainDOB — Logon: password di accesso, screensaver, timeout e Blocca (dobinterface2)

Nuovo foglio logon.c: la "tenda". Se /SYSTEM/CONFIG/Logon_password.dat
esiste, il primissimo frame del desktop e' la schermata di accesso;
se non esiste (la ISO live non lo contiene mai) si va dritti al
desktop. La presenza del file E' l'interruttore: rimuovere la
password cancella il file.

Sicurezza PASSIVA e dichiarata tale: nessuna cifratura, una tenda per
l'impiccione occasionale. Ma nel file non c'e' MAI la password in
chiaro: record versionato MDLOGON1 con hash salato (FNV-1a 64 +
rimescolamento, salt dal pool di entropia), confronto a tempo
costante, lockout di 1.5 s dopo un tentativo errato — senza mai
bloccare il thread: il lockout e' un timestamp piu' un one-shot che
ripulisce la riga d'errore, il loop resta puro event-driven. Formato,
hash e verifica vivono in <dob/logon.h> (header-only, condiviso col
wizard).

Resa: la tenda e' il backbuf stesso alzato a LOGON_LAYER_Z=900 —
sopra finestre, servo, tray e toast, sotto il solo cursore; lo z=50
di MC/About copre le finestre ma non basta da sipario. Il ramo tenda
di compositor_rebuild pulisce a NERO, disegna la sola UI del logon e
ritorna: non un pixel del desktop trapela (lo screensaver e' il nero
puro). I present restano i soliti flip pieni flash-free.

Input: tenda su = ogni tasto muore in logon_key e TUTTO
l'instradamento mouse e' soppresso (logon_gate_mouse in input_pump,
prima di ogni consegna): niente finestre, pannello, icone, widget,
scroll, screenshot (Stamp irraggiungibile per la stessa via).
L'evento che risveglia lo screensaver viene INGOIATO — mai consegnato
ne' battuto come primo carattere della password. All'ingresso della
tenda input_abort_sessions chiude d'ufficio drag/resize/grab e
cattura del puntatore, e menu/tray/About/MC vengono chiusi: al
risveglio il desktop e' quieto.

Inattivita': timestamp last_activity aggiornato dal foglio input a
ogni evento umano + UN timer one-shot a riarmo pigro (armato per
l'intero timeout; allo scatto si riarma per il solo residuo). Zero
polling, MAI una syscall per keystroke. Timeout dal setting
ui.idle_screensaver (minuti, 0 = mai, default 10), live-reload
all'Applica come il typed-text. Allo scatto: schermo nero; al
risveglio, prompt se c'e' password, desktop se no.

Menu MainDOB: due voci nuove. "Blocca" (prompt se c'e' password,
altrimenti degrada al solo oscuramento — mai uno stato senza via
d'uscita) e "Oscura schermo" (screensaver immediato, sempre, anche
con password: al risveglio vale la regola unica).

Cambio password — la via EPS. dobinterface (gia' driver in
Startup_modules, quindi ammesso dal cancello del daemon) dichiara la
entry EPS SECRET sicurezza.logon_password: l'editor la rende come
casella mascherata e all'Applica spedisce il testo DIRETTAMENTE alla
nostra porta (GUI_LOGON_PW_WRITE, nuovo opcode 161; READ=160 risponde
sempre vuoto). Quel testo e' la password ATTUALE: verificata (o
assente) -> DOB_OK e doppio prompt sotto tenda; errata ->
DOB_ERR_DENIED e toast. Nel doppio prompt: campo vuoto al primo
Invio = password RIMOSSA (file cancellato); Esc annulla il cambio;
conferma diversa = si riparte. Il doppio prompt e' UI del foglio, NON
un DobPopup: i popup sono stub client con loop annidato in una
finestra servita da noi — chiamarli da noi stessi sarebbe un deadlock
per costruzione.

Nota di onesta' (ferro vero): niente DPMS in v1 — lo screensaver e'
un frame nero, i driver video attuali non espongono lo spegnimento
pannello (verbo futuro di DobVideoControl). Il cursore resta visibile
sulla tenda (HW cursor nei registri: nasconderlo richiede il driver);
scelta documentata, feedback che la macchina e' viva. [ABI]/[PX]
intatti: opcode e comandi solo AGGIUNTI, mai rinumerati; il desktop
fuori tenda e' identico al pixel.

Build: logon.c e' preso dal wildcard *.c del Makefile di boot;
nessuna regola nuova.

---

# MainDOB — Settings: tipo SETTING_SECRET (casella mascherata, solo EPS)

Quarto tipo di controllo nel protocollo (SETTING_SECRET=3): l'editor
lo rende come textbox MASCHERATA. Il daemon lo accetta SOLO in una
entry di classe EPS: in una entry FILE il valore finirebbe persistito
in chiaro nel .setting — esattamente cio' che il tipo esiste per
impedire. Stub e daemon validano in lockstep (era > SETTING_ENUM, ora
> SETTING_SECRET + il vincolo di classe).

Semantica nell'editor: un SECRET e' un handshake, non uno stato. Si
committa al servizio SOLO se l'utente lo ha davvero toccato
(tb.modified) — senza questo cancello ogni Applica del file
ricommetterebbe anche la casella intonsa, e il servizio riceverebbe
un falso tentativo a ogni salvataggio di un'altra impostazione. Su
successo la casella si svuota (segreto consumato); su fallimento
resta, per correggere senza ribattere.

libdobui: dob_textbox_t guadagna il flag `masked` — il Draw maschera
la SOLA fetta visibile (font a cella fissa: un '*' occupa lo spazio
esatto del carattere, tutta la matematica di selezione/cursore/anchor
resta identica), Copy/Cut/CopyAll rifiutano (mai segreti in
clipboard; il Cut degrada a delete della selezione), Paste resta
ammesso. Default false da Init (memset): zero impatto sui consumatori
esistenti.

---

# MainDOB — Setup: password di accesso nel passo Parametri (wizard)

Il passo "Parametri sistema" guadagna la doppia casella MASCHERATA
"Password di accesso (opzionale)" + "Conferma password" — la doppia
battitura e' la difesa dal typo, l'eco a pallini dal collo torto.
Avanti valida la coincidenza (popup d'errore se no); vuote entrambe =
nessuna password, legittimo. Pitch del passo uniformato a 66 px per
far entrare le sei righe nei 460 px del corpo.

All'installazione (INST_PHASE_CONFIG), se impostata, viene generato
/SYSTEM/CONFIG/Logon_password.dat col record MDLOGON1 di
<dob/logon.h> (hash salato, MAI la password in chiaro): dal primo
avvio del sistema installato dobinterface mostra la schermata di
accesso. Fallimento di scrittura non fatale (loggato): un sistema
senza logon e' un sistema funzionante. Il riepilogo mostra solo lo
STATO ("impostata"/"nessuna"), mai il valore.

---

# MainDOB — DobInterface 2.0: present di solo-contenuto senza rebuild del backbuf (dobinterface2)

Prestazioni: un cambio di solo contenuto (keystroke, redraw client)
faceva compositor_repaint -> compositor_rebuild PIENO, ripulendo il
backbuf e ridisegnando icone + pannello, anche se era cambiato solo il
corpo di una finestra. Ma le finestre sono layer separati dal backbuf.

Nuovo verbo compositor_content_blit: ri-baka solo le finestre col corpo
sporco (surface_dirty) e fa il flip PIENO, saltando il rebuild del
backbuf. Il loop di main separa il branch: DIRTY_FULL|PANEL -> repaint
pieno; DIRTY_CONTENT da solo -> content_blit (con ricaduta al repaint
pieno se overlay/anteprima resize sono attivi: quei pixel vivono sul
backbuf). Risparmio: clear backbuf + icon_draw_all + panel_draw per
ogni tick di contenuto — sensibile su ferro vero (compose HW), dove il
costo era il ridisegno CPU del chrome desktop.

Sicurezza del present: il flip resta PIENO (page-flip), l'unico modello
validato flash-free. NON e' il dirty-rect scissored (blit in-place
nella pagina visibile), che resta ritirato per il flash su ferro. Il
pattern e' identico al re-bake gia' in esercizio in
compositor_drag_blit. [ABI]/[PX] intatti.

Nota (debito annotato, non toccato qui): content_dirty per-finestra e'
stato vestigiale — scritto in piu' punti ma MAI letto in codice; il
trigger reale del bake e' surface_dirty. Da rimuovere in una pulizia
dedicata.

---

# MainDOB — DobInterface 2.0: dirty state con un solo proprietario (foglio dirty) (dobinterface2)

Le "4 sorelle" needs_repaint / needs_panel_repaint / needs_content_blit
/ needs_cursor_redraw erano 4 booleani globali scritti da ~50 punti in
11 fogli e AZZERATI da TRE fogli (compositor, main, input), concordi
solo per convenzione. NON un data race — il loop e' mono-thread
(verificato: nessun thread applicativo, nessun signal handler) — ma
stato mutabile senza proprietario: bastava toccare un'autorita'
ignorando le altre per lasciare una flag stantia (repaint spurio a ogni
tick) o persa (repaint mancato). "Race in attesa di succedere" nel
senso di hazard logico, non di concorrenza.

Nuovo foglio dirty.c: unico proprietario, stato PRIVATO (bitmask
DIRTY_FULL|PANEL|CONTENT|CURSOR), tre verbi soli — di_mark_dirty
(accende, OR-in: l'unico modo di sporcare), di_dirty (interroga, dal
loop-spec di main), di_dirty_clear (spegne). Regola di ownership: OGNI
present azzera SOLO le ragioni che ha presentato — compositor_repaint
consuma FULL|PANEL|CONTENT, compositor_drag_blit FULL|CONTENT, il ramo
cursore del loop CURSOR. Nessuna ragione ha piu' di un consumatore per
tipo di present: il clear sparso a tre autorita' e' sparito.

Comportamento identico per costruzione: la condizione di present (union
FULL|PANEL|CONTENT) copre la vecchia promozione+branch; il clear netto
per giro e' lo stesso insieme di prima. [ABI]/[PX] intatti. Le ~50
scritture grezze needs_X=true diventano di_mark_dirty(DIRTY_X)
(dichiarazioni d'intento). Il bitmask e' anche la base del dirty-rect
present (compositor_repaint_rect, ancora spento).

Build: dirty.c e' preso in automatico dal wildcard *.c del Makefile di
boot; nessuna regola nuova.

---

# MainDOB — DobInterface 2.0: primitive di disegno riusabili su surface + layout titolo unificato (dobinterface2)

Evoluzione del rifacimento dobinterface2. Nessun cambiamento [ABI] ne'
[PX]: comportamento identico al pixel, verificato per costruzione
(cambi meccanici equivalenti, non ricompilati qui).

Riciclo lungo i seam della modularita', non toppe:

- draw.c: le primitive di disegno passano da "legate al backbuf" a due
  strati. surf_hline/surf_vline/surf_fill_rect/surf_draw_rect operano
  su una SURFACE qualunque (verbi esecutivi puri, zero stato globale);
  fb_* restano come binding di convenienza al backbuf desktop,
  invariati per tutti i chiamanti storici (icons, panel, missionctl,
  winactions, about). Lo strato surf_* e' il nucleo promuovibile a
  libdobui il giorno in cui un client disegnera' sul proprio pannello
  SHM (percorso GUI_WIN_SHM_ENSURE).

- compositor.c: win_bake_chrome smette di ri-implementare il disegno.
  I bordi (alto, laterali, basso) passano da surf_hline/surf_vline; il
  titolo passa da string_to_glyphs (foglio font) invece di
  ri-srotolare il loop dob_font_decode/left/advance. Un solo punto
  calcola gli avanzamenti proporzionali: titolo finestra e testo del
  corpo non possono piu' divergere [PX].

Zero righe morte: string_to_glyphs esisteva gia'; nessuna API nuova
senza consumatore. La relocazione di draw/font a libdobui e' rimandata
al primo client che la usa — shared code senza consumatore e' proprio
il debito che si vuole evitare.

---

# MainDOB — imgcodec.mem: codec PNG/JPEG di sistema + DobPicture v2.1 (build 127)

Decisione di architettura (dell'utente, corretta): i codec immagine
NON si linkano statici in DobPicture — sono di SISTEMA, e viaggiano
come .MEM caricabile (il pattern di iso9660.mem/exfat.mem): chiunque
li riusa (thumbnail di DobFiles, viewer futuri) con una call diretta,
zero IPC, zero duplicazione nei binari.

IMGCODEC.MEM (programs/DobPicture/imgcodec.c -> build/programs/
imgcodec.mem, installato accanto a DobPicture.mdl):
- Self-contained come da contratto .mem: memcpy/memset propri,
  allocazioni via set_allocator del host (niente heap propria).
- DECODER (collaudati su HOST contro PIL/libjpeg): PNG bit-exact su
  RGB/RGBA/palette/gray 8bit (inflate DEFLATE completo: stored +
  Huffman fisso e dinamico; defilter sub/up/avg/Paeth; alpha composto
  su bianco; niente Adam7); JPEG baseline SOF0 1/3 componenti,
  campionamenti 1x1..2x2, restart marker, IDCT matriciale intera a
  punto fisso (la prima, a butterfly, era SBAGLIATA: bocciata dal
  collaudo, maxdiff 255 -> riscritta, avg ~1 su 4:4:4; il residuo sul
  4:2:0 e' la scelta nearest vs filtro triangolare di libjpeg,
  legittima per un viewer). Progressive: rifiutato pulito.
- ENCODER (collaudati: PIL apre entrambi): PNG con deflate a Huffman
  fisso + LZ greedy (hash 3 byte), CRC32/Adler32 — round-trip esatto
  col nostro decoder; JPEG baseline 4:4:4 q90, tabelle Annex K scalate,
  FDCT = trasposta della stessa matrice (primo tentativo con /8 di
  troppo: coefficienti quasi tutti a zero, bocciato e corretto —
  avg 1.72 vs originale a q90).

DOBPICTURE v2.1:
- carica il .mem all'avvio (assente/ABI diversa -> degrada a solo-BMP,
  mai un crash); Apri: .bmp|.png|.jpg|.jpeg (probe sui magic, ripiego
  BMP); Salva: formato per ESTENSIONE del percorso scelto (PNG/JPEG
  via codec, BMP nativo).
- UI completata dalla revisione: slider RGB reintegrate (fini, drag
  continuo, tinte per canale), bottoni [-]/[+] per zoom e pennello,
  strumento PAN (mano: la vista segue il drag), convenzione MainDOB
  Ctrl+rotellina = scorrimento orizzontale (event_modchange cache).

Build: regola .mem in programs/Makefile (ld -shared, verifica ET_DYN
come exfat.mem); mkbootdisk/mklive copiano imgcodec.mem accanto al
programma.

File: programs/DobPicture/{main.c,imgcodec.c,imgcodec_api.h},
programs/Makefile, tools/{mkbootdisk.sh,mklive.sh}.
# MainDOB — DobPicture v2: fix del modello di frame (build 126b)

Prima prova su ferro: finestra inusabile — solo pannello, toolbar
sparita. CAUSA: avevo trattato il cmdbuf come ritenuto incrementale,
ma il CONTRATTO del framework e' immediate-mode per frame (cmdbuf_reset
nello stub: ogni Invalidate consegna un buffer che SOSTITUISCE il
precedente). Il mio view_flush faceva un Invalidate separato col solo
blit del pannello: quel frame cancellava i record della toolbar.

FIX: un unico present(band_y0, band_rows) — l'UNICO punto che chiama
Invalidate: ri-registra la toolbar (~40 record, spiccioli) e dichiara
la banda sporca del pannello NELLO STESSO frame. Tutti i percorsi
(tratto, fill, zoom, palette, resize-drag col bersaglio in toolbar,
salvataggi) convergono li'. toolbar_draw -> toolbar_records (puro
registratore, niente Invalidate).

File: programs/DobPicture/main.c.
# MainDOB — DobPicture v2: riscrittura moderna (build 126)

Il v1 era uno dei primissimi programmi di MainDOB: pre-eventi, pre-SHM,
render a run-length con chiamate per-pixel al server — da cui il lag
tratto-schermo e il costo CPU/GPU/RAM/VRAM. Riscritto da zero sullo
stack moderno (il legacy resta come main_legacy.c.bak, fuori build).

ARCHITETTURA:
- Pannello SHM (come DobWrite): la vista E' il buffer condiviso col
  compositore; un tratto = record di banda da 9 byte, non un upload.
  Ripiego automatico a BlitBufferDynamic se il pannello manca.
- Orientato a EVENTI (app.h): il tratto si disegna nel mousemove con
  interpolazione di Bresenham tra i campioni — niente buchi, niente
  ritardo percepito; si ridipinge e consegna SOLO la banda sporca.
- Barra sinistra ELIMINATA, riassorbita in un'unica toolbar in alto:
  strumenti (icone ad alto contrasto: nel v1 erano quasi invisibili),
  zoom, spessore, dimensioni tela, palette 24 colori + corrente.

NOVITA' RICHIESTE:
- RIEMPIMENTO (flood fill, BFS con stack su heap — mai ricorsione).
- ZOOM 1/2/4/8x (tasti Z/X + indicatore in toolbar); scroll con frecce
  e rotellina.
- RIDIMENSIONAMENTO TELA dall'angolo basso-destra: maniglia
  tratteggiata sull'angolo della tela, cursore CURSOR_RESIZE in
  override, dimensione bersaglio in toolbar durante il drag, applicata
  al rilascio (contenuto preservato, aree nuove bianche).
- Contagocce (preleva e torna a matita), gomma.
- Codec BMP standard (b125), 24bpp bottom-up; load accetta 24/32bpp e
  top-down.

File: programs/DobPicture/main.c (riscritto), main_legacy.c.bak.
# MainDOB — DobPicture: codec BMP con canali R/B invertiti (build 125)

Scoperto riaprendo uno screenshot di sistema con DobPicture: canali
colore invertiti. INDAGINE: lo screenshot e' innocente — scrive BMP
standard (B,G,R su disco). Il colpevole e' il codec BMP di DobPicture,
che swappava R<->B in ENTRAMBE le direzioni: writer emetteva il byte
"B" dai bit 16-23 del canvas (che sono R, il canvas e' 0x00RRGGBB — la
palette lo dimostra), il loader rimontava specularmente. Il round-trip
dei PROPRI file tornava (doppio swap = identita'), mascherando il bug
da sempre; ma i BMP salvati erano NON-standard su disco (aperti
sull'host: invertiti anche li'), e ogni BMP standard esterno si apriva
coi canali scambiati. Il primo file BMP "straniero" della storia di
MainDOB — lo screenshot — l'ha smascherato.

CORRETTO: writer e loader ora usano la mappatura standard.

MIGRAZIONE (una tantum, da sapere): i BMP salvati da DobPicture PRIMA
di questa build erano swappati su disco — riaperti ora appariranno
invertiti UNA volta; risalvati, diventano standard per sempre.

File: programs/DobPicture/main.c.
# MainDOB — screenshot: scorciatoia alternativa Ctrl+Alt+Shift+P (build 124b)

Per gli emulatori, dove Stamp R Sist e' spesso rapito dall'host (QEMU
su macOS in testa). inputd: tracking dell'Alt SINISTRO (0x38 non
esteso — prima esisteva solo AltGr, che seleziona il layout) e check
della combinazione a livello di SCANCODE (0x19 = tasto P), prima della
traduzione di layout: indipendente dalla mappa attiva. Entrambi gli
Alt valgono (option puo' presentarsi da entrambi i lati). Consegna lo
stesso SKEY_PRTSC: da li' in poi la catena e' identica.
# MainDOB — screenshot di sistema: Stamp R Sist -> BMP (build 124)

Funzione screenshot, architettura decisa in discussione: la cattura si
genera in DOBINTERFACE per ricomposizione SOFTWARE della scena — il
compositore possiede tutto (z-order, chrome, cmdbuf trattenuti,
pannelli SHM mappati, cursore), quindi la cattura e'
DRIVER-INDIPENDENTE per costruzione: identica su bga, mach64, x3100 e
ferro vero, zero op nuove nel protocollo video, zero lavoro per-driver.
Non e' un readback hardware: cattura il modello del compositore, che
coincide con lo schermo finche' tutto passa dalla pipeline dv_* (in
MainDOB: per costruzione).

CATENA: inputd rileva Stamp R Sist (E0 37; il fake-shift E0 2A cade
gia' nel ramo esteso ignoto) -> SKEY_PRTSC (137) al subscriber ->
dobinterface lo INTERCETTA prima del routing alla finestra a fuoco ->
screenshot_take() ricompone in un buffer RAM e scrive
/DATA/Screenshots/AAAA-MM-GG-hh.mm.mmm.bmp (millisecondi da gettime;
24bpp bottom-up, lo stesso dialetto BMP di DobPicture) -> toast col
percorso come riscontro.

RICOMPOSIZIONE (visitor 1:1 con origine e clip, riuso del decodificatore
cmdbuf gia' esistente — lo stesso dei widget SHM e delle thumbnail):
sfondo -> finestre in z-order (twin raster del chrome di win_bake:
bordo/header/titolo/corpo bianco; corpo = pannello SHM copiato
direttamente + replay del cmdbuf trattenuto) -> cursore (pixel-alpha
dalla stessa bitmap del layer z=999).

SPECCHIO CPU DELLE TEXTURE: i pixel del tex_pool vivono nella RAM del
processo driver (SYSRAM) — irraggiungibili dalla ricomposizione. Ora
ogni texture del pool tiene uno specchio CPU (alloc a TEX_ALLOC, banda
copiata a TEX_UPDATE, free a TEX_FREE e alla distruzione finestra).
Costo: una copia in piu' di cio' che il client gia' invia (icone: KB;
pagina DobWrite: MB) — accettato e documentato; malloc fallita =
segnaposto grigio nello screenshot, mai un crash. Lo specchio abilita
anche thumbnail fedeli in futuro.

FUORI v1 (documentato nel codice): icone desktop, wpanel/tray, toast
nella cattura — richiedono il twin dei rispettivi percorsi di disegno
(v1.1). Le finestre, il contenuto che conta, ci sono per intero.

File: boot/inputd/main.c, boot/dobinterface/main.c.
# MainDOB — misura dei pool DMA nel task manager (build 123)

Il pool DMA diventa osservabile, su tre livelli:
- ZONA DMA (<16 MB, PMM): frame liberi gia' contati dal PMM
  (pmm_stats.dma_free), ora esposti nell'header dello snapshot.
- SLOT TRACCIATI (layer driver, 64 totali): pressione (usati/max)
  nell'header — la tabella piena e' il sintomo n.1 di un driver che
  perde buffer DMA.
- PER PROCESSO: pagine DMA tracciate per PID, per riga.

MECCANICA: accessori dma_track_pages_of / dma_track_slot_pressure in
syscall/driver.c sotto il PROPRIO s_dma_lock; sys_task_snapshot li
interroga DOPO il collettore, mai annidati sotto il lifecycle-lock —
zero ordini di lock nuovi. ABI: header 100 byte (reserved ->
dma_free_frames + dma_slots impacchettato usati<<16|max), riga 56 byte
(+dma_pages); assert aggiornati su entrambe le copie.

UI: colonna "DMA KB" nella grid (vuota per chi non ne ha); tab RAM con
riga "DMA (<16MB): N KB liberi - slot X/64".

File: kernel/syscall/{driver.c,syscall.c}, kernel/proc/tasksnap.h,
libdob/include/dob/task.h, programs/taskmgr/main.c.
# MainDOB — indagine "bga consuma 24 MB": artefatto di conteggio, separato device/RAM (build 122)

INDAGINE (dal collaudo del task manager): i 24812 KB di bga NON sono
RAM consumata. Scomposizione esatta:
- 16384 KB = l'apertura VRAM del BAR PCI, che bga mappa con
  SYS_MMAP_PHYS (vregion con flag VREG_DEVICE, PTE non-cachable):
  memoria DEL DISPOSITIVO, zero frame fisici — ma il total_pages
  introdotto in b120 contava tutte le vregion indiscriminatamente.
- ~8.4 MB di RAM VERA: lo shadow framebuffer di composizione
  (malloc w*h*4: 3-5 MB a seconda della risoluzione) + superfici
  SYSRAM di staging + codice/heap. Legittimo per un driver che
  compone; scala con la risoluzione. NESSUN leak.

CORRETTO:
- vregion_list.device_pages: sottoinsieme di total_pages con
  VREG_DEVICE, mantenuto negli stessi verbi di mutazione (insert,
  unlink, clear, resize — gia' sotto vm_lock).
- ABI snapshot: riga a 52 byte — mem_pages ora e' SOLO RAM (totale
  meno device), nuovo dev_pages per le aperture MMIO. Entrambe le
  copie dell'header + assert.
- taskmgr: colonna "Mem KB" = RAM vera; nuova colonna "MMIO KB"
  (vuota per chi non ha aperture — quasi tutti). bga ora mostra
  ~8.4 MB di RAM e 16384 KB di MMIO, ciascuno per quello che e'.

NOTA per il futuro (vGPU/VRAM): la VRAM *interna* (quote, superfici
in-VRAM) resta amministrata dal driver — questa colonna mostra solo
l'apertura mappata; il consumo fine andra' chiesto a DobVideoControl
via IPC quando si fara' quella tab.

File: kernel/mm/vregion.{c,h}, kernel/proc/{thread.c,tasksnap.h},
libdob/include/dob/task.h, programs/taskmgr/main.c.
# MainDOB — Gestione Attivita' v2: tab, grid multicampo, resize, renice (build 121)

Revisione dell'interfaccia dal collaudo visivo, piu' il pezzo kernel
che mancava per il "cambia priorita'".

KERNEL:
- SYS_PROC_SET_PRIORITY (119): renice di un intero processo per PID
  (0..3), PI-CONSAPEVOLE — per ogni thread: base_priority = nuova, ed
  effettiva = min(base, boost dei mutex posseduti), lo stesso modello
  del recompute del mutex (lettura di boost_prio senza guard come nel
  recompute: advisory, si sana all'unlock). Tutto sotto il
  lifecycle-lock; la consegna cross-core del cambio e' quella di b119
  (need_resched + IPI). PID 0 intoccabile. Modello di fiducia
  mono-utente documentato nella syscall.
- ABI snapshot v1 esteso: riga a 48 byte con `priority` (la migliore
  fra i thread del processo) — entrambe le copie dell'header + assert.

TASKMGR v2:
- QUATTRO TAB (strip disegnata in alto, click per cambiare):
  Processi / CPU / RAM / Core. Pannello comandi per-tab.
- PROCESSI: grid MULTICAMPO (dobgrid, N colonne ridimensionabili,
  PID congelato a sinistra): PID | Processo | Uso | Pri | Core | Thr |
  Mem KB | Pin. Selezione agganciata al PID (sopravvive al riordino),
  CANC = termina.
- CPU: sismografo grande dell'uso totale. RAM: candele (usata/libera)
  + sismografo dell'uso %. CORE: candele per-core + un sismografo per
  core (fino a 4 strisce impilate).
- STORICI in ring buffer PROPRI (~38 s), alimentati nei grafici con
  UpdateAll a ogni ridisegno: sopravvivono a cambio tab e resize (la
  re-Init dei widget ne azzererebbe i campioni interni).
- RESIZE riparato: event_resize -> layout() ri-inizializza la
  geometria dei widget sulle dimensioni correnti e ridisegna tutto.
  Prima il contenuto era cablato a 640x460 dentro finestre cresciute.
- CAMBIA PRIORITA': overlay modale in-finestra (4 righe cliccabili coi
  quanti 2/5/10/20 ms; tasti 0..3 come scorciatoia); il polling si
  ferma sotto l'overlay e riparte alla chiusura con riscontro
  immediato nella colonna Pri.

IN DISCUSSIONE (non implementato): grafici vGPU/VRAM — la sorgente
naturale non e' una syscall ma il driver video via IPC
(DobVideoControl gia' espone quota/free di VRAM al desktop); se ne
riparla quando si progetta il protocollo.

File: kernel/proc/{thread.c,tasksnap.h}, kernel/syscall/syscall.{c,h},
libc/include/sys/syscall.h, libdob/include/dob/task.h,
programs/taskmgr/main.c.
# MainDOB — fix build: stub EPS di taskmgr (build 120b)

Il link di taskmgr.mdl falliva su tutti i dobui_* / dobpopup_Show /
dobconfig_Get: mancava la dichiarazione degli Entry Point stub nel
Makefile dei programmi. Aggiunta:
  EPS_taskmgr := DobInterface DobPopup DobConfig
(DobConfig per cliptext, dipendenza transitiva di DOBUI_OBJS — stesso
insieme, meno DobFileSystem, di modules.)
# MainDOB — contabilita' del tempo CPU + SYS_TASK_SNAPSHOT + Gestione Attivita' (build 120)

Fondamento e primo consumatore del task manager. Struttura ovunque a
blocchi: esecutivi in alto, logica in basso.

KERNEL — contabilita' (prima NON esisteva: nessun "% CPU" possibile):
- thread_t.cpu_time_ns (in CODA alla struct: layout del boomerang
  intoccato): scritto SOLO da switch_to sulla home quando il thread
  LASCIA la CPU — single-writer, zero lock. Una sola lettura di clock
  per switch, riusata per la scadenza del quanto.
- sched_cpu_t.{last_dispatch_ns, idle_time_ns}: ozio per-CPU (uso =
  1 - Δidle/Δt); ancorati in scheduler_start / enter_ap.
- process_t.cpu_time_dead_ns: il tempo dei thread morti confluisce nel
  processo in reap_detach (sotto il lifecycle-lock: PCB vivo per
  costruzione).
- Letture remote di u64 su i386 possono strapparsi: read_u64_stable
  (rilettura fino a stabilita', bounded) negli accessori
  scheduler_cpu_idle_ns / scheduler_thread_cpu_ns. L'ozio include la
  frazione idle IN CORSO (una CPU ferma da secondi non deve sembrare
  occupata fino al prossimo switch).
- vregion_list.total_pages: mantenuto dai verbi di mutazione (gia'
  sotto vm_lock), letto senza lock dallo snapshot.

KERNEL — SYS_TASK_SNAPSHOT (118), ABI in proc/tasksnap.h (copia
identica in libdob/include/dob/task.h, layout inchiodato da assert):
header {ncpu, now_ns, idle_ns per core, RAM} + fino a 64 righe {pid,
stato, home, pinned, thread, pagine, cpu_ns, nome}. FILOSOFIA: il
kernel espone CONTATORI MONOTONI, mai percentuali — la finestra di
misura la sceglie il display. Il collettore (task_snapshot_collect,
thread.c) cammina i THREAD sotto il lifecycle-lock e aggrega per
processo: mai la proclist (nessun nesting di lock nuovo), PCB vivi per
costruzione, kernel thread aggregati in 'kernel' (PID 0), idle esclusi.
Le righe si raccolgono in un buffer kernel e si copiano DOPO il
rilascio del lock (copy_to_user puo' fault-are, mai con uno spinlock in
mano).

USERSPACE — programs/taskmgr ("Gestione Attivita'", nel menu):
- polling a 300 ms via dobui_set_tick (il tick del framework E' il
  poll: nessun loop a mano);
- percentuali come delta tra due poll: uso core = 1-Δidle/Δt, uso
  processo = Δcpu/(Δt*ncpu);
- barre per-core + RAM (dobbg), storico CPU totale ~19 s (doblg),
  tabella ordinata per CPU%% decrescente (insertion sort stabile: le
  righe non ballano) con selezione agganciata al PID (sopravvive al
  riordino);
- "Termina processo" dal pannello (o CANC) con conferma dobpopup_YesNo,
  PID 0 intoccabile; colonna [pin] per i driver inchiodati.
- Registrato in programs/Makefile; mkbootdisk lo raccoglie da solo.

File: kernel/proc/{thread.{c,h},scheduler.{c,h},process.h,tasksnap.h},
kernel/syscall/syscall.{c,h}, kernel/mm/vregion.{c,h},
libc/include/sys/syscall.h, libdob/include/dob/task.h,
programs/taskmgr/{main.c,manifest.dob}, programs/Makefile.
# MainDOB — priority inheritance CROSS-CORE + catena transitiva sicura (build 119)

Mancanza segnalata dal collaudo: il PI era diventato cieco oltre il
proprio core. Tre buchi concatenati chiusi, piu' uno latente trovato
per strada.

BUCO 1 — il boost non attraversava i core: scheduler_set_priority
aggiornava le code ma non pungolava MAI il core remoto. Un owner READY
boostato su un core occupato restava in coda fino al prossimo evento di
QUEL core (fino a un quanto, 2-20 ms): la stessa classe di latenza
uccisa in b114 per i wake, sopravvissuta per i boost — e il PI serve
proprio quando l'attesa e' urgente. Stesso rimedio: sul percorso di
boost di un READY, need_resched + IPI se il boostato batte strettamente
running_prio (letto sotto lo stesso sc->lock che lo pubblica); sulla
demozione di un RUNNING remoto (declassato sotto un pronto piu' alto),
IPI al suo core. Il giudice remoto decide comunque sotto il proprio
stato: IPI spuria innocua.

BUCO 2 — niente catena transitiva: A(alta)->M1(B), B bloccato su
M2(C bassa, tipicamente su un ALTRO core): C non veniva mai boostato e
l'inversione persisteva attraverso B. La catena della 1.1 iniziale era
stata rimossa per UAF (dereferenziava thread/mutex attraverso finestre
di free). Reintrodotta in forma SICURA: il waiter registra
waiting_owner_tid (VALORE, mai puntatore) sul proprio thread, sotto
m->guard — dove owner_tid e' gia' in mano; thread_boost_by_tid percorre
la catena di SOLO TID in TID, interamente sotto s_table_lock (lo stesso
lock che reap_detach prende prima del kfree: nessun thread della catena
liberabile mentre si cammina), senza mai dereferenziare mutex altrui.
TID stantio = al peggio un boost al thread sbagliato, inflazione
temporanea sanata dal recompute al suo unlock — mai un puntatore morto.
Profondita' max 8 (taglia anche i cicli di deadlock). Anello seguito
solo se l'owner e' BLOCKED su mutex.

BUCO 3 — il boost di un owner BLOCKED moriva li': coperto dalla catena
(e' esattamente l'anello intermedio).

LATENTE (trovato durante l'audit) — mutex_release_all_owned (rilascio
ORFANO nel reap) faceva wake_ONE: con piu' waiter su un mutex di un PCB
in teardown, i co-waiter non svegliati restavano BLOCKED per sempre su
una wait queue in via di kfree. Ora wake_ALL: rigiocano l'acquisizione,
chi trova il mutex sparito fallisce pulito.

NOTA LAYOUT: waiting_owner_tid e' in CODA a thread_t —
video_boomerang.asm deriva offset fissi (owner a 164, assert statico in
driver.c, che ha correttamente bloccato il primo tentativo di layout).

File: proc/thread.{c,h}, proc/scheduler.c, sync/mutex.c.
# MainDOB — work stealing: soglia di sovraccarico anti ping-pong (build 118)

Dal collaudo su live-CD + disco installato (b117): meccanismo di
migrazione corretto e race-clean (i driver migravano PRIMA della
registrazione IRQ e la linea seguiva la home nuova, poi pinned li
bloccava — come da progetto), ma la POLICY era troppo avida: ping-pong
sulle coppie IPC (DobFileSystem <-> MainDOB_Setup durante l'install,
clock/dobinterface a raffica). Dinamica: due processi in alternanza
IPC — in ogni istante un core lavora e l'altro, idle, vedeva
nr_ready==1 sul core occupato (il peer appena svegliato, in coda) e lo
strappava via, spezzando la coppia; poi i ruoli si invertivano. Il
cooldown per-processo teneva, ma con DUE processi alternati la
frequenza apparente raddoppiava.

CAMBIATO:
- PULL_MIN_READY = 2: si chiede lavoro solo a un core con ALMENO 2
  runnable in coda. Con 1 solo, girera' comunque entro <= 1 quanto sul
  suo core: rubarlo compra ms di latenza al prezzo della localita'. La
  soglia e' applicata su ENTRAMBI i lati — richiedente (idle_try_pull)
  e donatore (rivalutata sotto il proprio sc->lock: una richiesta
  stantia, col sovraccarico evaporato nel frattempo, si consuma a vuoto
  invece di strappare l'ultimo runnable).
- MIGRATION_COOLDOWN_NS: 50 -> 250 ms. Le migrazioni devono essere
  eventi correttivi rari, non un regime.

Effetto atteso: zero migrazioni durante ping-pong IPC a 2 processi
(nr_ready resta < 2); il furto scatta solo con contesa vera (>= 2 in
coda: benchmark + giochi + servizi sullo stesso core).

File: proc/scheduler.c.
# MainDOB — SMP: migrazione cooperativa a grana di processo, work stealing (build 117)

Intervento D, l'ultimo della campagna multicore (A+B b114, C b115,
tuning b116). Lo scheduler ora e' work-conserving anche a fronte di
squilibri nati DOPO il placement: un core senza lavoro lo chiede a chi
ne ha.

ARCHITETTURA — quattro pilastri, dall'analisi delle race (un microkernel
vive di IPC: il design e' dettato dalle race, non viceversa):
 1. SOLO LA HOME ri-homa i propri processi. Il core affamato CHIEDE
    (migrate_req + IPI di resched), mai ruba dalla coda altrui:
    "T e' homed su di me" cambia solo per azione mia. Il direct-switch
    IPC (check home==self sotto IF=0) resta corretto SENZA lock aggiunti
    sul percorso caldo.
 2. Grana di PROCESSO INTERO con nessun thread RUNNING (garantito per
    costruzione: solo la home esegue i suoi thread, e sta eseguendo il
    donatore, che appartiene a un altro processo). La vecchia home non
    ricarica mai piu' quel CR3, la nuova ridiventa l'unica: le
    invarianti di reap/finalize e thread_destroy sopravvivono intatte.
 3. I lettori di home_cpu diventano lock-recheck-retry
    (lock_home_queue: leggi home, prendi quel lock, RILEGGI; il flip
    avviene con ENTRAMBI gli sc->lock tenuti -> convergenza in <=2
    giri). Applicato a enqueue_home_and_kick, scheduler_remove,
    scheduler_set_priority.
 4. Esclusioni sotto il lifecycle-lock: pinned (nuovo flag PCB,
    kernel-set: driver con linee IRQ — la RTE IOAPIC punta alla home —
    e driver video, il cui CR3 e' prestato dal boomerang a thread di
    altri core), teardown rivendicato/accodato/armato, cadaveri in
    attesa di reap (la reap_list e' della vecchia home), cooldown
    anti-thrash 50 ms, processo del corrente.

RACE CHIUSE (le due non ovvie):
- Spawn vs migrazione: la home dei thread utente e' ora riletta dal
  processo DENTRO thread_publish, sotto il lifecycle-lock — lo stesso
  sotto cui il donatore flippa proc->home_cpu e percorre la lista. La
  finestra (home letta fuori lock -> flip -> publish con home stantia =
  due core sullo stesso CR3) non esiste piu'.
- Route del teardown vs migrazione: scheduler_route_process_teardown
  legge home sotto il lifecycle-lock; il donatore controlla
  destroy_claimed/queued/awaiting sotto lo stesso lock. O il router
  vede la home nuova, o il donatore si ritira. Nesting nuovo
  lifecycle->tw_lock, senza inverso (audit fatto).

MECCANICA:
- Richiedente (idle_try_pull, dal blocco idle prima dell'hlt,
  rate-limit 5 ms): vittima = core online piu' carico (metrica pesata)
  con nr_ready >= 1; CAS su migrate_req (una richiesta pendente per
  vittima) + IPI. cpu_idle e' gia' alzato sotto barriera: la consegna
  del donatore vede idle -> IPI garantita, sti;hlt fusi -> mai un
  risveglio perso.
- Donatore (scheduler_donate, dal giudice, IF spento dal check al
  ritorno: lifecycle-lock non rientrante): candidato = processo del
  primo READY a priorita' piu' alta che passa le esclusioni — massimo
  sollievo, e i partner IPC (BLOCKED, non in coda) non vengono mai
  scelti: la localita' client/server non si spezza. Trasferimento sotto
  lifecycle + entrambi gli sc->lock; nome/PID copiati prima del rilascio
  (fuori dal lock la nuova home puo' finalizzare il PCB). Log:
  "[SCHD] migrato PID .. 'nome': core X -> Y (pull)."
- Il protocollo running_prio/cpu_idle di b114 resta intatto: la
  consegna post-donazione usa la stessa barriera simmetrica.

File: proc/scheduler.{c,h via thread.h}, proc/thread.{c,h},
proc/process.h, syscall/driver.c.
# MainDOB — smistamento: metrica pesata, anti false-sharing, log del core (build 116)

Revisione di ottimizzazione dello smistamento introdotto in build 114.

TROVATO E CORRETTO — false sharing sulla sched_cpu_t: la 1.0 allineava
la struttura per-CPU a 64 byte, la riscrittura l'aveva perso. Senza
aligned(64), elementi adiacenti di s_cpu[] condividevano cache line
proprio sui campi piu' caldi (lock, run_queue, need_resched): ogni
acquire/enqueue di una CPU invalidava la linea della vicina, su OGNI
operazione di scheduling. Ripristinato: ogni sched_cpu_t su linea
propria.

MIGLIORATO — metrica di carico del placement: nr_homed da solo mentiva
(dieci server perennemente bloccati pesavano quanto dieci spinner
CPU-bound). Nuovo contatore nr_ready: profondita' runnable ISTANTANEA,
ESATTA (non advisory) — mutata sotto sc->lock negli stessi 4 punti che
toccano le code (enqueue_ready, pop di pick_next scarti inclusi,
scheduler_remove, riaccodo di set_priority con compensazione), costo
zero perche' la cache line e' gia' in scrittura. Audit: sched_node non
e' toccato da nessun altro file. Carico di placement =
nr_homed + 3*nr_ready (i runnable si contendono la CPU ORA, i homed
dormienti restano come pressione di fondo); slack invariato (2, in
unita' pesate).

AGGIUNTO — riga di spawn con lo smistamento in chiaro:
  [PROC] PID %d '<nome>' creato (core %u, spawnato da core %u).
Home == creatore -> la localita' ha tenuto; diversi -> il bilanciatore
ha sparso. La lettura del log basta a validare il placement dal vivo.

File: proc/scheduler.c, proc/process.c.
# MainDOB — SMP: sync TSC al bring-up, clamp globale fuori dal percorso caldo (build 115)

Intervento C della campagna multicore (A+B in build 114). Terza radice
del multicore-piu'-lento: OGNI clock_now_ns passava da un clamp monotono
globale (lock cmpxchg8b su una cache line condivisa) — e il clock si
legge in switch_to, slice check, time_event_refresh, timer, su ogni CPU.
La linea rimbalzava di continuo tra i core. In piu', con TSC sfasati, la
CPU "indietro" vedeva il tempo congelato al fronte globale: quanti e
timer slittavano.

Lo sfasamento ora e' corretto ALLA FONTE, il clamp resta solo dove serve
davvero.

AGGIUNTO:
- Sync TSC al bring-up (tsc_smp_sync_bsp/ap, arch/x86/tsc.c): per ogni
  AP, handshake ping-pong a round numerati guidato dalla BSP; offset
  stimato dal campione a round-trip MINIMO (errore residuo <= rtt/2,
  frazioni di us). Pubblicazione a meta' 32-bit con release TSO (store
  del contatore per ultimo: mai un 64-bit strappato su i386). L'offset
  e' ratificato dall'AP nel PROPRIO slot di _tsc_cpu_off, UNA volta,
  PRIMA di entrare nello scheduler: dopo, nessuno scrive piu'. Timeout
  su entrambi i lati: sync mancato = offset 0 + log, mai boot bloccato.
  Rendezvous in smp.c: AP subito dopo il check-in, BSP subito dopo
  wait_checkin — bring-up seriale, un canale condiviso basta.
- _tsc_local_cycles(): RDTSC + offset per-CPU, il "cicli" canonico di
  tutto il layer ns (_tsc_ns_read). Su UP: RDTSC nudo, zero costi.
- tsc_cycles_to_raw(): riconversione a cicli GREZZI al filo
  dell'hardware. CONTRATTO: le interfacce in cicli viaggiano CORRETTE;
  l'unico consumatore del grezzo e' la scrittura di IA32_TSC_DEADLINE
  (lapic_arm_tscd), che confronta col TSC nudo della CPU.

CAMBIATO:
- clock_now_ns (time/clock.h): NIENTE piu' clamp globale — monotono
  per-CPU puro, nessuno stato condiviso in scrittura sul percorso
  caldo. Le logiche a deadline fanno solo confronti ora>=scadenza (skew
  residuo = frazioni di us, mai underflow); trascorso=ora-inizio e'
  per-thread, quindi per-CPU (thread pinnati). Verificato: nessun
  consumatore fa aritmetica di elapsed cross-CPU.
- clock_now_ns_global() (nuovo, percorso FREDDO): monotono globale
  STRETTO via clamp, riservato al wall-clock di SYS_GETTIME
  (clock_get_realtime): il tempo civile non arretra mai tra processi su
  core diversi, nemmeno di un ns.
- lapic_timer_arm_ns: coppia (cicli, ns) coerente da tsc_snapshot al
  posto del mix tsc_read() grezzo + ns derivati (innocuo prima
  dell'offset, sbagliato dopo).

VM DI TEST: ripristinato il profilo 1.0 nel Makefile — QEMU_SMP=2,
macchina q35, CPU con APIC (QEMU_CPU_APIC). Il profilo legacy PIC-only
resta a un override: make run QEMU_CPU=$(QEMU_CPU_NOAPIC)
QEMU_MACHINE=$(QEMU_MACHINE_PC) QEMU_SMP=1.

File: arch/x86/tsc.{c,h}, arch/x86/smp.c, arch/x86/lapic.c,
time/clock.{c,h}, Makefile.
# MainDOB — SMP: wake-IPI verso CPU occupate + placement con localita' (build 114)

Diagnosi del "multicore piu' lento del singlecore": lo scheduler non era
work-conserving. Due radici, entrambe chiuse qui (interventi A+B; C=clock,
D=work stealing seguiranno).

RADICE 1 — wake cross-core senza IPI se il bersaglio era occupato:
enqueue_home_and_kick mandava l'IPI SOLO verso CPU idle. In modo eventi una
CPU occupata non riceve interrupt fino alla scadenza del proprio quanto:
un wake anche di priorita' SUPERIORE aspettava fino a 2-20 ms. Con l'IPC
di un microkernel = latenze a valanga.

RADICE 2 — placement round-robin cieco + pinning senza migrazione:
client e server finivano su core diversi, il direct-switch IPC (solo
same-CPU) non scattava quasi mai, e ogni messaggio pagava la Radice 1.

AGGIUNTO:
- sched_cpu_t.running_prio: priorita' del thread SCELTO per la CPU,
  scritta ESCLUSIVAMENTE sotto sc->lock da ogni decisore (reschedule,
  scheduler_yield_to, scheduler_set_priority sul running).
  SCHED_NUM_PRIORITIES = idle. Il waker remoto la legge fuori lock ma
  DOPO la propria sezione di enqueue sullo stesso lock: o la decisione
  remota precede (release TSO -> valore fresco visibile), o segue (il
  pick vede gia' il thread accodato e, se outranks, lo sceglie da se').
  Nessun wake outranking perso; al peggio una IPI spuria, smontata dal
  giudice sotto il proprio lock.
- enqueue_home_and_kick: IPI se cpu_idle OPPURE t->priority <
  running_prio. Latenza del wake outranking: da <=1 quanto a ~una IPI.
- scheduler_yield_to: sezione decisore unificata sotto sc->lock per
  ENTRAMBI gli esiti (READY e BLOCKED — prima il ramo BLOCKED non
  prendeva il lock: la pubblicazione l'ha reso necessario).
- sched_cpu_t.nr_homed: bilancio ADVISORY dei thread homed per CPU
  (inc/dec atomici, letture senza lock; gara = piazzamento subottimale,
  mai errore). Aggancio simmetrico per costruzione: thread_publish (+1,
  idle esclusi) / reap_detach (-1), sotto lo stesso s_table_lock.
- scheduler_pick_cpu_local (nuovo default dei processi): CPU del
  CREATORE, salvo squilibrio oltre SCHED_PLACEMENT_SLACK (2) rispetto
  alla CPU online meno carica (allora quella). Le catene IPC restano
  locali -> il direct-switch torna a scattare; i server di boot si
  spargono comunque appena il creatore supera lo slack.
  scheduler_pick_cpu (round-robin) resta come distributore neutro.

File: proc/scheduler.{c,h}, proc/thread.c, proc/process.c.
# MainDOB — panic universale: disegno nel FRAMEBUFFER attivo (approccio B) (build 113)

Abbandonato il mode-switch VGA (fragile, hardware-specifico, non copre il
ferro non-BGA come l'x3100 LVDS). Il panic ora disegna il testo direttamente
nei pixel del framebuffer GIA' acceso dalla GUI: universale (qualunque scheda
con un framebuffer), niente cambio modo.

RIMOSSO (pulizia richiesta): tutto il codice VGA del panic dei build 103-112
-- bga_disable, vga_misc/seq/crtc/gc/ac/dac_program, vga_set_text_mode_3,
video_force_text_mode, e i dump diagnostici (vga_state_dump ecc.).

AGGIUNTO:
- kernel/console/panic_font.c: font 8x16 (copia di libdob dob_font_data).
- console.c, sezione panic riscritta in stile modulare:
  * esecutivi: fb_put_pixel (32/24/16 bpp), fb_fill, fb_draw_glyph;
  * orchestratori: fb_emit (un carattere), kpanic (prepara schermo, emette
    header+messaggio+coda). panic_emit instrada: sempre seriale; a schermo
    nel framebuffer se registrato, altrimenti testo VGA (panic al boot).
  * console_register_panic_fb: mappa il framebuffer fisico in VA kernel
    (mmio_map, regione condivisa -> valida al panic in ogni contesto).
- Syscall SYS_SET_PANIC_FB (117), solo driver: registra fisico+geometria.
- Driver BGA: in bga_apply_mode registra vram_phys+primary_offset[0], w, h,
  pitch=w*4, 32bpp.

Font e drawing coprono 32/24/16 bpp. Ferro non-BGA (x3100/mach64): baste che
il loro driver chiami SYS_SET_PANIC_FB col proprio framebuffer -> stesso
percorso, universale. File: console/console.{c,h}, console/panic_font.c,
syscall/syscall.{c,h}, libc syscall.h, drivers/bga/main.c.
# MainDOB — panic: dump VGA POST-reprogram (build 112)

Dai dati del build 111 (VGADUMP PRE): la GUI gira a 1024x768x32 via BGA LFB e
i registri CRTC sono in config grafica (crtc01=7F, crtc09=40, crtc13=00). La
compressione del testo punta a crtc13 (offset/pitch) = 0.

Questo build aggiunge un secondo dump [VGADUMP POST] DOPO il reprogram, per
sapere se i write ai registri attaccano: se crtc13 diventa 28 il valore e'
giusto (problema di rendering QEMU), se resta 00 QEMU ignora i write (e so
dove intervenire). Nessun'altra modifica: dato pulito. File: console/console.c.
# MainDOB — panic: ripristino sfondo rosso (riprogramma senza seq-reset) + dump diagnostico VGA/BGA (build 111)

Dai test: solo disable-BGA (109) o disable+AC (110) -> schermo NERO (0xB8000
non mostrato). Il riprogramma registri (105) faceva comparire il buffer
(sfondo rosso) col testo visibile; il seq-reset (106) lo rendeva invisibile.

- Ripristinato il riprogramma mode 3 SENZA il reset del Sequencer (torna lo
  sfondo rosso col testo visibile) + DAC per i colori corretti.
- Aggiunto vga_state_dump(): in kpanic, PRIMA del reset, legge e stampa su
  seriale i registri reali (Misc, seq1, CRTC h-display/max-scan/offset, e
  BGA enable/xres/yres/bpp). Serve a correggere la geometria compressa con
  DATI VERI invece di indovinare i registri alla cieca.

Prossimo passo: dal serial log del prossimo test leggo [VGADUMP] e sistemo
la geometria con precisione. File: console/console.c.
# MainDOB — schermata di panic: disable-BGA + riabilita video (AC), niente riprogramma geometria (build 110)

Cosa hanno detto i test:
- 105 (disable-BGA + riprogramma registri mode 3): rosso mostrato, testo COMPRESSO.
- 109 (solo disable-BGA): schermo NERO (buffer 0xB8000 non mostrato).

Lettura: dopo il disable-BGA la VGA resta blanked; serve un tocco a un
registro per riabilitare il video e far ri-renderizzare QEMU. Ma riscrivere
CRTC/clock (105) rompeva la geometria. Il boot lascia gia' mode 3 corretto.

Fix: video_force_text_mode ora fa bga_disable + vga_video_enable + clear +
cursore. vga_video_enable riabilita SOLO l'uscita video via Attribute
Controller (0x3DA flip-flop, poi 0x3C0 <- 0x20): fa ri-render a QEMU con la
geometria del boot (intatta), senza toccare CRTC/clock -> niente compressione.
File: console/console.c. Ferro non-BGA nativo: follow-up.
# MainDOB — schermata di panic: solo disable-BGA, niente riprogramma VGA (build 109)

Diagnosi: il riprogramma manuale dei registri VGA (mode 3 completo) ROMPEVA
il modo testo che il boot aveva gia' impostato pulito. Regressione osservata:
build 105 (senza seq-reset) testo compresso ma visibile; dal 106 (seq-reset +
full mode-3) schermo tutto rosso, testo invisibile -- non era il colore (il
DAC del 108 non ha aiutato) ma la GEOMETRIA rotta dal riprogramma.

Fix: video_force_text_mode ora fa SOLO bga_disable + clear + cursore. Su QEMU
la GUI accende solo l'overlay VBE_DISPI senza toccare i registri testo VGA
(mode 3 dal boot), quindi disabilitare BGA basta a tornare al modo testo
pulito. Rimosso tutto il riprogramma registri (misc/seq/crtc/gc/ac/dac).

Follow-up onesto: il ferro non-BGA in modo grafico NATIVO (mach64/x3100) non
e' coperto da questo -- servira' un reset registri VGA fatto bene e testato
sul ferro, separatamente. File: console/console.c.
# MainDOB — schermata di panic: carica il DAC (colori del testo) (build 108)

Sintomo: schermata di panic tutta rossa, testo INVISIBILE. Causa: il mode-set
VGA programmava Attribute Controller (attributo->indice palette) ma NON il
DAC (indice palette->RGB). Cosi' i colori dipendevano da cio' che la GUI/BGA
aveva lasciato nel DAC: lo sfondo cadeva su rosso ma il "bianco" del testo su
un rosso quasi identico -> testo invisibile.

Fix: nuovo blocco esecutivo vga_dac_program() che carica la palette VGA a 16
colori nel DAC (indici 0..15, valori 6-bit), chiamato da vga_set_text_mode_3.
E AC portato a mappatura IDENTITA' (attributo i -> indice DAC i) cosi' la
catena attributo->indice->colore e' pulita. Ora bianco su rosso e' leggibile.
File: console/console.c.
# MainDOB — refactor del reset video di panic nel paradigma modulare (comportamento invariato) (build 107)

Solo forma, zero cambi di comportamento: il codice VGA del panic era
monolitico. Ristrutturato in blocchi esecutivi (un lavoro ciascuno, port I/O
diretto) sopra + orchestratori-prosa sotto, come il resto del kernel.

- Blocchi esecutivi: bga_disable, vga_misc_set, vga_seq_program (col reset
  sincrono), vga_crtc_program (sbloccato), vga_gc_program, vga_ac_program,
  vga_text_clear. Ogni tabella di registri vive nel suo blocco.
- Orchestratori: vga_set_text_mode_3 (misc -> seq -> crtc -> gc -> ac) e
  video_force_text_mode (bga_disable -> set mode 3 -> clear -> sync cursore).

Stesse scritture di registro, stesso ordine, stesso risultato del build 106
(incluso il fix del reset sincrono del Sequencer). File: console/console.c.
# MainDOB — fix geometria testo nella schermata di panic (reset sincrono del Sequencer) (build 106)

Il build 105 ha reso VISIBILE la schermata di panic (rossa, modo testo) con la
GUI/BGA attiva -- il freeze e' risolto. Ma il testo appariva SCHIACCIATO in una
scheggia sul bordo sinistro: geometria orizzontale sbagliata.

Causa: vga_set_text_mode_3 scriveva i registri del Sequencer SENZA asserire
prima il reset sincrono. Cambiare il dot-clock (Misc Output) mentre il
sequencer gira non fa applicare pulito il nuovo clock -> la scheda tiene il
clock alto della risoluzione BGA precedente e il testo mode 3 viene reso a
quel clock -> compressione orizzontale.

Fix: assereisci il reset sincrono del Sequencer (SEQ0 <- 0x01), carica SEQ 1-4,
poi rilascia il reset (SEQ0 <- 0x03). Sequenza standard di mode-set VGA.
File: console/console.c.
# MainDOB — strumento di test: syscall SYS_PANIC_TEST + companion 'panictest' (build 105)

Per verificare la schermata di panic (video_force_text_mode del build 103/104)
senza saper generare un panic a comando:
- Kernel: nuova syscall SYS_PANIC_TEST (116) -> sys_panic_test() fa un kpanic()
  deliberato. NON gated (strumento di test). syscall.h kernel + libc mirror.
- Companion: programs/panictest (NO_DOBUI) chiama syscall0(SYS_PANIC_TEST).
  mklive.sh lo copia in /SYSTEM/PROGRAMS/panictest/panictest.mdl in automatico
  (loop su build/programs/*.mdl).

USO: con il desktop attivo, apri /SYSTEM/PROGRAMS/panictest/panictest.mdl da
DobFiles (come usbdiag). Il kernel andra' in panic e la schermata rossa di
panic deve COMPARIRE in modo testo (se video_force_text_mode funziona), invece
di lasciare la GUI congelata.
File: kernel/syscall/syscall.{c,h}, libc/include/sys/syscall.h,
programs/panictest/main.c, programs/Makefile.
# MainDOB — panic video completo (reset VGA mode 3 per ferro non-BGA) + DobDisk lanciato come PROGRAMMA, non driver (build 104)

== 1) PANIC VIDEO: reset registri VGA a mode 3 (ferro reale) ==
Completato video_force_text_mode: dopo aver disabilitato BGA (che copre
QEMU/Bochs/VBE), ora resetta anche i registri VGA al modo testo 3 standard
(Misc Output, Sequencer, CRTC con sblocco reg 0x11, Graphics e Attribute
Controller). Copre il ferro non-BGA VGA-compatibile (mach64/x3100 in modo
nativo) dove 0x1CE e' un no-op. NESSUNA ricarica font: il fixedsys 8x16 del
BIOS vive nel piano 2 e MainDOB non usa mai modi VGA planari (0x12/0x13)
che lo sovrascrivano -> resta intatto. File: console/console.c.

== 2) DobDisk: lanciato come PROGRAMMA (spawn_file), non driver ==
Il flusso "Formatta" passava a DobDisk le coordinate ma non apriva una
finestra. Causa: la primitiva DAS 'spawn' promuove SEMPRE a driver
(spawn_file_driver -> make_driver), giusto per driver e programmi di
controllo hardware, ma sbagliato per un'app a finestra: il processo gira e
riceve i parametri, ma non ottiene una finestra da dobinterface.
Fix: nuova primitiva DAS 'run' che usa spawn_file (nessuna promozione) — il
percorso con cui parte ogni altro programma GUI. usb_mass_storage.das:
'spawn DobDisk...' -> 'run DobDisk...'. DobDisk non ha bisogno di privilegi
driver (parla con usbms via IPC e con dobinterface, non fa port I/O).
File: boot/hotplug/das.c, config/DAS/usb_mass_storage.das.

NON COMPILATO qui: verifica che "Formatta" ora APRA la finestra DobDisk da
sola; e che un panic con GUI attiva mostri la schermata testo (QEMU e, se
hai modo, ferro reale).
# MainDOB — fix lockup mutex PI (opzione A) + panic video visibile (torna a modo testo, come GRUB) (build 103)

== 1) LOCKUP: priority-inheritance dei mutex fuori dal guard (opzione A) ==
Radice del kernel panic "spinlock LOCKUP ... mai rilasciato": era l'UNICO
path del kernel che teneva un lock heap (m->guard) attraverso una
chiamata-fuori. mutex_unlock e lock_internal chiamavano recompute/boost
(-> scheduler_set_priority -> sc->lock) SOTTO m->guard, e boost_owner_chain
camminava la catena di ownership dereferenziando thread_t risolti per TID
SENZA refcount. Sotto la raffica di morti/respawn (ora che il flusso
"Formatta" arriva in fondo), quell'owner poteva essere reaped su un altro
core tra il lookup e il dereference -> use-after-free MENTRE si tiene
m->guard -> guard mai rilasciato -> lockup, e panic freezato.

Fix (opzione A, il principio che il kernel usa ovunque: muta sotto lock,
rilascia, poi chiama fuori):
- mutex_unlock: rilascia m->guard PRIMA di recompute_owner_priority(self)
  (self e' current_thread, vivo -> no UAF, no scheduler sotto guard).
- lock_internal: registra il boost + fotografa owner_tid sotto guard,
  rilascia, poi applica il boost del SOLO owner diretto fuori dal guard.
- Nuovo thread_boost_by_tid(tid, prio): risolve il TID e applica sotto
  s_table_lock -- lo STESSO lock che reap_detach prende per azzerare la
  entry PRIMA del kfree -> il thread non puo' essere liberato durante il
  boost -> UAF impossibile. No-op se il TID e' sparito.
- Rimosso boost_owner_chain (catena transitiva col deref UAF). Costo:
  niente PI transitiva A->B->C (rara; al peggio inversione residua, non un
  bug di correttezza) -- enormemente meglio di un lockup del kernel.
File: sync/mutex.c, proc/thread.c, proc/thread.h.

== 2) PANIC VIDEO: torna a un modo testo visibile ==
Perche' il panic "freezava" senza schermata: kpanic scrive in modo testo
VGA (0xB8000) + seriale, ma con la GUI attiva la scheda e' in modo grafico
BGA -> il testo va in un buffer NON mostrato (per questo il messaggio c'era
solo nel log seriale). Il kernel non aveva alcun reset video.
Fix: nuova video_force_text_mode() chiamata da kpanic PRIMA di emettere --
disabilita BGA (VBE_DISPI ENABLE<-0 via 0x1CE/0x1CF) riportando QEMU/Bochs
e le schede VBE-compatibili in modo testo VGA (mode 3, "a livello BIOS",
come GRUB), pulisce il buffer e azzera il cursore. Solo port I/O diretto,
niente lock ne' driver userspace: sicuro nel path di panic.
Caveat: copre QEMU/BGA (dove e' avvenuto il crash) e schede VBE-compatibili.
Per il ferro non-BGA (mach64/x3100 in modo nativo) servirebbe anche un
reset completo dei registri VGA a mode 3 + ricarica font: follow-up.
File: console/console.c.

NON COMPILATO qui: verifica boot, flusso Formatta a raffica (niente piu'
lockup), e -- forzando un panic con la GUI attiva -- la schermata di panic
ora VISIBILE su modo testo.
# MainDOB — MAX_PROCESSES di nuovo 65535 (finestra anti-ABA ampia); crash da diagnosticare a parte (build 102)

Ripristinato MAX_PROCESSES=65535: la finestra di sicurezza anti-ABA con
policy monotona e' proporzionale a MAX (un PID non torna finche' il cursore
non percorre tutto lo spazio), quindi 65535 e' 64x piu' robusto di 1024 su
sessioni lunghe. Monotono ricicla comunque (fa il wrap e riusa gli slot
liberati), quindi non esaurisce a 65535 vivi.

Il lockup spinlock del build 100/101 e' un bug SEPARATO: insert/remove
rilasciano il lock su ogni percorso, lo scan monotono ha bordi corretti, e
il lock della tabella processi e' statico (non heap). Va identificato
risolvendo gli indirizzi del panic (0xc0117a98 chiamante, 0xd00b64bc lock)
contro la symbol map, non cambiando la dimensione.
File: proc/process.h.
# MainDOB — MAX_PROCESSES 65535 -> 1024 (come 1.0). Mitiga il possibile lockup del build 100; monotono invariato (build 101)

Il build 100 ha causato un KERNEL PANIC intermittente: "spinlock LOCKUP:
lock 0xd00b64bc mai rilasciato". Il lock e' nell'HEAP (0xd00xxxxx), nella
regione dove MAX_PROCESSES=65535 faceva allocare all'handle table ~768 KB
(3 array x 256 KB) all'inizio dell'heap. Sintomi a valle: usb_uhci e inputd
mute (non riescono a girare col sistema inchiodato), schermata VGA di panic
assente (il gestore panic prende lo stesso lock inchiodato -> freeze).

65535 era un salto di 256x eccessivo. Il 1.0 usava monotono + 1024 ed era
stabile: 1024 da' la STESSA protezione anti-ABA (un PID liberato non torna
per ~1024 allocazioni; il boot ne fa ~28) con allocazione piccola e scan
monotono corto (max 1023 iter sotto lock invece di 65534). La policy
HT_REUSE_MONOTONIC resta invariata.

NB: questo e' il fix a rischio-basso e piu' probabile, ma il lock inchiodato
va identificato con certezza: risolvere 0xc0117a98 (chiamante) e 0xd00b64bc
(lock) contro la symbol map del build per puntare il sottosistema esatto.
File: proc/process.h (una riga).
# MainDOB — RADICE: allocazione PID monotona (come 1.0) + MAX_PROCESSES 65535. Fine dell'ABA bolla-hotplug che rendeva usb_uhci muto (build 100)

Radice VERA, provata dal commento stesso di hotplug (main.c 1.1): un driver
muore e respawna durante il boot, il suo PID viene RICICLATO a usb_uhci
prima che il reaper ritiri la bolla hotplug stantia che ancora lo riferisce
-> la bolla stantia INTERCETTA l'attach e dirotta usb_uhci sul device del
morto (il BDF dell'AHCI) -> usb_uhci pilota l'hardware sbagliato, non vede
mai gli interrupt UHCI -> MUTO sulla linea condivisa -> bring-up di
usb_mass_storage incompleto -> usbms_N assente -> DobDisk non vede l'USB ->
"Formatta" non parte. Il 1.0 non lo aveva perche' NON ricicla i PID in
fretta (next_pid monotono + MAX_PROCESSES=1024); il 1.1 li riciclava
(HT_REUSE_FIFO + MAX_PROCESSES ristretto a 256).

FIX (Opzione 1, kernel, alla radice): allocazione PID monotona come 1.0.
- krt/handle_table: nuova policy HT_REUSE_MONOTONIC. Un id liberato torna
  in gioco SOLO dopo che il cursore rotante (next_scan, come next_pid del
  1.0) ha percorso l'intero spazio -> ritardo di riuso massimo e costante,
  a prescindere da quanti slot siano liberi. Cosi' la bolla stantia e'
  SEMPRE reaped prima del riuso del PID.
  Rifattorizzato nel paradigma: blocchi esecutivi (id_take_fifo/lifo/
  monotonic, id_return_fifo/lifo, slot_publish) sopra; orchestratori
  handle_table_insert/remove sotto, che scelgono il percorso per policy.
  LIFO/FIFO invariati per chi li usa (shm resta LIFO).
- proc/process.c: la tabella processi passa a HT_REUSE_MONOTONIC.
- proc/process.h: MAX_PROCESSES 256 -> 65535 (finestra ampia; pid_t e'
  int32, nessun array statico [MAX_PROCESSES], ~512KB via kpages large).

Isolamento: REVOCATO Plan A del build 99 (consegna PIC di default) e
rimossa la diagnostica del build 98 in pirq.c. La consegna torna IOAPIC
(default 1.1), cosi' il fix e' testabile da solo e si tiene lo steering
SMP. Il fix vale a prescindere dalla consegna: usb_uhci ora aggancia il
device GIUSTO in entrambi i modi.

NON tocca: driver, hotplug (le sue guardie anti-ABA restano, ridondanti),
forwarding, scheduler, delivery. Solo l'allocazione degli id.
NON COMPILATO qui: verifica boot (legacy PIC e q35 IOAPIC), poi inserimento
pendrive non formattata -> "Formatta" deve aprire DobDisk CON l'unita' USB,
formattabile FAT32/exFAT. usb_uhci non piu' "mute" sulla linea condivisa.
File: krt/handle_table.{c,h}, proc/process.{c,h}, + revert intr.{c,h}/
kernel.c/Makefile (Plan A) e pirq.c (diagnostica).
# MainDOB — consegna IRQ dispositivo di default sul PIC 8259 (come 1.0); IOAPIC opt-in. Fix starvation usb_uhci su GSI condivisa (build 99)

Radice (diagnosticata su ferro, build 98): ahci (0:4.0, INTA) e usb_uhci
(0:5.0, INTD) collassano DAVVERO sulla stessa PIRQE -> GSI 20 (lo swizzle
INTx->PIRQ->GSI e' corretto, NON un bug: verificato). Su quella GSI
condivisa la sequenza EOI-prima-di-mask delle linee level dell'IOAPIC
(intr_dispatch) affama il driver piu' lento: usb_uhci resta muto, i suoi
transfer si strozzano, il bring-up di usb_mass_storage non completa,
usbms_N non si registra nel block layer, DobDisk non vede l'unita' e il
flusso "Formatta" non parte. Il kernel 1.0 non aveva il problema perche'
restava sul PIC per le IRQ dispositivo (IOAPIC mappato+mascherato).

FIX (Piano A, il piu' sicuro e fedele): la migrazione all'IOAPIC diventa
OPT-IN. Default = consegna PIC, come 1.0.
- Nuovo orchestratore intr.c: device_irq_delivery_setup() (in fondo,
  prosa) decide il percorso per caso: (1) IOAPIC_DELIVERY a build + IOAPIC
  presente -> migra; (2) nessun IOAPIC (legacy, es. Armada) -> PIC; (3)
  IOAPIC presente ma non richiesto (DEFAULT) -> PIC. Chiama il blocco
  esecutivo intr_switch_to_ioapic() solo nel caso 1.
- kernel.c: il boot chiama device_irq_delivery_setup() invece della
  migrazione diretta.
- Makefile: `make ... IOAPIC_DELIVERY=1` per l'opt-in (steering per-CPU +
  MSI su ferro sano). Default off.

Perche' e' sicuro: il layer driver e' GIA' bimodale (sys_irq_register_pci
risolve via Interrupt Line in modo PIC, via chipset in modo IOAPIC), ed e'
il percorso PIC che gira gia' su tutto l'hardware legacy senza APIC.
Zero tocchi al PIC/legacy, allo scheduler SMP (gli IPI di reschedule
usano il LAPIC, non l'IOAPIC), al forwarding, ai driver. Su QEMU q35
ahci e usb_uhci leggono Interrupt Line che il chipset puo' assegnare a
linee 8259 diverse -> separati, niente flood.

Costo: su SMP con default (PIC) le IRQ dispositivo vanno al BSP invece che
steerate. 1.0 faceva cosi'. Chi vuole lo steering e ha consegna condivisa
sana usa IOAPIC_DELIVERY=1.

Rimossa la diagnostica temporanea del build 98 in pirq.c.
File: arch/x86/intr.c, arch/x86/intr.h, kernel.c, irq/pirq.c, Makefile.
NON COMPILATO qui: verifica boot (legacy PIC e q35), inserimento pendrive
non formattata -> "Formatta" -> DobDisk deve vedere l'unita' USB.
# MainDOB — DIAGNOSTICA: log della decisione INTx->PIRQ->GSI per device (build 98, temporanea)

Passo 0 del Piano B (fix risoluzione INTx per la condivisione ahci+usb_uhci
su GSI 20). Aggiunta UNA riga in pirq_resolve_gsi che stampa, per ogni
device risolto: slot.func, pin INTx, via (DxxIR integrato / swizzle slot
esterno), PIRQ e GSI finale.

Scopo: 0:4.0 (ahci) e 0:5.0 (usb_uhci) risultano entrambi su GSI 20. Lo
swizzle 'pirq = 4 + ((slot+pin-1)&3)' darebbe, a pin uguali (INTA), GSI 20
e 21 -> DIVERSI. Se il log conferma pin uguali ma GSI uguali, il collasso
e' un BUG dello swizzle da correggere; se i pin differiscono (es. uhci su
INTD) e' condivisione vera e si passa al Piano A.

Nessun cambio di comportamento: solo log. Da rimuovere col fix definitivo.
File: irq/pirq.c.
# MainDOB — REVOCA della guardia IPC anti-ABA del build 96 (regressione: pipeline di rilevamento USB) (build 97)

Il build 96 aveva aggiunto una guardia in ipc_reply_staged che consegnava
la reply solo se il processo rispondente possedeva la porta-bersaglio del
sender. Era un fix SPECULATIVO su un sottosistema che funzionava. Ha
introdotto una regressione: una pendrive USB non viene piu' rilevata
(nessun usb_mass_storage, nessuna icona DAS) perche' un anello IPC della
pipeline di annuncio riceveva la propria reply SCARTATA e si piantava.

Cause plausibili del falso-scarto (non serve stabilirla per revocare):
delega della reply (un front-end riceve, un back-end risponde), reply
generata in contesto kernel, o un pattern porta/proprietario diverso da
quello ipotizzato. Il sottosistema IPC era gia' corretto; la guardia era
di troppo.

FIX: revocata la guardia. ipc_reply_staged torna identico al 1.0/92:
routing per s_pending[sender_tid] senza il controllo di proprieta'.
channel.c e' ora byte-identico all'originale. Il buco ABA-su-TID resta
teoricamente aperto (rarissimo, richiede riciclo TID + reply in ritardo);
se mai servira' chiuderlo si fara' con un token d'incarnazione opaco nel
sender_tid, NON con una guardia lato-proprietario.

Restano intatti: scheduler event-driven (93), forwarding IRQ senza
deaf-strike (94), early-unmask opt-in (95).

File: ipc/channel.c (revert puro).
# MainDOB — IPC: guardia anti-ABA sul TID nella consegna della reply (build 96)

Guasto (latente, presente anche in 1.0): la reply veniva instradata per
s_pending[sender_tid] GREZZO, senza incarnazione del thread. Se un TID
veniva riciclato tra la morte/fine del sender originale e una reply in
ritardo, un thread NUOVO che riusa quel TID (bloccato su una propria
call) riceveva nel suo buffer la reply del vecchio: dato giusto, thread
sbagliato -> app che "impazzisce". Tipico chiudendo un'app mentre e' in
mezzo a una RPC verso un server lento.

FIX (kernel-only, nessun cambio ABI, nessun tocco ai server): blocco
esecutivo reply_matches_sender() + guardia nell'orchestratore
ipc_reply_staged. La reply si consegna SOLO se il pending trovato aspetta
davvero una risposta DA NOI, cioe' il sender aveva indirizzato una porta
che il processo rispondente possiede (id + generazione, via
ipc_port_get_checked). Una reply legittima viene sempre dal server a cui
si e' spedito -> passa sempre. Una reply mis-instradata su TID riciclato
verso un ALTRO server -> cade (IPC_ERR_INVALID), niente scrittura nel
buffer ne' wake. Vale anche come guardia: solo il proprietario di una
porta ne serve le richieste, quindi solo lui puo' rispondere (niente
reply-spoofing da non-proprietari).

Ordine di lock invariato (pending -> global, come ipc_cleanup_thread);
la guardia e' PRIMA di ogni scrittura/wake; il payload posseduto viene
liberato sul drop.

LIMITE onesto: chiude la mis-consegna CROSS-server (il caso pratico:
app uccisa mid-RPC, TID riciclato a un thread che parla con un altro
server). Resta un buco stretto SAME-server (stesso TID riciclato, stesso
identico server, reply in ritardo): per chiuderlo serve un token
d'incarnazione che faccia round-trip nel sender_tid (opaco, echeggiato
dal server) — cambio che richiede audit dei server, rimandato.

File: ipc/channel.c. NON COMPILATO qui: verifica boot + IPC sotto carico
(apri/chiudi app che fanno RPC); nessuna reply legittima deve sparire.
# MainDOB — forwarding IRQ: early-unmask event-driven quando la sorgente riporta l'interrupt come suo (build 95)

Contesto: su linea INTx condivisa (es. GSI 20 = ahci + usb_uhci) il giro
si chiudeva solo quando OGNI sharer aveva riportato irq_done. Un device
veloce (ahci) restava quindi mascherato — throttlato fino a
IRQ_DONE_TIMEOUT_MS per fire — in attesa dell'ack di un vicino lento
(usb_uhci mentre enumera). Tassa di latenza sul boot e sull'I/O.

Perche' non "early-unmask on line-deassert": l'IOAPIC non espone una
query sincrona affidabile "la linea e' ancora assertita?" (il Remote-IRR
non lo dice). Sondarla reintrodurrebbe l'euristica-polling appena tolta.

FIX (pulito, event-driven): la sorgente lo sa se l'interrupt era suo. Se
lo riporta, il kernel chiude il giro SUBITO — niente attesa dei vicini.
Un driver che ha servito il proprio device chiama SYS_IRQ_DONE con
ecx == IRQ_DONE_MINE ('MINE', 0x4D494E45); irq_line_ack chiude il giro
(cancel timer + unmask) a prescindere da pending_done. Un secondo device
sulla stessa linea si auto-sana col re-fire level: un giro pulito per
sorgente, nessuno storm, nessuna euristica.

Backward-safe: il sentinel e' un magico a 32 bit, non un bool. Un driver
legacy che lascia garbage in ecx non lo azzecca (collisione ~2^-32) e
mantiene la semantica wait-for-all di prima. Nessun nuovo syscall: l'ABI
resta a 128, 1:1 con 1.0.

Per averne beneficio i driver vanno aggiornati a passare ecx=IRQ_DONE_MINE
quando l'IRQ era del proprio device (una riga). Finche' non lo fanno, il
comportamento e' invariato. Alternativa strutturale per i device
MSI-capaci (ahci): passare a MSI (edge, nessuna mask-until-service) li
toglie del tutto dalla linea condivisa — opt-in lato driver.

File: syscall/driver.c (solo irq_line_ack / sys_irq_done + il define).
NON COMPILATO qui: verifica boot + I/O disco; con un driver aggiornato,
il device che riporta MINE non deve piu' attendere il vicino.
# MainDOB — forwarding IRQ condiviso: via il deaf-strike (falso-sgancio di driver sani); restructure a blocchi con orchestratori-prosa in fondo (build 94)

Sintomo: dopo il disinnesto di una pendrive l'icona DAS resta fantasma e
le finestre non si chiudono; ferro e QEMU. Diagnosi (usbdiag): l'hardware
vede il disconnect (PORTSC CCS=0, CSC latchato), ma la FSM del driver UHCI
resta a "Dispositivo pronto" — non viene svegliata a processarlo.

CAUSA (kernel, non driver — usb_uhci e' identico a 1.0): su linea IRQ
condivisa e trafficata il forwarding mascherava e attendeva irq_done da
ogni sharer entro 50 ms; chi mancava accumulava uno strike e a 8 veniva
SGANCIATO dalla linea. Sotto i wake persi di pre-93 un driver SANO ma
schedulato in ritardo mancava le scadenze, veniva marcato sordo e tagliato
fuori dalla linea -> non riceveva piu' alcuna IRQ, inclusa la resume del
disconnect -> FSM congelata, icona permanente. Il difetto: un unico
timeout faceva tre mestieri (liveness di linea, rilevamento morte,
unmask), e il #2 (morte via cronometro) falsava i sani.

FIX:
- Eliminato il deaf-strike: il safety timer resta un backstop di SOLA
  liveness di linea (force-unmask perche' i vicini non muoiano di fame),
  e NON sgancia piu' nessuno. La morte di un driver e' dichiarata solo
  dal reaper (irq_cleanup, chiamato da process_destroy) — autoritativa,
  gia' esistente. Un driver vivo ma lento tiene la sua linea. Rimossi il
  campo missed_fires e il define IRQ_DEAF_STRIKES.
- Restructure a blocchi secondo il paradigma del kernel: orchestratori
  leggibili come prosa IN FONDO (irq_forward_handler dispatch,
  irq_line_watchdog, irq_line_ack), blocchi esecutivi standard sopra
  (line_open_fire, line_fire_bound, line_fire_probe, line_close_fire,
  pending_ports, notify_ports, sharer_of). I due comportamenti prima
  intrecciati — forwarding degli sharer e discovery INTx — sono separati
  in line_fire_bound / line_fire_probe. Contratto invariato: decisione
  sotto s_irqfwd_lock, wake FUORI dal lock.

PRESERVATO alla lettera (nessun cambiamento di comportamento oltre lo
sgancio): la discovery INTx (probe/pending/claim), il backoff, il
piggyback dei pending sulle linee legate (ICH PIRQ, GSI>=16), le
degradazioni con heap timer esausto (giro senza grazia / park), la
contabilita' per-sharer, l'ordine dei lock.

Doppia compatibilita' legacy/SMP invariata: mask/unmask/eoi restano
dietro intr_line_mask/unmask + lapic_eoi (PIC su Armada, IOAPIC+LAPIC su
SMP); MSI resta il percorso edge separato.

File: syscall/driver.c (solo il sottosistema di forwarding; syscall dei
driver, DMA, timer, MSI-setup, PCI-wire, video boomerang INTATTI).

NON COMPILATO in questo ambiente: verifica per te il boot su ferro,
l'INSERIMENTO (enumerazione: il piu' a rischio, tocca la discovery) e la
RIMOZIONE. Nel log: la resume del disconnect deve ora processarsi e non
deve piu' comparire "PID N sordo ... sganciato dalla linea".
# MainDOB — consegna dei risvegli event-driven: notify separato dalla decisione di prelazione; via la lettura cross-CPU in gara e il floor idle 250 ms (build 93)

Sintomo (1.1 tickless): lag diffuso, comandi eseguiti in ritardo o ignorati,
GUI a scatti, FSM dei driver che si incantano; peggiore a -smp 1 che a -smp 4.
Radice: non un problema di throughput ma di LATENZA di consegna dei risvegli.
Andando tickless, il tick periodico 1000 Hz — che in 1.0 sanava entro 1 ms ogni
wake perso — non c'e' piu'. Un wake non consegnato si manifesta ora come latenza
diretta; a cascata i receiver IPC dormono, le code porta si riempiono, il pool
IRQ (512) e PORT_QUEUE_MAX si esauriscono e i messaggi vengono DROPPATI.

CAUSA: enqueue_home_and_kick (blocco ESECUTIVO) decideva anche la POLICY di
prelazione leggendo g_cpus[home].current di un ALTRO core senza lock —
euristica 'outranks' in gara. Su una CPU idle il rilevamento (victim->is_idle,
lettura stantia) falliva e l'IPI di sveglia non partiva: la CPU restava in hlt
col lavoro accodato. Il floor idle di 250 ms in time_event_refresh mascherava
il difetto SOLO su SMP (era #ifdef MAINDOB_SMP); su UP non c'era rete e la
latenza era piena.

FIX — separazione notify / decisione, secondo il design a blocchi (logica sul
fondo, esecutivi come blocchi autonomi):
- enqueue_home_and_kick torna puro ESECUTIVO: accoda + alza need_resched
  ("rivaluta", non "prelaziona"). Nessuna lettura cross-CPU. Su SMP, verso un
  bersaglio REMOTO manda l'IPI solo se e' idle, letto dal flag AFFIDABILE
  cpu_idle sotto barriera store-load SIMMETRICA col blocco idle (lock addl,
  P6-safe: gira sull'Armada E500 senza SSE2). Un bersaglio ATTIVO non riceve
  IPI: si rivaluta da se' al proprio epilogo di uscita kernel o a fine quanto
  (latenza <= 1 quanto). L'IPI e' riservata a chi dorme.
- scheduler_preempt_if_needed diventa il GIUDICE unico: decide la prelazione
  con il PROPRIO current sotto il PROPRIO lock (mai in gara), IF spento dalla
  decisione allo switch. Gate unificato: wake -> batte STRETTAMENTE; fine quanto
  (scadenza passata) -> pari-grado fa round-robin (preserva il time-slicing);
  idle -> cede a qualunque lavoro. Questo rende sicuro il need_resched
  incondizionato: senza, reschedule(false) degraderebbe un thread alto-grado.
- nuovo blocco idle autonomo scheduler_idle_block(): pubblica cpu_idle sotto
  barriera, rilegge il lavoro, dorme con sti;hlt fusi. Unico punto in cui una
  CPU si addormenta; usato da BSP e AP. switch_to pubblica cpu_idle = chi gira
  (chiude la finestra hlt->switch dove il flag sarebbe restato stantio true).
  L'idle degli AP prima era un hlt nudo che non guardava nemmeno la runqueue:
  ora e' allineato al BSP.
- time_event_refresh torna OROLOGIO puro: min(agenda timer, fine quanto). Via
  il floor 250 ms e TIME_EVENT_IDLE_SAFETY_NS: nessun risveglio dipende piu' dal
  polling, quindi non c'e' nulla da rattoppare.

CONTESTO verificato per tutte le variabili — UP vs SMP, PIT vs LAPIC one-shot,
TSC calibrato o no. UP: nessuna IPI (blocco SMP #ifdef via), i wake arrivano
dall'epilogo di uscita kernel; una CPU UP idle e' svegliata solo da un IRQ ->
epilogo. SMP: richiede LAPIC per le IPI; flag cpu_idle + barriere simmetriche
rendono la sveglia dell'idle race-free. slice_deadline resta locale (niente
letture 64-bit strappate cross-core su i686). Fallback PIT periodico (TSC non
calibrato) invariato e sicuro. Il ritorno da syscall e' gia' incanalato
nell'epilogo condiviso (syscall_stub -> isr_common -> isr_dispatch ->
preempt_if_needed): nessun canale di consegna aggiunto.

NOTA onesta: tolta ogni rete periodica, un eventuale evento perso a livello
HARDWARE (IPI LAPIC persa su ferro) non si auto-sana e appende sodo — massima
debuggabilita', in linea con la priorita' dichiarata. Un backstop a periodo
lungo (secondi) contro la perdita IPI hardware e' una decisione SEPARATA, non
questa patch.

File: proc/scheduler.c, proc/scheduler.h, proc/thread.c, time/event.c.
Verifica a runtime: log "[TIME] Modo eventi attivo"; i contatori di drop IPC
([IPC] POOL IRQ ESAURITO, porta PIENA) devono andare a zero; -smp 1 ora
reattivo quanto -smp 4 (sparita l'asimmetria del floor).
# MainDOB — run-hotplug: backend USB via -blockdev, cicli insert/remove ripetibili (build 92)

Sintomo (dal monitor QEMU): dopo il PRIMO ciclo device_add/device_del, ogni
re-add fallisce con "Property 'usb-storage.drive' can't find value
'usbstick'" — il test hotplug era eseguibile UNA volta per sessione.

CAUSA (QEMU, non MainDOB): il backend era definito con -drive if=none. Con la
semantica legacy di -drive, quando il device che lo referenzia viene rimosso
con device_del QEMU DISTRUGGE ANCHE IL BACKEND (il blocco "appartiene" al
device dal momento dell'attach). Il re-add non trova piu' 'usbstick'.

FIX — Makefile, target run-hotplug: backend definito con DUE -blockdev
(driver=file + driver=raw impilati, node-name=usbstick). I nodi -blockdev
sopravvivono all'unplug per contratto QEMU: device_add e device_del sono ora
ripetibili senza limiti nella stessa sessione. Comandi del monitor invariati;
razionale annotato nel Makefile accanto al target.

Workaround in-sessione per QEMU gia' avviati col vecchio target (senza
riavviare): ricreare il backend dal monitor con
  drive_add 0 if=none,id=usbstick,format=raw,file=usbstick.img
e poi il device_add consueto.

Nessuna modifica a kernel o driver in questa build.
# MainDOB — doppio frame dei driver video come OPT-IN via DobSettings (mach64 + x3100); bonifica dei tag clonati in x3100 (build 91)

Contesto: la ristrutturazione di dobinterface ha eliminato i flash allo
spostamento delle finestre che il page-flip (doppio frame, l'"antialiasing
2x" storico) era nato a coprire. Sul ferro con VRAM contata — Armada E500
(mach64, 8 MB) ed Extensa 5220 (x3100) — quella copertura era obtorto collo:
un intero frame di VRAM sacrificato al back buffer. Ora che la causa e'
rimossa, il doppio frame diventa una SCELTA.

IMPLEMENTAZIONE (identica nei due driver, secondo il design a blocchi):
- nuovo verbo <drv>_double_buffer_opted_in(): declareSetting idempotente di
  "video.double_buffer" (SETTING_BOOL, etichetta "Doppio frame (page-flip
  anti-flicker)", default "false") + getSetting. Ritorna true SOLO su valore
  esattamente "true": assente/errore/daemon non raggiungibile = frame
  singolo. Lo stub DobSettings ha riconnessione bounded (2 s max se
  settingsd non risponde): il driver video non resta mai in ostaggio del
  daemon, e nessun ciclo di dipendenze e' possibile (settingsd dipende da
  DobFS/disco, mai dal video).
- l'allocazione della back page nell'init e' ora dietro l'opt-in: a "false"
  (default) NON viene nemmeno tentata — sull'Armada restituisce ~3 MB su 8
  al pool superfici; a "true" con allocazione fallita resta il ripiego
  storico a frame singolo. Il resto del driver si adatta da solo
  (double_buffered gia' governava clear della back page, page-flip,
  compose e diagnostica).
- build: mach64.mdl e x3100.mdl linkano DobSettings_stub.o (pattern gia' in
  uso: ac97 con DobInterface_stub) con -I$(BUILD_DIR)/eps.

BONIFICA COLLATERALE trovata durante il lavoro: x3100/main.c aveva NOVE
debug_print col tag "[mach64]" — fossili del clone da cui il driver e'
nato. Su un log seriale del ferro avrebbero attribuito i messaggi al driver
SBAGLIATO nel momento peggiore (debug su macchina fisica). Tutti rinominati
"[x3100]"; nota storica lasciata nel punto della scoperta.

Per il test sul ferro: il default e' il frame singolo su entrambe le
macchine senza toccare nulla; per riattivare il comportamento storico,
DobSettings -> "Doppio frame (page-flip anti-flicker)" -> true, riavvio del
driver (o della macchina). La declare e' idempotente: la voce compare
nell'editor dal primo boot.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su entrambi i main;
regole di link riviste a mano (prerequisiti + linea LD + include EPS).
# MainDOB — RADICE VERA del fault [VID]: CR3 perso alla preemption della fase in-driver del boomerang (build 90)

La riga "in volo" di b84 ha consegnato il caso completo al primo log utile:
opcode 0x400 = DV_SURFACE_CREATE (vproc 1, payload 20 byte dallo stack del
chiamante) — la PRIMA superficie di dobinterface — con il solito
vec 14 err 0x4 eip 0x00408bf8 cr2 0x01345000, e i retry successivi a cr2 0.

ISTRUTTORIA (in ordine, con le prove):
  1. L'allocatore libc e' stato SCAGIONATO sperimentalmente: harness offline
     con sbrk finto su arena + fuzz di 200.000 operazioni casuali (malloc/
     free/realloc, taglie 1B..3MB, verifica di integrita' dei contenuti e
     bounds a ogni passo): ZERO violazioni. La logica sequenziale e' sana; il
     lock b82 e' nell'immagine (bga.mdl dipende da LIBC_OBJS, rilinka).
  2. Restava una sola classe compatibile con: fault deterministico identico
     da b73 a b89, indipendente dal lock heap, EIP nel testo libc, cr2 a
     heap_bga + 2 superfici, e la faccia gemella "niente fault ma GUI
     corrotta". Ed era nello SCHEDULER, dove l'utente puntava da tre turni.

RADICE: context.cr3 viene impostato ALLA CREAZIONE del thread
(t->context.cr3 = t->owner->page_directory) e MAI aggiornato allo switch-out;
switch_to ricarica sempre quel valore. La fase in-driver del boomerang presta
al thread del CHIAMANTE il CR3 del DRIVER — e quel thread puo' essere
prelazionato li' dentro (quanto scaduto: da b85 la scadenza arriva sempre) o
bloccarsi legittimamente (sbrk del driver, contesa heap con yield). Al
resume: CR3 = directory del PROPRIETARIO (dobinterface), EIP = dentro il
codice di bga. Le immagini condividono la base 0x400000: il fetch NON fault-a
e l'esecuzione prosegue nel testo del processo SBAGLIATO, fino alla prima
lettura di un indirizzo che esiste solo nell'AS di bga — l'heap delle
superfici: cr2 0x01345000, err lettura/non-presente, deterministico perche'
la sequenza di allocazioni al boot e' deterministica. Se invece il codice
interleaved SCRIVE prima di leggere: pagine di dobinterface corrotte in
silenzio — i boot "senza fault" con finestre a caso. Il bug e' nato col
boomerang (il log b73 mostrava gia' questo fault) e non dipende da malloc,
BUSY o registrazioni: dipende SOLO dal timing della preemption dentro la
finestra di dispatch — bimodale per costruzione.

FIX — proc/scheduler.c, dentro switch_to (una riga di semantica, non una
pezza): IL CR3 E' STATO DEL THREAD, NON IDENTITA' DEL SUO OWNER —
prev->context.cr3 = cpu_read_cr3() allo switch-out. Un thread riprende
nell'address space in cui stava girando, qualunque esso sia: la fase
in-driver sopravvive a preemption e blocchi; per i thread ordinari il CR3
vivo coincide con quello dell'owner (invariante, zero cambi di
comportamento). I percorsi di ritorno e di recovery del boomerang gia'
ripristinano il CR3 del chiamante in-band: coerenti con il salvataggio.

Con questa radice si spiegano e si chiudono in un colpo: il fault [VID]
deterministico e i suoi retry a cr2 0 (stato del driver abortito a meta'),
le finestre disposte a caso/morte nei boot senza fault, e la coppia
bimodale che ha resistito a b82/b83. I fix b82 (lock heap: bug reale
latente), b83 (retry BUSY), b85 (tempesta quanto) restano tutti validi e
necessari — erano strati sopra questa radice.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su scheduler.c; harness
e fuzz dell'allocatore agli atti; revisione manuale dei percorsi di ritorno/
recovery del boomerang rispetto al nuovo salvataggio.
# MainDOB — audit kernel integrale: casi non previsti chiusi (cleanup_owner a drain completo, heap timer esausto sui percorsi IRQ, guardia CMOS) (build 89)

Richiesta: passare TUTTO il kernel a caccia di bug e casi non gestiti.
Metodo: sweep -Wall -Wextra su ogni sorgente (esito: ZERO warning, per la
prima volta nella storia del ramo) + revisione logica mirata dei percorsi
caldi (IRQ forwarding, registry, tempi, teardown) e dei fix recenti stessi.

VERIFICATI SANI (nessun intervento): PIT_HZ=1000 (tutte le divisioni dei
tempi sono esatte); sleep degradata a yield quando l'heap timer e' pieno
(gia' gestita con log); contratto wait_prepare/wait_cancel (il waker che
batte il cancel e' documentato e gestito); drain_and_wake di b86 (il lotto
esattamente pieno con lista vuota costa solo un giro a vuoto); mmap_phys UC;
buffer dei log bounded.

TROVATI E CORRETTI:

1. registry_cleanup_owner — DOPPIO bug della stessa famiglia sanata in b86:
   (a) i record di parcheggio del morto venivano rimossi a lotto SINGOLO di
   8: gli eccedenti sopravvivevano con un tid stantio — al riciclo del TID,
   una register futura avrebbe SBLOCCATO IL THREAD SBAGLIATO (parente
   stretto dell'ABA dei PID di b78, lato TID); (b) il loop delle voci
   SMETTEVA DI DISATTIVARLE quando il batch dei waiter (16) si riempiva:
   voci attive di un morto = registry_wait futuri serviti con una porta
   morta. Riscritto a lotti ripetuti su entrambi i fronti; il nome della
   voce viene copiato FUORI dal lock prima del collect (lo slot disattivato
   e' riutilizzabile da una register concorrente che lo riscrive).

2. syscall/driver.c — timer_arm_in IGNORATO in tre punti del percorso IRQ:
   a heap timer esausto (cap 1024) la linea restava MASCHERATA SENZA RETE.
   Ogni sito ora degrada in modo sicuro, mai in wedge:
   - fire su linea legata: timeout degenerato immediato (contabilita'
     chiusa, unmask subito, notifiche comunque; gli irq_done a linea non
     mascherata sono no-op per contratto — peggior esito una notifica
     ripetuta);
   - finestra di claim del probe: park immediato (nessuna finestra senza
     scadenza; la discovery si riarma alla prossima registrazione);
   - backoff: degrada al park del caso 2b (un backoff senza sveglia non
     finirebbe mai).
   Ognuno con la propria riga di log ("heap timer esausto, ...").

3. time/rtc.c — guardia sull'assurdo: CMOS con istante minore dell'uptime
   (batteria morta) mandava la compensazione di b86 in underflow (~2106).
   Meglio un orologio sballato di 3 s che di 80 anni.

Verificato: sweep integrale -fsyntax-only -Wall -Wextra su TUTTI i .c del
kernel = zero warning; revisione manuale dei tre percorsi degradati.
# MainDOB — PARITA' DRIVER-FACING con l'1.0: buffer DMA di nuovo UNCACHED (regressione da ferro vero) (build 88)

Suggerimento dell'utente: il kernel 1.0 e' "preparato" per l'USB sul ferro
vero (Armada E500 = UHCI/PIIX4; Extensa 5220 = ICH8, che richiese "profonde
revisioni su pcie e dma") — confrontare cosa mette a disposizione dei driver
che l'1.1 non ha. Audit di parita' condotto voce per voce:

PARITA' CONFERMATA (nessun intervento):
- superficie syscall driver identica (SYS_GET_DRIVERS ritirato DI PROPOSITO
  in entrambi, slot 93 "retired");
- WAIT_READY (op 67, rendezvous deterministico) e' protocollo TRA processi
  userland (servito da usbms, consumato da DobDisk/hotplug): presente;
- gatekeeper EHCI (handoff USBLEGSUP + CONFIGFLAG=0, vitale sull'ICH8 che ne
  ha DUE): e' un driver, presente in drivers/usb_ehci;
- sys_mmap_phys: BAR MMIO mappati PTE_CACHE_DISABLE in entrambi;
- dma alloc: contiguita' fisica garantita in entrambi (pmm_alloc_contiguous;
  vitale per i pool TD/QH che calcolano i fisici come base + i*sizeof);
  policy di zona equivalente (DMA->ANY vs PREFER_LOW).

REGRESSIONE TROVATA E CORRETTA — sys_dma_alloc: l'1.0 mappava i buffer DMA
con PTE_CACHE_DISABLE; l'1.1 li mappava WRITE-BACK CACHEABLE. E' memoria
letta/scritta da un BUS MASTER: i driver scrivono descrittori (TD/QH, PRD,
ring) con store volatile e ZERO barrier — contratto scolpito dall'1.0 dopo la
campagna sul ferro (descrittori in cache e device che legge la RAM =
corruzione DMA intermittente; cfr. 1.0 "overflow del pool TD... corruzione
DMA"). Su QEMU la regressione e' INVISIBILE (coerenza emulata perfetta):
sarebbe esplosa alla prima sessione seria su Armada/Extensa, intermittente e
senza firma. Ripristinato PTE_CACHE_DISABLE con il razionale nel punto giusto
(stessa classe di mmap_phys: cio' che si condivide con l'hardware non passa
dalla cache).

Nota di metodo: e' il terzo bug di questa campagna invisibile per costruzione
su QEMU (dopo la race 0xCF8 e il PIRQ) — il ferro resta l'arbitro; i test
Armada/Extensa delle prossime sessioni vanno fatti su questa base.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su driver.c.
# MainDOB — guardia anti-ISO-stantia su rerun; istruttoria chiusa sul fault [VID] (meccanismo confermato dai layout, cura gia' in b82); protocollo di retest post-tempesta (build 87)

ISTRUTTORIA [VID] CHIUSA SULLA CARTA, coi numeri finali del layout:
  - elf.c/process.c: il brk parte a FINE IMMAGINE (r.brk), non a 0x10000000
    (quello e' solo il default pigro di sys_brk per processi senza loader);
  - quindi l'heap di bga inizia a ~0xD00000 (immagine ~0x400000 + BSS con
    g_bga: 8 MiB di cmdlist_storage + tabelle) e cr2 0x01345000 cade ~7 MB
    DENTRO l'heap: l'interno del sys_pixels malloc'ato di una superficie
    SYSRAM (i back buffer di dobinterface, 0x300000 l'uno);
  - paging_map_page opera sul CR3 VIVO (mappatura ricorsiva) e sys_brk sulla
    contabilita' di video_boomerang_as_process: in dispatch ENTRAMBI sul
    driver — il percorso kernel e' coerente, scagionato;
  - resta un solo meccanismo compatibile con un buco deterministico a offset
    fisso dentro un buffer grande: il BRK DIVERGENTE dell'allocatore libc
    senza lock (free con trimming sbrk(-N) su un contesto, malloc in crescita
    sull'altro) — che e' ESATTAMENTE cio' che b82 cura. Deterministico perche'
    la sequenza di allocazioni al boot e' deterministica.
CONCLUSIONE: la cura del fault [VID] e' gia' nel repo da b82 — ma b82 e b83
sono USERLAND, e i test sono passati da `make rerun`, che dichiara "no
rebuild": con ogni probabilita' quelle build non sono MAI state eseguite (il
cr2 fotocopia b81->b83 e' la firma di una ISO stantia). Il secondo fault a
cr2 0 e' danno secondario del primo (reset a meta' operazione).

FIX DI QUESTA BUILD — il bug e' nel BUILD SYSTEM, ed e' li' che si cura:
`make rerun` ora RIFIUTA di partire se un sorgente qualunque (kernel, libc,
libdob, boot, drivers, programs, common_files) e' piu' recente della ISO
(find -newer): tre giri di caccia sono stati bruciati testando binari che non
contenevano i fix. Per i confronti deliberati con immagini vecchie c'e'
`make rerun-stale`, il cui nome dichiara cosa si sta facendo.

PROTOCOLLO DI RETEST (tutti i sintomi aperti sono stati osservati sotto la
tempesta di interrupt b85 e/o su ISO potenzialmente stantie — vanno rimisurati
su base pulita): make run (rebuild completo). Attesi in serie di riavvii:
zero [VID] fault (b82+b85), GUI deterministica (b83+b85), orologio esatto
(b86), sistema reattivo nel tempo (b85, contatori IPC b80 muti). Poi il
detach USB su run-hotplug con BUILD_MODE=debug: la FSM del driver copre gia'
unplug in idle (RD), in finestra awake (check alla demozione) e in attivita'
(errore di transfer) — se fallisce ancora su base pulita, il log [uhci] del
device_del nominera' il ramo, e SOLO allora si tocca il codice.

Verificato: dry-run del Makefile ok; nessuna modifica a codice kernel/driver
in questa build (per scelta: niente pezze su sintomi misurati male).
# MainDOB — bonifica dei bug annotati: drain completo del registry, collisione BOOT_MODULES_MAX, wall-clock ancorato al boot (build 86)

Chiusura dei tre item aperti nei changelog precedenti, ciascuno nella propria
funzione logica.

1. REGISTRY, drain a lotti COMPLETO (registry.c). collect_name_waiters e
   collect_parked staccano a lotti (16/8) per svegliare fuori da s_lock —
   giusto — ma registry_register si fermava al PRIMO lotto: il waiter oltre
   il sedicesimo e il parcheggiato oltre l'ottavo sullo stesso nome
   restavano in attesa PER SEMPRE (la register di un nome e' una-tantum,
   nessun altro evento li avrebbe toccati). Oggi non mordeva (max 4 su un
   nome) ma era una mina a orologeria sulla crescita di Startup_modules.
   Nuovo verbo drain_and_wake: colleziona sotto lock, sveglia fuori, ripete
   finche' i lotti non tornano NON pieni. La voce viene installata PRIMA del
   drain: chi arriva dopo la trova e non parcheggia — nessuna finestra.

2. STARTUP_MODULES_MAX (kernel.c). Il parser di Startup_modules ridefiniva
   BOOT_MODULES_MAX=32 sopra il 16u di boot_info.h: due CONCETTI diversi
   (voci del file di avvio vs moduli GRUB del multiboot) con lo stesso nome
   — l'unico warning residuo di ogni sweep dalla b74. Rinominato
   STARTUP_MODULES_MAX con commento sul perche': lo sweep del kernel e'
   finalmente SILENZIOSO al 100%.

3. WALL-CLOCK ANCORATO (time/rtc.c) — il primissimo bug diagnosticato della
   campagna (b74) e mai curato: clock_get_realtime = epoch + monotono, ma
   rtc_init impostava l'epoch col valore CMOS GREZZO letto a ~3 s di uptime
   (calibrazione TSC + splash) — il sistema viveva ~3 s nel futuro, per
   sempre. Latente nell'1.0 (uptime alla lettura ~ms), reso visibile dalla
   splash dell'1.1. Ora: epoch = CMOS - uptime alla lettura (udiv64_u32 di
   clock_now_ns), e la riga [RTC] dichiara la compensazione ("-Ns di boot").

Restano deliberatamente aperti, in attesa di dati post-b85: il fault [VID]
deterministico (da rivalutare senza la tempesta di interrupt; la riga "in
volo" di b84 lo nomina se ricompare) e il detach USB (serve il log debug del
device_del, changelog b79). La freelist LIFO di ipc/port.c resta per scelta
(guardia a generation gia' usata dai consumatori).

Verificato: sweep -fsyntax-only -Wall -Wextra COMPLETAMENTE pulito su
registry.c, kernel.c, rtc.c (zero warning, incluso lo storico).
# MainDOB — FIX RADICE del degrado sistemico: tempesta di auto-interrupt del quanto tickless su thread solitari (build 85)

L'indicazione dell'utente era giusta: scheduling, non video. La catena,
verificata anello per anello nel codice:

  1. switch_to imposta slice_deadline = now + quanto e time_event_refresh
     arma il LAPIC one-shot su quella scadenza (modo eventi: NESSUN tick
     periodico di fondo);
  2. il quanto scade con la runqueue VUOTA (thread CPU-bound solitario:
     bga che composita, un client che disegna, ahci in init): il vecchio
     scheduler_slice_check non faceva NULLA — e nessun altro punto del
     codice rinnovava slice_deadline, che restava NEL PASSATO per sempre;
  3. event_fire -> time_event_refresh: la scadenza minima in agenda e'
     quella passata -> lapic_arm_oneshot: delta_ns=0 -> init_count=1 ->
     il LAPIC RISPARA DOPO UN CICLO DI BUS;
  4. loop perpetuo fire->check(nulla)->refresh(passato)->fire: TEMPESTA di
     auto-interrupt alla massima frequenza su quella CPU, per tutta la
     vita del thread solitario.

Conseguenze, che sono ESATTAMENTE la fenomenologia riportata: CPU che vive
nell'ISR (sistema "progressivamente lento e laggoso"), IPC/input/GUI in coda
dietro la tempesta (click in ritardo o persi), thread remoti accodati a pari
grado senza IPI ne' need_resched (per design non prelazionano) che restano
affamati finche' il solitario non blocca (finestre che non compaiono MAI in
certi boot), bimodalita' pura da timing SMP (quale CPU cattura il CPU-bound).
Su TSC-deadline e sul fallback PIT one-shot la meccanica e' identica (scadenza
passata = fire immediato): il bug e' a monte, nel mancato rinnovo.

FIX — proc/scheduler.c, dentro la funzione logica del check (niente pezze):
scheduler_slice_check ora distingue i due esiti a quanto scaduto: c'e' un
pari/miglior grado pronto -> need_resched (round-robin, come prima); NESSUNO
da far girare -> il quanto si RINNOVA (slice_deadline = now + quanto del
corrente). Il rinnovo chiude la tempesta (refresh arma sempre una scadenza
futura) ED e' la rete di sicurezza per i wake remoti a pari grado: al
prossimo fine-quanto il check li vede — latenza massima UN quanto (2..20 ms
per grado), mai starvation.

Nota metodologica: i fix b82 (lock allocatore) e b83 (retry su BUSY) restano
— bug reali, ma secondari; il fault [VID] deterministico va rivalutato SOPRA
questa base (la tempesta stravolgeva i tempi di tutto): se ricompare, la riga
"in volo" di b84 lo nomina. Il landmine collect_parked cap=8 del registry
(moduli oltre l'ottavo parcheggiati per sempre su uno stesso nome) e'
annotato e oggi non morde (max 4 su un nome): candidato per la prossima
build.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su scheduler.c.
# MainDOB — reset di metodo sulla GUI bimodale: il fault ora dice QUALE chiamata era in volo; ipotesi heap/BUSY archiviate come non-radice (build 84)

I bootlog b83 falsificano le ultime due ipotesi: il fault e' TORNATO con lo
STESSO cr2 (0x01345000) nonostante il lock dell'allocatore (b82) e il retry su
BUSY (b83) — quindi nessuna delle due era la radice (restano fix corretti di
per se': bug reali, ma non QUESTO bug). In piu' compare un secondo fault a
cr2 0x00000000 subito dopo il primo: e' danno SECONDARIO — il DV_ERR_RESET
abortisce bga a meta' operazione e la chiamata successiva inciampa su uno
stato interno mai riempito. Un boot ha mostrato zero finestre SENZA alcun
fault: terza via silenziosa o (vedi nota ISO) userland stantio.

COSA E' STATO ESCLUSO A COLPO SICURO, dai numeri: cr2 0x01345000 NON e' heap
(brk parte a 0x10000000, verificato in sys_brk — che gia' usa correttamente
video_boomerang_as_process), NON e' SHM (0x60000000..0x70000000), NON e'
mmap_phys (0x80000000+), NON e' IPC overflow (0x60000000+). E' nel limbo
[fine ELF .. 0x10000000): BSS gigante oltre il mappato, oppure un puntatore
selvaggio ma deterministico — p.es. un fisico usato da virtuale (0x01345000
cade nel range fisico del blob live). Deterministico su TRE build diverse =
layout statico, non allocazione.

QUESTA BUILD — l'unico intervento e' diagnostico, DENTRO la funzione logica
esistente (video_boomerang_fault_recover), dal contratto dello slot che gia'
conteneva tutto senza che nessuno lo leggesse: nuova riga
"[VID ]   in volo: opcode N args(a0,a1,a2) payload VA 0x.. size .."
(BM_SCRATCH=opcode di ingresso, EBX/ECX/EDX=argomenti, ESI/EDI=payload del
chiamante). Col prossimo boot storto la mappa e' meccanica: opcode -> case del
dispatcher fast -> funzione dv_ -> struct di wire -> QUALE campo conteneva
cr2. Fine delle congetture.

NOTA ISO IMPORTANTE: b82 (libc) e b83 (header dv_call) vivono nell'USERLAND —
entrano in gioco solo ricostruendo la live ISO. `make rerun` dichiara
esplicitamente "no rebuild": se i test b82/b83 sono stati fatti in rerun su
una ISO precedente, quei fix non sono MAI stati eseguiti (coerente con il cr2
fotocopia). Prima del prossimo test: make pulito che rigenera la ISO, poi il
primo boot storto con la riga "in volo".

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su driver.c.
# MainDOB — SECONDA race del boot GUI: DV_ERR_BUSY silenzioso del boomerang senza retry nei chiamanti (build 83)

I due bootlog b82 dell'utente provano DUE cose insieme: (1) il fault [VID] e'
SPARITO da entrambi i boot — il fix b82 (lock dell'allocatore) ha eliminato
quel bug, che era reale; (2) la GUI zoppica ancora in modo bimodale, ora SENZA
ALCUNA firma kernel. Quindi c'era una seconda race, indipendente, interamente
nel protocollo.

MECCANICA: lo stub int 0x85 (video_boomerang.asm), a g_boomerang_lock conteso,
spinna con budget LIMITATO (gira a ring 0 con IF=0: non puo' attendere) e poi
molla con DV_ERR_BUSY — silenziosamente, per design (meglio un errore al
chiamante che una CPU murata). Ma le primitive dv_callN (sys/syscall.h) erano
int 0x85 nudo: NESSUN retry. Ogni contesa tra due dv_call su CPU diverse — o
una dv_call arrivata durante una fase in-driver lunga — diventava un errore
DURO e MUTO. Sulle chiamate fondative di dobinterface all'avvio: finestre
disposte a caso o morte, click/resize che non rispondono, bimodale col timing.
Nota bruciante: il fix b82 ha probabilmente ALLARGATO questa finestra — il
contesto dispatch che attende l'heap-lock del driver fa yield() TENENDO
g_boomerang_lock, e gli altri chiamanti bruciano il budget piu' spesso. Le due
race erano intrecciate: b82 ne ha chiusa una e ingrassata l'altra.

FIX — libc/include/sys/syscall.h: le sette primitive dv_call0..5 e dv_call_pl
diventano _raw; sopra, wrapper con retry su DV_ERR_BUSY (yield() tra i
tentativi, budget DV_CALL_BUSY_RETRY_MAX=4096 — chi tiene il lock gira su
un'altra runqueue e non attende mai chi ritenta: progresso garantito; BUSY
oltre il budget = wedge reale, torna al chiamante). NOTREADY non si ritenta
(driver assente = stato reale). Verificato repo-wide che NESSUN driver usa
DV_ERR_BUSY come risposta applicativa: il codice e' oggi esclusivamente il
bail del kernel; se un giorno servisse un busy applicativo, dovra' essere un
codice diverso (annotato nell'header). Il retry vive nella primitiva: ogni
chiamante presente e futuro lo eredita.

Test di accettazione: serie di riavvii live consecutivi — GUI deterministica,
nessun [VID] fault, finestre/click/resize vivi a ogni boot. Le due radici
(heap senza lock, BUSY senza retry) coprono insieme l'intera fenomenologia
bimodale osservata; se residuasse ancora qualcosa, il prossimo passo e' il
debug build di dobinterface (le [dob] ora sono l'unico punto cieco rimasto).

Verificato: header compile-test m32 con tutte le sette varianti; nessun uso
esterno delle _raw; sweep pulito.
# MainDOB — FIX RADICE del boot GUI bimodale: allocatore libc non thread-safe nei processi a doppio contesto (main + dispatch boomerang) (build 82)

La diagnostica b81 ha consegnato la prova al primo boot storto — due volte,
IDENTICA:

    [VID ]   dettaglio: vec 14 err 0x4 eip 0x00408bf8 cr2 0x01345000

Lettura (err bit W=0) da ring 3 (bit U=1) su pagina NON PRESENTE (bit P=0),
indirizzo page-aligned e DETERMINISTICO tra boot diversi. Questo scagiona la
corruzione "classica" di metadati (darebbe indirizzi ballerini) e smaschera
la vera meccanica: cr2 0x01345000 NON e' la finestra VRAM (mmap_phys alloca a
0x80000000+), e' il range del BRK di bga (immagine a 0x400000 + ~19 MB di
heap: i buffer grossi del compositor vivono li'). E' un BUCO NON MAPPATO in
mezzo al heap.

MECCANICA: la malloc della libc e' una free-list singola con trimming — free
puo' fare sbrk NEGATIVO per restituire pagine. In MainDOB uno stesso address
space ospita DUE contesti su CPU diverse (modello migrating-thread): il
main-thread del driver video e il dispatch boomerang (il thread del CHIAMANTE
IRETtato in bga). Nessun lock: una free con trimming sul main mentre una
malloc cresce nel dispatch => contabilita' del break divergente => malloc
ritorna un puntatore dentro pagine che il kernel ha appena smappato => prima
lettura = #PF deterministico a meta' buffer. Il fault_handler, col boomerang
in volo, instrada correttamente al recovery (DV_ERR_RESET) — ma la chiamata
persa e' quella FONDATIVA di dobinterface: da li' finestre disposte a caso,
click/resize morti. Bimodale perche' dipende dal sovrapporsi delle malloc dei
due contesti nella finestra di avvio; a regime, ogni coincidenza = crash
"casuale".

FIX — libc/src/stdlib.c, lock dell'allocatore:
  - parola di lock in AS condiviso con __sync_lock_test_and_set + yield() nel
    giro di attesa. NON un futex, DELIBERATAMENTE: nel contesto boomerang
    l'identita' kernel del thread e' quella del chiamante e il futex verrebbe
    chiavato sul processo SBAGLIATO; l'xchg userspace e' corretto per
    costruzione (stessa parola, stesso AS). Progresso garantito: chi tiene il
    lock gira su un'altra runqueue e non attende mai chi spinna.
  - malloc/free -> wrapper pubblici lockati su vie interne _unlocked (la
    ricorsione di malloc dopo heap_grow usa la via interna: nessun
    annidamento); calloc invariato (compone i pubblici); realloc riscritto
    ATOMICO sotto il lock (header + copia + free del vecchio con le vie
    interne: una free concorrente poteva fondere/spostare il blocco tra la
    lettura dell'header e la memcpy).
Vale per TUTTO l'userland, non solo bga: qualunque processo futuro con
dispatch in-AS eredita l'allocatore corretto.

Atteso: il fault [VID] sparisce dai boot (N riavvii consecutivi puliti), GUI
deterministica all'avvio; in piu' una fetta dei crash "casuali" a regime
dovrebbe rientrare. Se il degrado progressivo persiste ANCHE senza il fault,
restano aperti i binari b79/b80 (uhci runtime; contatori IPC ora parlanti).

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su stdlib.c; revisione
manuale dell'assenza di annidamento del lock su tutte le vie.
# MainDOB — race bimodale del boot GUI inchiodata al boomerang video: diagnostica del fault + indiziato primario (heap del driver senza lock tra main-thread e dispatch) (build 81)

Due bootlog della STESSA build live, identici riga per riga tranne una: nel
boot "storto" compare

    [VID ] fault nel dispatcher boomerang (driver PID 14): chiamante
           'dobinterface.mdl' (PID 8) ripristinato con DV_ERR_RESET

e da li' finestre disposte a caso, click/resize che non rispondono. E' lo
STESSO fault gia' visto nel primissimo log b73: race latente che precede
tutta la campagna b74..b80. La bimodalita' del boot GUI e' TUTTA qui: quando
il fault non scatta, il boot e' sano.

ESCLUSO: l'ordine di registrazione del bga e' gia' corretto (probe -> modeset
-> boomerang int 0x85 -> registry "video" per ULTIMO, con tanto di commento
che spiega il perche'): dobinterface viene sguinzagliato solo a driver
dichiaratamente pronto. Il fault avviene DENTRO il dispatcher su una chiamata
legittima, in modo timing-dependent.

INDIZIATO PRIMARIO (dal design stesso, commento su sbrk in driver.c): "la
malloc del driver gira dentro il dispatch boomerang". Quindi due contesti
girano nell'AS di bga IN PARALLELO su CPU diverse: il main-thread del driver
(transport IPC) e il dispatch boomerang (il thread del CHIAMANTE, IRETtato in
bga). Il lock kernel (g_boomerang_lock) serializza dispatch-vs-dispatch, ma
NIENTE serializza dispatch-vs-main — e la malloc/free della libc condivisa dai
due contesti non e' thread-safe: corruzione di heap del driver dipendente dal
timing. Al boot la finestra e' massima (dobinterface bombarda di dv_call
mentre il main di bga sta ancora rientrando dall'init): bimodale per
costruzione; a regime, ogni coincidenza = i "crash casuali".

QUESTA BUILD (prova prima della cura): il recovery del boomerang ora fotografa
il fault PRIMA di ricostruire il frame — nuova riga
"[VID ]   dettaglio: vec V err E eip 0xADDR cr2 0xADDR". L'indirizzo decide in
un colpo tra le ipotesi: CR2 nel range heap del driver (e/o EIP dentro
malloc/free) = corruzione heap (indiziato primario confermato); CR2 nella
finestra VRAM = mappatura pigra; CR2 vicino allo stack di dispatch = stack;
CR2 < 0x1000 = deref NULL su stato del driver. Un solo boot storto con questa
riga vale piu' di ogni congettura.

Cura attesa (dopo conferma): lock utente attorno all'allocatore condiviso nei
due contesti del driver video (o arena separata per il percorso dispatch) —
intervento in libc/bga, NON nel kernel; il boomerang di suo sta gia' facendo
il suo lavoro (recovery pulito, lock mai incastrato).

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su driver.c.
# MainDOB — AUDIT IPC: core sano, ma cieco dove perde eventi — ogni perdita ora ha una voce (build 80)

Audit integrale del sottosistema IPC (channel.c 944 righe, port.c 412) alla
ricerca della causa del degrado sistemico (click persi/in ritardo, crash
casuali, rallentamento progressivo).

COSA E' RISULTATO SANO (verificato riga per riga):
- ciclo di vita delle entry messaggio: magazine per-CPU + depot + pool IRQ,
  free corretta su OGNI percorso incluso enqueue fallito e teardown porta
  (che drena la coda e libera i payload posseduti);
- ownership dei payload: chiusa anche su copyout fallito
  (deliver_payload_to_user libera prima di valutare l'esito) e su reply
  staged (snapshot sempre posseduto);
- protocollo di sonno del receiver: prepare-first + re-check sotto IF=0 —
  il wake perso "classico" (send tra check e sleep) e' strutturalmente
  impossibile; wait_cancel gestisce il waker che batte il cancel;
- teardown thread/reply pendenti: lock hashato per-TID attraversato dal
  cleanup PRIMA che il reaper liberi lo stack del sender.

COSA ERA CIECO (e ora parla):
1. POOL IRQ (512 entry, sorgente dei post kernel->user: SCADENZE TIMER
   userspace in testa): a esaurimento droppava CONTANDO IN SILENZIO —
   s_irq_drops esisteva e non veniva stampato MAI. Un drop li' e' un timer
   perso: GUI che salta un frame o un timeout, FSM driver che si incantano,
   eventi in ritardo di un giro — esattamente la famiglia dei sintomi
   riportati, e per costruzione invisibile. Ora: "[IPC ] POOL IRQ ESAURITO:
   post kernel perso (drop #N)", rate-limited, fuori lock.
2. PORTA PIENA: la riga c'era ma anonima; ora attribuisce — "porta %u (owner
   PID %d) PIENA" — cosi' si vede SUBITO quale consumer e' addormentato o in
   stallo.
3. TABELLA PORTE ESAUSTA (4096): ipc_port_create falliva in silenzio — le
   app "impazzivano" senza spiegazione. Ora logga con PID richiedente e
   conteggio occorrenze: se il numero cresce nel tempo, e' la firma di un
   LEAK di porte (create senza destroy) — candidato primario per un degrado
   progressivo.

NOTA DI METODO per la bisezione del degrado: la reinstallazione per il test
b78 ha cambiato ANCHE l'userland del disco (prima girava quello vecchio
dell'install originale). La matrice quindi ha due assi: kernel (b73..b80,
archivi per-build disponibili) e userland (vecchio/corrente). Una sessione
degradata su b80 con il log seriale sotto mano ora RACCONTA se la perdita e'
IPC (una delle tre voci sopra compare) o va cercata altrove (uhci runtime:
DMA/tassa GSI condiviso — protocollo in changelog b79).

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su channel.c e port.c.
# MainDOB — politica di riuso id PER-TABELLA (FIFO ai processi, LIFO alla SHM) + istruttoria sul degrado sistemico e sul detach USB (build 79)

Due segnalazioni dal campo su b78: (A) hotplug USB: l'inserimento funziona, la
RIMOZIONE no — "funziona una volta sola"; (B) degrado sistemico: click/comandi
in ritardo o persi, crash casuali, finestre corrotte gia' all'avvio, sistema
progressivamente piu' lento.

ISTRUTTORIA (B). Audit completo delle modifiche b74..b78: handle_table sana
(guardia sul doppio-remove, niente fallback nascosti, ring verificato anche a
stress senza duplicati); riarmo del safety timer dalla callback LEGALE per
contratto del sottosistema (heap_idx, lock mollato nelle callback); ordinamento
lock pulito; buffer dei log bounded. Restano DUE sospettati:
  1. la dinamica degli id SHM cambiata dal FIFO di b78: i buffer finestra
     nascono e muoiono di continuo, col LIFO gli id restavano densi e bassi,
     col FIFO ruotano su tutto lo spazio 1..127 — e' l'unica delta di b78 che
     tocca il percorso finestre;
  2. l'uhci che da b78 pilota PER LA PRIMA VOLTA il vero silicio: bus master
     DMA vivo (frame list/TD/QH — via dma_alloc, ma mai esercitato prima) e
     condivisione LEGITTIMA del GSI col disco di boot (ogni interrupt AHCI ora
     attende anche il done dell'uhci: tassa di latenza su tutto l'I/O disco).

FIX/LEVA (B): la politica di riuso diventa un CONTRATTO ESPLICITO di
handle_table_init (ht_reuse_t): HT_REUSE_FIFO per la tabella processi (il fix
ABA di b78 resta — per i PID il riuso tardo e' obbligatorio), HT_REUSE_LIFO per
la SHM (dinamica id identica a pre-b78: densi, bassi, riuso immediato; la
generation resta la guardia forte). Il popolamento della freelist e' funzione
della politica cosi' che entrambe assegnino prima gli id bassi (l'harness ha
beccato l'inversione). E' insieme la scelta giusta per contratto E la leva di
bisezione: se il degrado persiste su b79, la causa NON e' la dinamica SHM.

PROTOCOLLO DI BISEZIONE (B), in ordine:
  1. b79, sessione normale SENZA USB (make run): se il degrado persiste ->
     kernel b74..b79 (bisezione con gli archivi per-build); se sparisce ->
  2. b79 con USB (make run-hotplug, anche senza mai inserire la pendrive): se
     il degrado compare -> e' l'uhci runtime (DMA/schedule/tassa del GSI
     condiviso) e si scava li' con BUILD_MODE=debug.

ISTRUTTORIA (A), detach non rilevato. La FSM del driver e' progettata bene:
dopo l'enumerazione parcheggia in ST_DEVICE_IDLE = Global Suspend con
Resume-Detect armato, e in EGSM il DISCONNECT genera resume (QEMU lo fa su
attach E detach). Quindi il buco e' a valle: o l'RD del detach viene
classificato male (ramo "kept" di ST_DISCONNECT_CHECK / deferred RD), o
l'evento arriva mentre il controller e' in RUN (rimozione subito dopo l'uso,
prima della demozione a idle) e va perso per sempre — in RUN l'UHCI non ha
interrupt di port-change. Serve il log debug del momento del device_del:
make BUILD_MODE=debug run-hotplug, inserire, attendere l'icona e "[uhci]
device ready (idle-suspended, RD armed)", poi device_del pen. Tre esiti
possibili e ognuno punta a un colpevole diverso: (i) nessuna riga [uhci] ->
l'IRQ non arriva (consegna kernel/QEMU); (ii) righe [uhci] ma niente GONE ->
classificazione RD nel driver; (iii) GONE annunciato ma icona viva -> pipeline
a valle (hotplug/usbms/DobFS/GUI). Nessun fix alla cieca: prima il log.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su handle_table.c,
process.c, shm.c, kernel.c, driver.c; harness di ENTRAMBE le politiche + stress
FIFO (5000 op, zero duplicati) passati.
# MainDOB — FIX RADICE USB: ABA sui PID in hotplug — l'uhci pilotava il CONTROLLER SBAGLIATO (build 78)

La pistola fumante, consegnata dalle firme b77 al primo colpo:

    [IRQ ] linea 20: +sharer 'usb_uhci' (PID 16, 2 totali)
    [IRQ ] 0:4.0: GSI 20 dalla cache binding (PID 16)

usb_uhci si e' registrato con il BDF 0:4.0 — IL DEVICE DELL'AHCI DI BOOT. Da
li' tutto il resto: LEGSUP scritta nel config dell'AHCI, "USBSTS" letto dal
BAR dell'AHCI (spazzatura), zero resume-IRQ propri, sharer muto della linea 20
(50 ms di tassa a ogni interrupt del disco), pendrive invisibile. Il VERO UHCI
al suo slot restava senza driver, con hotplug convinto di averlo coperto. Ogni
sintomo dei bootlog b74..b77 discende da qui.

MECCANICA (ABA sui PID): handle_driver_ready risolve l'attach con
bubble_find_by_pid(sender_pid).
  1. hotplug spawna 'ahci' per 0:4.0 -> bolla A, driver_pid=16;
  2. quell'istanza esce (dedup contro ahci.mdl) e il kernel ricicla SUBITO il
     PID (freelist LIFO): lo spawn successivo, usb_uhci, e' di nuovo PID 16
     -> bolla B, driver_pid=16;
  3. l'attach dell'uhci (READY, sender 16) scandisce le bolle in ordine
     d'array e trova PRIMA la bolla A: risposta = device 0:4.0. L'uhci pilota
     l'AHCI;
  4. doppio avvelenamento: proc_status(16) ora vede VIVO (e' l'uhci!), quindi
     il reaper non ritira MAI la bolla stantia A.

FIX SU DUE LIVELLI:
  1. boot/hotplug/main.c — l'invariante al punto che la crea: il kernel
     garantisce PID unici FRA I VIVI, quindi appena spawn_file_sync consegna
     il PID del nuovo figlio, ogni ALTRA bolla che dichiara quel PID parla di
     un morto: ritirata subito (log "bolla stantia ritirata"), non al reap che
     non arriverebbe mai. bubble_find_by_pid torna cosi' non ambigua per
     costruzione.
  2. kernel, krt/handle_table.{h,c} — freelist da stack LIFO a RING FIFO: uno
     slot liberato torna in CODA e viene riassegnato solo dopo che tutti gli
     altri liberi hanno girato (con 256 processi, il riuso passa da "lo spawn
     dopo" a "centinaia di spawn dopo"). Per ogni bookkeeping chiavato su id
     il LIFO era una pistola carica; la generation resta la guardia forte per
     chi usa handle_ref_t. Semantica del ring verificata a tavolino (l'id
     liberato rientra per ULTIMO). Nota: la freelist privata di ipc/port.c
     resta LIFO ed e' fuori scope — le porte hanno gia' la guardia a
     generation usata dai consumatori (hotplug driver_gen, dobinterface
     owner_gen).

Contesto che si chiude: il claim "sbagliato" della linea 20 (b74/b75) non era
il claim a sbagliare — era l'uhci a credersi 0:4.0, e 0:4.0 STA davvero sulla
20. Il resolver del chipset, la serializzazione config e le firme di
membership (b75..b77) restano acquisiti e sono cio' che ha reso questa radice
visibile in UNA iterazione di log.

Atteso al prossimo run-usb: "[hotplug] bolla stantia ritirata" quando il
gemello ahci muore, uhci con il SUO BDF ("0:S.F: INTx risolto dal chipset ->
GSI g"), zero muti sulla linea 20, pendrive enumerata (icona) e hotplug
in/out funzionante. Vale anche per il ferro: sull'Extensa i 5 UHCI companion
moltiplicano gli spawn ravvicinati — la stessa finestra ABA, ora chiusa due
volte.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su handle_table.c;
revisione manuale hotplug/main.c (pattern memset+log identico al reaper);
harness Python della semantica ring FIFO passato.
# MainDOB — FIX boot bimodale del disco AHCI (gemello hotplug vs istanza di boot) + firma su OGNI variazione di membership IRQ (build 77)

Due fatti dal bootlog b76: (1) "a volte il disco ahci c'e' e a volte no" —
questo giro mancavano proprio "0:31.2 -> GSI 16" e "linea 16 -> ahci"; (2) il
muto PID 16 e' di nuovo sharer della linea 20 senza NESSUNA delle righe di
risoluzione b76 (ne' chipset, ne' fallthrough, ne' claim): e' entrato da un
percorso che non lasciava firma.

1. DISCO BIMODALE — drivers/ahci/main.c. Il claim della chiave per-controller
   (ahci@B:S.F) era immediato per ENTRAMBE le istanze: se il gemello spawnat
   da hotplug arriva mentre l'istanza di boot (Startup_modules) sta ancora
   scandendo/inizializzando, il gemello vince la chiave e l'istanza di boot —
   che esiste APPOSTA per avere il disco presto — muore. Il root mount resta
   appeso al gemello, in ritardo di secondi: DobFileSystem puo' scadere il
   timeout d'attesa e il disco "non c'e'". Esito = funzione del timing SMP
   (bimodale); il lock config di b76 ha rimescolato i tempi e l'ha reso
   visibile, ma la finestra era li' da prima. Fix: l'istanza via hotplug
   DIFFERISCE il claim di 500 ms — lo standalone di boot vince il proprio
   controller in modo deterministico, il gemello perde con grazia; sui
   controller senza istanza di boot (il secondo HBA) il differimento costa
   solo il ritardo. L'arbitro resta il registry atomico: il defer decide solo
   l'ordine di arrivo.

2. OSSERVABILITA' TOTALE DELLA MEMBERSHIP — syscall/driver.c. Chiusi gli
   ultimi percorsi silenziosi:
   - irq_attach_sharer: ogni sharer OLTRE il primo logga
     "linea N: +sharer 'nome' (PID n, k totali)" — prima entrava muto, ed e'
     esattamente il buco da cui e' comparso il PID 16 senza storia;
   - sys_irq_register_pci, hit della cache binding: "0:S.F: GSI g dalla cache
     binding (PID n)" — era l'unica via di risoluzione senza riga;
   - irq_cleanup: "linea N: -sharer PID n (teardown)".
   Con b76+b77 OGNI ingresso/uscita da una linea e OGNI via di risoluzione
   (chipset / cache / empirica / fallthrough) ha una firma: il prossimo log
   racconta la storia completa di chi si e' legato dove, come e quando.

Lettura attesa del prossimo run-usb: per l'uhci DEVE comparire una (una sola)
di queste storie — "risolto dal chipset -> GSI 20" + "+sharer" (tutto sano:
condivisione legittima con l'AHCI di boot), oppure "dalla cache binding",
oppure "non risolto dal chipset -> empirica" + "rivendicato empiricamente".
Se il PID dell'uhci compare come "+sharer" SENZA alcuna riga di risoluzione
precedente, il binario usb_uhci sul disco installato NON e' il sorgente
corrente (registrazione legacy diretta) — e la nota operativa di b76 diventa
diagnosi: test_disk.img viene riusato ("keeping data"), l'userland risale
all'installazione. Rifare l'install dal live corrente (rm test_disk.img)
prima del prossimo test.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su driver.c e (lato
driver, con gli header di piattaforma) revisione manuale di ahci/main.c —
sleep_ms e' il verbo standard di unistd.h gia' usato nel file.
# MainDOB — FIX RADICE SMP: cicli PCI config 0xCF8/0xCFC non serializzati; osservabilita' della risoluzione INTx (build 76)

Sintomo (run-usb su b75): il resolver del chipset funziona per gli AHCI
("0:4.0 -> GSI 20", "0:31.2 -> GSI 16") ma per l'uhci NESSUNA riga di
risoluzione — e' ricascato in silenzio nella discovery empirica e ha ri-claimato
la linea 20 (muto: PID 16, come su b74). Nota a margine importante: per la
matematica del chipset stesso la condivisione e' VERA — uhci in slot 5 con INTD
e ahci in slot 4 con INTA atterrano ENTRAMBI su PIRQE -> GSI 20 (swizzle
(slot+pin-1)&3): il claim empirico stavolta indovinava giusto; e' la VIA che
era fragile.

RADICE: il meccanismo config legacy e' UNA coppia di porte globali (indirizzo
su 0xCF8, dato su 0xCFC) e nel kernel viveva in TRE copie private, nessuna
serializzata: kpci_* in syscall/driver.c (dietro SYS_PCI_READ/WRITE, cioe' il
traffico config di TUTTI i driver userspace), pci_cfg_* in irq/pirq.c (resolver
INTx, detection bridge) e pci_cfg_r32 in boot/disk.c. Su SMP due cicli
concorrenti si interfogliano ADDRESS/DATA e leggono il registro del device
SBAGLIATO. E' esattamente la finestra del sintomo: mentre il kernel leggeva il
pin INT# dell'uhci (0x3D), l'ahci PID 17 stava inizializzando su CPU1 a colpi
di SYS_PCI_READ — pin letto spazzatura -> pirq_resolve_gsi false -> fallback
empirico, tutto senza una riga di log. Race intermittente, senza firma (un BAR
corrotto, un MSI armato sul device sbagliato sono nella stessa famiglia), e
invisibile su UP: ecco perche' il ferro monoprocessore non l'ha mai mostrata.

FIX (consolidamento D9, non pezza): nuovo componente arch/x86/pci_cfg.{h,c},
possessore UNICO del ciclo config sotto un solo spinlock irqsave (foglia del
grafo dei lock: la sezione critica non chiama nulla). API: pci_cfg_read32/
write32 e read8/write8 (i sub-word sono RMW del dword contenitore, atomici
rispetto a ogni altro ciclo). Migrati i tre ex-possessori: driver.c (kpci_*
ora delegano: SYS_PCI_READ/WRITE dei driver e MSI kernel passano dal lock),
irq/pirq.c (copie private rimosse), boot/disk.c (idem — li' la concorrenza non
c'era, ma la regola D9 vuole UN ciclo config nel kernel, non tre di cui una
"tanto sicura").

OSSERVABILITA' (il fallthrough non degrada piu' in silenzio):
  - "[IRQ ] 0:S.F: INTx non risolto dal chipset, discovery empirica (PID n)" —
    su ICH questa riga e' un'anomalia da investigare, non routine;
  - "[IRQ ] 0:S.F: GSI g rivendicato empiricamente (PID n)" — ogni claim
    empirico dichiara chi, cosa e dove.
Col b75 la storia dell'uhci era ricostruibile solo per assenza di righe; ora
ogni via di risoluzione lascia una firma.

Atteso su run-usb: "0:5.0: INTx risolto dal chipset -> GSI 20" per l'uhci
(condivisione legittima con l'AHCI di boot), zero timeout se il driver drena e
riporta (contratto not-ours -> irq_done); se i timeout persistessero CON l'uhci
risolto, il problema e' definitivamente nel driver (loop bloccato) e il log lo
dira' per nome. NOTA OPERATIVA: run-usb riusa test_disk.img esistente
("keeping data") — l'userland sul disco installato risale all'installazione;
per escludere driver stantii, rifare l'install dal live corrente prima del
test (rm test_disk.img, poi il flusso d'installazione).

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su pci_cfg.c, pirq.c,
driver.c, disk.c, kernel.c (resta il solo warning preesistente
BOOT_MODULES_MAX). Il Makefile del kernel auto-scopre i sorgenti: pci_cfg.c
entra nel build senza modifiche.
# MainDOB — PORT prescritto dalla 1.0 b145: routing INTx->PIRQ->GSI deterministico su ICH (build 75)

Contesto dal ferro e dai bootlog b74: il muto sulla linea 20 ora ha un nome — e'
usb_uhci (PID 16), NON il secondo ahci. Cioe' l'uhci ha CLAIMATO la linea 20
(quella dell'AHCI di boot) e poi tace: claim sbagliato. Meccanica: col piggyback
di b74 l'uhci in risoluzione viene notificato dai fire dell'AHCI; il suo test
"e' mio?" legge USBSTS, e con la pendrive GIA' inserita al boot l'uhci ha cause
VERE pendenti (connect/enumerazione in corso) — il test e' onestamente positivo
sul fire SBAGLIATO. Il claim empirico non puo' distinguere "la mia linea ha
sparato" da "ho uno status pendente mentre sparava la linea di un altro". La
stessa ambiguita' era gia' nota sui probe (header USBSTS del driver, b73); il
piggyback l'ha estesa alle linee legate.

La 1.0 su ferro vero non aveva il problema perche' NON risolveva: b145 spegneva
la consegna IOAPIC di default (Armada E500/PIIX4 = niente IOAPIC = PIC mode =
Interrupt Line valido; Extensa 5220/ICH8M = flag off = idem) e prescriveva la
correzione definitiva: "routing PCI INTx -> PIRQ -> GSI in APIC mode... PIRQ ->
GSI 16-23 secondo l'ICH". Questa build la implementa.

1. irq/pirq.c riorganizzato a DUE backend, mutuamente esclusivi per costruzione:
   - PIIX (invariato): macchine PIC-mode, rewiring via PIRQRC 0x60-0x63 + ELCR.
   - ICH (nuovo): LPC a 0:31.0 (vendor Intel, classe 06:01 — copre ICH6..ICH10:
     q35=ICH9, Extensa=ICH8M). pirq_init fotografa i DxxIR (D25..D31) dal
     chipset config space (RCBA, base a LPC 0xF0, finestra mmio_map di 0x20
     byte, smontata subito); RCBA spento -> default di reset 0x3210. Nuova API
     pirq_resolve_gsi(bus,slot,func): integrati D25..D31 dal nibble del pin nel
     DxxIR; slot esterni con lo swizzle ICH standard 4+((slot+pin-1)&3)
     (PIRQE..H); GSI = 16 + PIRQ, fisso sull'ICH. Verificato contro i bootlog:
     D31 SATA -> PIRQA -> GSI16 e AHCI in slot 4 -> PIRQE -> GSI20, esattamente
     cio' che la discovery empirica aveva imparato a fatica.

2. syscall/driver.c, sys_irq_register_pci (ramo IOAPIC): l'ordine di
   risoluzione diventa binding gia' appreso -> pirq_resolve_gsi (chipset) ->
   discovery empirica. Sull'hardware bersaglio con IOAPIC il GSI e' LETTO, non
   indovinato: la classe dei claim sbagliati (uhci su linea 20) muore alla
   radice, l'uhci lega il SUO GSI a register-time, il pending set resta vuoto e
   probe/backoff/piggyback restano l'estremo fallback per bridge ignoti.

3. kernel.c: pirq_init() era gated dietro il successo di intr_switch_to_ioapic
   — regressione sulla 1.0 (che la chiamava incondizionata): sull'Armada (PIC
   mode, switch mai eseguito) il PIIX non veniva rilevato e SYS_IRQ_WIRE_DEVICE
   era morto. Ora la detection e' incondizionata (pura lettura) e precede lo
   switch.

Effetti attesi: run-usb senza "timeout irq_done ... muti", uhci con GSI proprio
gia' al boot, hotplug pendrive in/out come sulla 1.0 (evento resume, zero
polling); Extensa 5220 puo' finalmente tenere la consegna IOAPIC (steering
per-CPU, GSI>15) con l'USB vivo; Armada invariato ma con SYS_IRQ_WIRE_DEVICE di
nuovo funzionante.

Verificato: sweep -fsyntax-only -Wall -Wextra pulito su pirq.c, driver.c,
kernel.c (resta il solo warning preesistente BOOT_MODULES_MAX in kernel.c).
Da validare sul ferro: Extensa (pendrive in/out, CD/DVD hotplug) e Armada
(regressione wire). Nota su QEMU: lo swizzle degli slot coincide con quello di
lpc-ich9 di QEMU/SeaBIOS ((slot+intx)%4+4), confermato dai GSI osservati.
# MainDOB — FIX forwarding IRQ: discovery su GSI condivisi, backoff dei probe, sharer sordi sganciati; rapporto di boot veritiero (build 74)

Quattro interventi correlati, tre nel forwarding IRQ (syscall/driver.c) e uno
nella sequenza di boot (kernel.c). Origine: hotplug USB morto in silenzio su
Q35/ICH9 (pirq supporta solo PIIX -> risoluzione INTx SOLO empirica) + spam
"linea 20: timeout irq_done (1 di 2 riportati)" nel log di boot.

1. DISCOVERY CIECA SUI GSI LEGATI (radice dell'USB morto). Il ramo probe del
   forward handler girava solo con sharer_count==0: se l'INTx del device in
   risoluzione condivideva il GSI con una linea GIA' legata (AHCI + UHCI sullo
   stesso PIRQ, il pane dell'ICH), il pending non veniva MAI notificato e non
   risolveva MAI — hotplug morto per costruzione, senza una riga di log (il
   driver uhci e' event-driven puro: niente polling PORTSC, per scelta). Ora i
   fire delle linee legate (GSI >= 16) notificano ANCHE i pending; i pending
   non entrano in pending_done (non sono sharer, contratto gia' rispettato dai
   driver: "not ours, stay pending"). sys_irq_pci_claim distingue il claim da
   piggyback: su linea gia' legata non tocca timer/pending_done del fire in
   volo degli sharer esistenti.

2. PARK PERMANENTE DEI PROBE. Un GSI di discovery assertito senza claim veniva
   parcheggiato per sempre (mask + drop del gate); nulla lo riarmava se non un
   NUOVO register_pci. Un solo assert orfano al boot (l'ac97 col backend rotto
   basta e avanza) uccideva l'hotplug del claimant futuro. Ora, FINCHE'
   esistono pending, il probe scaduto entra in backoff (PROBE_BACKOFF_MS=200:
   resta mascherato, il safety timer diventa la riapertura) — una sorgente
   level orfana e' limitata a un fire per periodo senza mai chiudere la porta
   alla discovery. Il park definitivo resta per gli assert a risoluzioni
   concluse (pending==0). In piu' la finestra di claim e' sua:
   PROBE_CLAIM_TIMEOUT_MS=250, non i 50 ms del protocollo done — il claimant
   deve svegliarsi e interrogare l'hardware, non solo riportare.

3. SHARER SORDI: IDENTITA' E SGANCIO. La contabilita' dei done era un contatore
   collettivo: il log del timeout non diceva CHI non riporta, e un doppio
   report di un driver saldava il debito di un vicino muto. Ora e' per-sharer
   (done_this_fire): il log del timeout elenca i PID muti; ogni timeout
   consecutivo e' uno strike e a IRQ_DEAF_STRIKES=8 il sordo viene sganciato
   dalla linea (i vicini smettono di pagare 50 ms a interrupt). QUALUNQUE done,
   anche tardivo dopo l'unmask forzato, azzera gli strike: un driver lento ma
   vivo non viene mai sganciato. irq_cleanup riusa lo stesso verbo di sgancio e
   sconta il debito del morto solo se lo doveva davvero.

4. RAPPORTO DI BOOT VERITIERO. Il riquadro finale ("Fase 6 operativa" — numero
   di un'altra era) veniva stampato DOPO gli spawn: ma lo spawn e' asincrono
   (moduli sulle altre CPU, molti parcheggiati su needs:) e kmain e' il thread
   idle, quindi il riquadro compariva in MEZZO al bring-up dei driver
   dichiarando operativo un sistema a meta'. Ora il rapporto chiude cio' che il
   kernel puo' GARANTIRE (stage 1-6, dicitura "kernel operativo") e precede lo
   spawn: l'ordine sullo schermo e' causale. idle_entry risale
   nell'orchestratore; stage rinumerati (rapporto=7, moduli=8).

Sintomi attesi in meno: nessuno spam "timeout irq_done" a regime (e quando
c'e', dice chi); pendrive USB rilevata anche con UHCI su GSI condiviso o dopo
assert orfani al boot; banner mai in mezzo ai log dei driver.

Nota preesistente (non toccata): kernel.c ridefinisce BOOT_MODULES_MAX (32 vs
16u di boot_info.h) — warning di ridefinizione nello sweep, da riconciliare.

Verificato: sweep -fsyntax-only pulito su driver.c e kernel.c (resta il solo
warning preesistente di cui sopra).
# MainDOB — FIX build system: BUILD_MODE non arrivava ai sub-make — nessuna debug_print (build 73)

Causa della "cecita' diagnostica" delle build 63..72 (make BUILD_MODE=debug ma zero
righe [USER] sul seriale, che ha fatto girare a vuoto piu' indagini): il Makefile
top-level esportava ROOT_DIR e BUILD_DIR ma NON BUILD_MODE. I sub-make (programs,
boot, drivers, e lo stesso kernel) fanno `include config.mk`, dove
`BUILD_MODE ?= release`: senza la variabile ereditata, ogni sub-make la
reimpostava a "release" e OPT_FLAGS perdeva -DMAINDOB_DEBUG. Risultato: TUTTE le
debug_print (gate #ifdef MAINDOB_DEBUG in unistd.h, valutato nel compilando) e le
diagnostiche dei driver venivano compilate via — anche in una build "debug".
Il valore da riga di comando esisteva solo nel processo make top-level.

Fix: `export ... BUILD_MODE` nel Makefile. Con l'export il `?=` di config.mk nei
sub-make non sovrascrive piu' (assegna solo se non definita), quindi
`make BUILD_MODE=debug` ora propaga -DMAINDOB_DEBUG a kernel, driver, servizi e
programmi in un colpo solo. Una riga, nel posto giusto (il build system), non una
pezza nei singoli Makefile.

Effetto: il prossimo `make BUILD_MODE=debug run` fara' finalmente parlare i driver
(righe [USER], [sata], [dobfs]) e le diagnostiche dell'installer aggiunte in b72
([MainDOB_Setup] copy ... FAILED rc=...): la fase 7/9 diventa leggibile.

Verificato: la modifica e' al Makefile; kernel e sorgenti invariati da b72 (bump
di build per tracciabilita').
# MainDOB — diagnostica mirata sul fallimento dell'installer in fase 7/9 (build 72)

L'errore si ripete in FASE 7/9 = INST_PHASE_CONFIG ("Scrittura della configurazione"):
non un caso, ma nemmeno una delle cause deterministiche che l'analisi statica poteva
inchiodare. Escluse per lettura diretta in questo giro:
  - permessi: il target in fase CONFIG e' la DFS SECONDARIA, che con mount_secondary
    salta l'intero sandbox (Associations e i font passano dallo stesso gate);
  - overflow di startup[8192]: gli snprintf incrementali clampano a (size - sp),
    troncano senza corrompere; ~40 righe brevi stanno larghe;
  - write parziale: sector_buf del DFS e' 64 KB, il chunk del Setup 16 KB — wn non
    viene mai troncato, quindi w==n sempre su un buffer sano;
  - race AHCI b71: colpirebbe fasi diverse a ogni run, non sempre la 7.

Cio' che resta e' un rc RUNTIME che nessuno stampava: gli helper del Setup
tornavano bool nudo, il DFS tornava lo status senza nominare la causa. Reso
visibile (integrato nei verbi, sotto #ifdef MAINDOB_DEBUG):
  - MainDOB_Setup: install_copy_diag(step, path, rc) — install_copy_file e
    install_write_string ora nominano QUALE dei quattro passi fallisce
    (open-src / open-dst / read / write), con rc e servizio target.
  - DobFileSystem: handle_write nomina il fallimento di ex.api->write (rc, wn, pos,
    path) sul percorso exFAT — il target di uno split install e' exFAT.

Nessun cambio di comportamento in release. Il prossimo run `make BUILD_MODE=debug`
stampera' la riga esatta al momento del fallimento: da li' il fix e' mirato.

Ipotesi in cima alla lista da confermare col log: esaurimento fd sul target
(fd_table a 256 con reaper degli orfani — ma un accumulo lungo le fasi OS/driver/
programmi potrebbe arrivare a quota in CONFIG), oppure un rc specifico di
ex.api->write/create sul volume exFAT.

Verificato: sweep -fsyntax-only pulito su Setup e DobFileSystem.
# MainDOB — FIX race del dispatcher AHCI: latch cmd_inflight one-shot (build 71)

Race reale nel driver ahci (intermittente, si aggrava con comandi back-to-back su
una porta — copie di tanti file piccoli, es. l'installer): cmd_inflight[p] veniva
ARMATO dall'issuer sotto enter_critical (flag + PxCI=1) ma AZZERATO fuori dalla
sezione critica, dopo ahci_wait_done. Nella finestra fra l'uscita del waiter e quel
clear, un secondo IRQ sulla stessa porta poteva:
  - leggere un cmd_inflight[p] ancora 'true' di un comando GIA' concluso e postare
    un wakeup stantio sul done_port -> il comando SUCCESSIVO riceveva il messaggio
    al posto del proprio e, in coincidenza con un vero ritardo, un "Command
    timeout!" fasullo (una scrittura fallita a caso);
  - oppure vedere il clear a meta' e non postare affatto.
La radice: inflight e done_port vivevano oltre la finestra in cui l'issuer li
possiede.

Fix, dentro il protocollo issuer<->dispatcher (nessuna guardia parallela):
  - il dispatcher CONSUMA cmd_inflight[p] come LATCH one-shot: sotto enter_critical
    (la stessa regione con cui l'issuer lo arma) verifica-e-azzera prima di postare
    il wakeup. Un completamento => un solo wakeup; un IRQ back-to-back trova il
    latch gia' consumato e non ri-posta nulla di stantio.
  - i due issuer (issue_cmd, issue_atapi) fanno ora una clear IDEMPOTENTE del latch
    sotto enter_critical, solo per i percorsi in cui il dispatcher non posta mai
    (fast-path "gia' done", errore PxTFD, timeout): nessun latch orfano per un IRQ
    tardivo.
  - commento del dispatcher riscritto: descriveva la vecchia lettura unguarded
    "benigna", ora documenta il latch.

Il waiter continua a validare PxCI/PxTFD a ogni risveglio: i wakeup spuri restano
innocui, ma non se ne generano piu' di sistematici.

Nota: se il timeout dell'installer persistesse, la conferma e' la riga
"[sata] Command timeout!" (runtime, non "init") nel log debug; la sua ASSENZA
sposterebbe la causa su dobfs/exFAT. Questo fix vale comunque di suo.

Verificato: sweep -fsyntax-only pulito su ahci (e kernel invariato da b70).
# MainDOB — FIX: installer a 0% — il Setup violava l'opacita' del routing block (build 70)

Sintomo: fase 1/9, "DFS secondaria avviata, attesa registrazione..." poi
"Errore: timeout attesa mount target". Il DFS secondario si registra DOPO il mount
(scelta anti-freeze documentata nel suo main): timeout = mount mai completato.

Radice (codice condiviso, latente anche sulla 1.0 — esplode solo con DUE controller
AHCI, la config di questa macchina): il layer block codifica il routing multi-istanza
NEL selector (block.c: native_selector = (inst<<8)|porta; il servizio giusto e'
"ahci" o "ahci_N") e il header lo dichiara OPACO. MainDOB_Setup lo violava:
provider hardcodato "ahci" + selector passato codificato. Il DFS secondario leggeva
il boot sector chiedendo all'istanza sbagliata una porta inesistente (256): mount
morto, timeout. DobDisk non soffre (passa dal layer che decodifica); i mount DAS
nemmeno (hotplug sostituisce $provider col servizio ANNUNCIANTE e il token porta la
porta cruda). Solo il percorso esplicito del Setup era rotto — su un controller
singolo (inst=0) coincideva col giusto, ecco perche' non s'era mai visto.

Fix, nel layer proprietario:
  1. libdob block.h/block.c — block_provider_binding(i, service_out, cap,
     native_selector_out): il verbo SANZIONATO che traduce il handle opaco nella
     coppia (servizio, selector nativo) per gli spawn di DobFileSystem --mount.
     Vive accanto alle classi e riusa i naming esistenti (ahci_svc_of_sel,
     usbms_%u): l'encoding non esce mai dal layer.
  2. MainDOB_Setup — entrambi i mount (boot-stub FAT32 e root exFAT split) usano il
     binding; via l'hardcode "ahci"/"ata" e il selector crudo.
  3. Igiene: warning -Wcomment preesistente (2349) silenziato.

Verificato: sweep -fsyntax-only pulito su Setup e libdob; kernel invariato da b69.
# MainDOB — igiene b68: il contratto dei timer anche nel header (build 69)

Verifica d'integrazione richiesta sul fix b68 — esito: i fix vivono NEI verbi
(detach_if_armed e heap_remove_at modificati direttamente, transizioni del pool
dentro sys_timer_set/cancel/cleanup, init-once nell'orchestratore
driver_syscalls_init). Due rifiniture emerse dal controllo:
  1. time/timer.h — il prototipo di timer_init era muto: il contratto
     (mai su un timer potenzialmente armato; per i pool init-once + arm/cancel)
     ora sta nel header, che e' la superficie di contratto dello sheet.
  2. syscall/driver.c — i due commenti impilati al discovery-arm fusi in uno.

Nessun cambio di comportamento. Verificato: sweep pulito (UP+SMP).
# MainDOB — FIX RADICE: la ruota timer si faceva decapitare — heap corrotto, NULL-write nel drain, fault "di dobinterface" (build 68)

Indagine sul page fault che uccideva dobinterface (cr2=0, eip=0xc0104e7c, scrittura
ring 0 "per conto utente"). Triangolazione dell'EIP via layout di link (ordine find
+ dimensioni .text ricompilate coi flag release): il colpevole cade in
syscall/driver.c — e la classificazione "per conto utente" copre anche i fault in
CONTESTO IRQ/drain attribuiti al processo di turno: dobinterface era la vittima piu'
probabile (e' chi ha piu' timer in aria), non il colpevole.

DUE difetti concatenati, stessa famiglia:

  1. DECAPITAZIONE DELL'HEAP (latente da sempre nella 1.1). I safety_timer delle
     linee IRQ venivano inizializzati SOLO dal discovery-arm dei GSI PCI: le linee
     legacy (tastiera 1, mouse 12, ata 14, ac97 11) armavano a ogni fire un timer_t
     AZZERATO in BSS — e heap_idx azzerato vale 0, non -1: detach_if_armed chiamava
     heap_remove_at(0) e RIMUOVEVA LA TESTA DELL'HEAP, il timer di qualcun altro.
     Ogni IRQ inoltrato poteva cancellare in silenzio uno sleep, un pacing, una
     sveglia: la sorgente piu' credibile delle stranezze storiche dei timer 1.1.
     Col churn giusto (due CPU, dobinterface a 60 Hz, ahci appena acceso dalla
     b65) l'heap degenerava fino alla scrittura su puntatore NULL nel drain:
     cr2=0, il fault osservato.
  2. RACE DEL POOL ASINCRONO: sys_timer_set faceva timer_init A OGNI riuso dello
     slot, fuori dal lock della ruota — azzerando heap_idx di un ktimer
     potenzialmente ancora armato dalla vita precedente; cancel/cleanup scrivevano
     used=false senza lock.

Fix integrato su tre livelli:
  - time/timer.c (la ruota si difende): detach_if_armed verifica L'IDENTITA'
    (heap_idx in range E s_heap[idx]==t) prima di toccare l'heap e riallinea il
    back-index; heap_remove_at con guardia i<s_count; contratto di timer_init
    documentato (mai su un timer potenzialmente armato; l'idioma per i pool e'
    init-once + arm/cancel: arm_locked gestisce gia' il ri-armo con detach+gen++).
  - syscall/driver.c (il pool rispetta il contratto): init-once in
    driver_syscalls_init di TUTTI i ktimer del pool asincrono E dei safety_timer
    di tutte le linee IRQ (il discovery-arm non ri-inizializza piu');
    sys_timer_set non chiama piu' timer_init al riuso; cancel e cleanup liberano
    lo slot sotto il pool lock DOPO timer_cancel.
  - I fire stantii delle vite precedenti restano filtrati dal gen++ (meccanismo
    esistente della ruota, ora mai piu' scavalcato).

Il respawn dei primary (b67) resta la rete per l'imprevisto; questo era il buco.
Conferma opzionale a build fatta: grep c0104e7c build/kernel.bin.map (la mappa e'
gia' prodotta dal link).

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP).
# MainDOB — FIX calibrazione b66 + PORT dalla 1.0: respawn dei servizi primary (build 67)

FIX (regressione b66): pit_calibration_window usava tsc_read() — la lettura
ANCORATA di tsc.h, che prima di tsc_init restituisce zero. Risultato del log:
"Calibrato: 0 MHz", calibrazione LAPIC trascinata a valori assurdi (1.006 MHz),
fallback periodico su una macchina col TSC sano e heartbeat incagliato. Ora la
finestra usa cpu_rdtsc() CRUDO, come la 1.0 — la finestra gira per definizione
prima che l'ancora esista. (Il fallback no-TSC che si e' incagliato resta da
collaudare per la classe 486: annotato, non riproducibile qui.)

PORT (occasione data dal page fault che ha ucciso dobinterface, PID 8: la 1.1
CONTIENE gia' i fault — "processo terminato, il kernel prosegue" — ma un vitale
morto non torna: freeze apparente):
  1. proc/process.h — is_primary nel PCB (flag "primary" del manifest, gia'
     scritto da mklive per settingsd e dobinterface, o auto-promozione).
  2. kernel.c — blocco "Primary: cache dei blob e respawn" accanto a
     spawn_module: bootfs muore a fine boot (il disco e' dei moduli), quindi il
     blob ELF di ogni primary viene COPIATO in cache al primo spawn (pochi MB per
     una manciata di servizi); lista di auto-promozione 1:1 con la 1.0
     (DobFileSystem, modules, dobinterface, hotplug, config, inputd);
     kernel_respawn_primary spawna dalla cache IGNORANDO il needs: originale
     (i servizi richiesti sono in piedi; parcheggiare un risorto su needs:video
     lo lascerebbe fermo per sempre) ma conservando i flags (privilegi).
  3. proc/process.c — process_destroy_local accoda il respawn via workqueue
     DOPO il rilascio di porte/IRQ/claim/registry (il risorto non trova nomi
     occupati dal proprio cadavere), per QUALUNQUE via di morte: fault, exit,
     kill. Il caricamento ELF non gira mai sotto i lock del lifecycle.
     Log di boot: i primary sono marcati "[primary]".

Resta APERTO il bug primario di quel fault: scrittura kernel a NULL
(cr2=0, eip=0xc0104e7c, ring 0 per conto di dobinterface). Il respawn e' la rete,
non la cura: servono simboli — dalla build: nm build/kernel.elf | grep c0104
(o addr2line -e kernel.elf 0xc0104e7c) e giro il fix.

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP).
# MainDOB — INTEGRAZIONE: niente piu' tick periodico sulle macchine con TSC; poll ata a ms onesti (build 66)

Revisione richiesta di stile: la b61 aveva AGGIUNTO il backend one-shot lasciando il
periodico come fase di boot ("Tick a 1000 Hz avviato" ancora in avvio) — una pezza,
non un'integrazione. La 1.0 un tick periodico non l'aveva PROPRIO: calibrava il TSC
con una finestra mode-0 SINCRONA (polling del readback, zero IRQ). Ora la 1.1 fa lo
stesso, con la decisione in UN punto solo.

  1. arch/x86/pit.c — nuovo verbo pit_calibration_window (port 1.0): mode 0,
     snapshot TSC fra byte basso e alto (il conteggio parte al byte alto), polling
     del readback fino al fronte OUT. Riusa os_program_count del backend one-shot.
     pit_init (periodico) resta SOLO come estremo fallback no-TSC e lo dichiara
     anche nel log ("Fallback periodico... TSC assente"). pit_silence eliminata:
     sul ramo LAPIC non c'e' piu' nulla da zittire.
  2. arch/x86/tsc.c — calibrazione sincrona su quella finestra (~50 ms, IF=0 ok):
     via il conteggio dei tick vivi. PIT_INPUT_HZ promosso in pit.h (condiviso).
  3. kernel.c, stage_time_online — l'orchestratore decide UNA volta:
     tsc_init sincrona -> se tsc_hz()==0 pit_init (fallback) -> clock -> sti ->
     splash. splash_hold_ms spinna sul TSC (tsc_busy_wait_ns); conta i tick solo
     nel fallback. Boot su macchina con TSC: il PIT non genera MAI un interrupt.
  4. time/event.c — via la chiamata (e il rito) pit_silence dal ramo LAPIC; log
     aggiornato ("PIT solo calibrazione"). krt/entropy.c: seed senza pit_ticks
     (a regime sarebbe zero), un giro di TSC in piu'.
  5. drivers/ata — INTEGRAZIONE al posto dello shim b63: i CALLSITE parlano
     millisecondi reali (2'400 probe/identify, 12'000 wait dati, 60'000 flush;
     1'200/6'000 i passi ATAPI), le firme dicono budget_ms, lo shim x12 e la sua
     costante sono eliminati. Scoperto e integrato anche il TERZO poll rimasto a
     spin 1.0 (atapi_wait_bsy_clear/atapi_wait_drq): ora sono prese sullo stesso
     ide_poll_core cooperativo — un solo motore di attesa nel driver.

Voce di boot attesa: sparisce "Tick a 1000 Hz avviato"; il modo eventi recita
"LAPIC ...; PIT solo calibrazione" (o, senza LAPIC, la sorgente PIT one-shot).

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP) e driver ata.
# MainDOB — FIX: gate IDT assenti sui vettori dei GSI PCI — #GP vaganti e IRQ AHCI mai consegnati (build 65)

Con la b64 DobDisk vede modello e capacita' veri; restavano: "Impossibile scrivere
l'MBR", un #GP che ammazzava ata.mdl (err=0x182), icone DAS lente/assenti.

Decodifica del fault: err 0x182 = bit IDT + indice 48 = consegna attraverso il
vettore 0x30 con GATE ASSENTE. Radice: intr_switch_to_ioapic arma i GSI PCI 16..31
sul vettore IRQ_BASE_VECTOR+gsi (48..63, per design: "vector-32 recupera il GSI"),
ma gli stub ISR coprivano solo 0..47 e 0x50..0xDF: la finestra 0x30..0x3F era un
BUCO. In piu' l'aggancio dei handler per gsi>=16 non avveniva mai:
irq_register_handler e intr_bridge_legacy_handler erano entrambi no-op sopra la
linea 15. Conseguenze: il primo INTx di un GSI armato diventava #GP(IDT,48) sul
processo di turno (ata ucciso da innocente, PID riciclato subito dopo — nel log:
FAULT eip=0x00400584 err=0x182); gli IRQ dell'AHCI non arrivavano MAI al driver:
comandi runtime in timeout, scrittura MBR fallita, letture lente via fallback.

Fix (nel disegno esistente, nessun percorso nuovo):
  1. arch/x86/isr_stubs.asm — il blocco generato parte da 0x30: stub 48..223.
  2. arch/x86/intr.c — VEC_GSI_FIRST 0x30 (finestra identita' 32+gsi, RISERVATA:
     l'allocatore MSI resta a 0x50); DECL_VSTUB/VSTUB estesi a 48..79; il gate in
     intr_set_vector_handler si installa da VEC_GSI_FIRST; intr_bridge_legacy_handler
     per line 16..31 esegue il percorso pieno (binding + isr + gate) — sotto 16
     resta il solo binding, gate e dispatch di irq.c invariati.
  3. syscall/driver.c — pci_discovery_arm: rimossa la irq_register_handler no-op
     per gsi>=16 (l'aggancio vero e' il bridge), con commento del perche'.

Catena verificata: irq_forward_handler deriva gia' la linea da vector-32 con
guardia MAX_IRQ_FORWARD=32; acpi_resolve_gsi e' identita' sopra 15 (mask/unmask
corretti); l'EOI di intr_dispatch copre la finestra 0x30 via ramo g_ioapic_active.

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP). L'asm e' testuale
(nasm assente qui): la build reale lo assembla.
# MainDOB — FIX RADICE: sys_dma_alloc violava il contratto ABI — disco fantasma 0 KB e DMA del device nella RAM bassa (build 64)

Diagnosi chiusa dallo screenshot di DobDisk: "SATA 0 - (senza nome) - 0 KB,
0 settori" — il disco fantasma che il commento di port_setup nomina come il male
da evitare. IDENTIFY "riuscita" ma a zeri = coppia vaddr/phys scollegata.

Radice: sys_dma_alloc (1.1) consegnava l'indirizzo FISICO scrivendolo nei primi
4 byte del buffer, con un commento che lo dichiarava "contratto 1.0". Il contratto
1.0 VERO — verificato sul sorgente 1.0 e sulla libc condivisa fra i due alberi,
che legge con vincolo "=b"(phys) — e' fisica in EBX, virtuale in EAX. La libc
leggeva quindi l'EBX salvato, cioe' l'argomento SIZE rimasto nel registro: ogni
driver otteneva phys = size richiesta.

Conseguenze (tutte osservate):
  - L'HBA AHCI veniva programmato con command list/FIS/dati a "fisici" come 1024,
    256, 4096: il DEVICE leggeva e SCRIVEVA via DMA nelle prime pagine della RAM
    fisica — corruzione silenziosa di sistema, che spiega le icone DAS in ritardo
    o assenti e la generale inaffidabilita' dello storage sulla 1.1.
  - Il buffer vero del driver non veniva mai toccato dal device: IDENTIFY
    completava (PxCI) e il driver parsava 512 byte di zeri -> modello vuoto,
    capacita' 0, il disco fantasma in DobDisk; letture dati (settore 0 delle
    partizioni, CD) a zeri o fallite -> niente partizioni, CD "inusabile".
  - I primi 4 byte di OGNI buffer DMA venivano clobberati dal kernel.
  - Stessa sorte per il percorso BMIDE dell'ata su macchine IDE (mascherata dal
    fallback PIO) e per i driver USB.

Fix: regs->ebx = phys prima del return (il frame ISR ripristina i registri con
popa: il valore arriva alla libc), rimossa la scrittura nei primi 4 byte.
Commento riscritto con il contratto vero e la storia dell'errore.

Nota: i fix b63 (budget poll ata) e la catena INTx restano validi e ortogonali;
questo era il guasto primario dello storage 1.1.

Verificato: sweep -fsyntax-only pulito (UP+SMP).
# MainDOB — FIX regressione b50: budget di poll ATA tagliati di 12x — dischi invisibili (build 63)

Sintomo (1.1, live e non): DobDisk senza dischi/partizioni, icona del CD presente
(ma inusabile), storicamente "sempre una race". Diagnosi: l'unico delta
comportamentale del driver ata fra 1.0 e 1.1 e' il poll cooperativo b50. I budget
1.0 contavano ITERAZIONI (~12 us l'una su QEMU): 200'000 = ~2.4 s REALI,
5'000'000 = ~60 s (il flush, citato cosi' anche nei commenti d'epoca) — e su
macchine lente le iterazioni rallentano, quindi il budget SCALAVA col ferro.
La conversione b50 (us/1000) li aveva ridotti a 201 ms / 5 s: taglio silenzioso
di 12x E perdita dell'auto-scala. Su host lenti o sotto carico di boot,
l'IDENTIFY del primary master dopo il soft reset non rientrava nel budget:
"[ata] No drive found, running anyway" — nessun disco in hdd_slots, quindi
niente LIST_DISKS per DobDisk e niente partizioni dal pusher; lo scan ATAPI
(piu' tollerante) trovava comunque il CD: icona presente, race confermata.

Fix: ide_poll_budget_ms() — fattore IDE_POLL_LEGACY_US_PER_ITER = 12 documentato:
i budget EFFETTIVI tornano identici alla 1.0 (2.4 s / 12 s / 60 s), il poll resta
cooperativo (finestra calda di 200 campioni, poi 1 campione/ms DORMENDO): 60 s di
budget costano zero CPU, nessun ritorno dello spin da 12 secondi che b50 curava.

Se il sintomo persistesse anche con la b63, il log seriale del boot resta il passo
successivo (righe discriminanti: "[BOOT] modulo ... (driver)", "[ata] ...",
"[IRQ ] linea 14 -> driver", "[sata] ...").

Verificato: sweep -fsyntax-only pulito (driver ata; kernel invariato da b62).
# MainDOB — igiene b61: sezioni a posto e codice morto depennato (build 62)

Revisione di stile sul port del tick source (b61), piu' potatura.

  1. arch/x86/pit.c — la superficie vtable del backend one-shot (ts_pit_* +
     export) era finita DENTRO la sezione Verbi, prima di on_tick: spostata in
     coda al file in una sezione propria ("Orchestratore: backend tick source"),
     lasciando in alto solo costanti, stato e verbi os_*. Ordine finale: Verbi ->
     Orchestratore (periodico) -> Orchestratore (backend one-shot, vtable).
  2. arch/x86/lapic.c — la superficie tick_source interrompeva l'API pubblica a
     meta' file: spostata in coda.
  3. pit_set_tickless(bool) -> pit_silence(void) — il ramo di riattivazione del
     periodico (false) non aveva NESSUN chiamante: depennato. Il fallback estremo
     (TSC non calibrabile) si decide al boot e non torna indietro a runtime;
     l'API ora dice esattamente cio' che fa. Aggiornati pit.h e time/event.c.

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP). Nessun cambio di
comportamento rispetto a b61 oltre alla rimozione del ramo irraggiungibile.
# MainDOB — PORT dalla 1.0: modo eventi anche senza LAPIC — tick source PIT one-shot (build 61)

Correzione di rotta sull'audit b60: la 1.0 NON ripiegava sul PIT periodico quando il
LAPIC mancava — aveva un'astrazione tick_source con backend PIT ONE-SHOT (8254
canale 0 in mode 0, riprogrammato per SCADENZA), la stessa macchina event-driven a
grana 838 ns. Il commento in pit.h della 1.1 lo dichiarava perfino come lavoro
pianificato ("il tick source definitivo arriva in 1.1.1"). Portato, adattandolo
all'architettura 1.1 (deadline assolute su monotono TSC; il PIT periodico resta come
heartbeat di early-boot e come estremo fallback se il TSC non e' calibrabile).

  1. time/tick_source.h (nuovo) — vtable {install, arm_deadline_ns, disarm,
     register_callback, name, max_arm_delta_ns}; superficie unica per i backend.
  2. time/tick_source.c (nuovo) — selettore puro, una volta: LAPIC se
     lapic_available && lapic_timer_usable, altrimenti PIT one-shot. Mai NULL.
  3. arch/x86/pit.c — backend one-shot (port fedele della 1.0): mode 0, ns->count
     via udiv64_u32, orizzonte singolo 54.925 ms (16 bit su 1.193182 MHz) e RIARMO
     IMPILATO nell'ISR per le scadenze oltre il tetto — i fuochi intermedi NON
     invocano la callback; disarm = flag + parcheggio a conteggio massimo, con
     tolleranza al singolo fuoco spurio residuo; install sostituisce l'ISR
     periodico via irq_register_handler (che smaschera IRQ0) e congela
     s_ticks/pit_uptime_ms (in modo eventi il monotono e' il TSC, come sul LAPIC).
     Stato toccato solo a IF=0 (arm sotto irq_save, ISR fino all'iret): zero lock.
     Backend globale: senza LAPIC niente AP, UP garantito. Stessa dieta di entropia
     del periodico (un rdtsc per fuoco).
  4. arch/x86/lapic.c — vtable tick_source_lapic: presa standard sulle funzioni
     esistenti (install no-op: lapic_init e' gia' corso in stage_smp_online).
  5. time/event.c — il motore dispatcha via s_src (scelto una volta): refresh
     arma/disarma il backend senza ramificare sull'hardware; try_enable ha come
     UNICO prerequisito assoluto il TSC calibrato; pit_set_tickless(true) SOLO sul
     ramo LAPIC (sul ramo PIT, IRQ0 E' la sorgente). event_fire identica per
     entrambi i backend.

Ordine verificato: la calibrazione TSC conta i tick del PIT PERIODICO in early-boot,
prima della selezione; il selftest heartbeat (stage 6) esercita la catena completa
sul backend scelto. Nessun sito arma il LAPIC scavalcando il backend (audit grep).

Per l'Armada E500 (Celeron con LAPIC fuso): stesso tickless della macchina moderna,
si sveglia solo a scadenza armata — non piu' 1000 interrupt al secondo.

Verificato: sweep -fsyntax-only pulito su kernel intero (UP+SMP).
# MainDOB — AUDIT hardware (E500 / Extensa / storage) + cintura no-LAPIC (build 60)

AUDIT, fatti verificati sul codice:

E500 (no LAPIC, singlecore, IDE) — il kernel 1.1 e' PROGETTATO per girarci:
  - time_event_try_enable rifiuta il modo eventi senza LAPIC/TSC: resta il PIT
    periodico a 1000 Hz, il cui on_tick porta il motore completo (timer_on_tick +
    scheduler_tick + switch post-EOI).
  - Consegna IRQ: default PIC 8259; intr_switch_to_ioapic e' esplicitamente no-op
    senza IOAPIC (il commento in kernel.c nomina l'Armada). Il fix b50 delle spurie
    7/15 e' PIC-corretto.
  - smp_boot_aps: no-op senza LAPIC; kick IPI sotto #ifdef MAINDOB_SMP e comunque
    solo verso CPU online diverse dalla corrente.
  - ata IDE: poll cooperativo (b50) + IRQ 14/15 via PIC.
  Aggiunta in questa build: guardia no-LAPIC in lapic_send_ipi (deref NULL teorica
  ma a costo di un confronto). VERDETTO: si', deve funzionare; da collaudare.

Extensa 5220 — i driver (ahci, usb_uhci/ehci/xhci, usb_mass_storage, usb_common)
sono IDENTICI AL BYTE fra 1.0 e 1.1: gli "interventi importanti su pcie/DMA" vivono
nei driver e nei servizi kernel. Il kernel 1.1 ha pirq_init (bridge PCI-ISA),
intr_switch_to_ioapic con INTx risolti empiricamente e MSI. Da verificare a caldo:
consegna IRQ ai driver utente su quella macchina (serve il log seriale).

dobDisk senza dischi sulla 1.1 — il "doppio processo ahci" e' BY DESIGN (elezione
multi-controller in due fasi, un'istanza per controller; il nome "ahci" va a chi
trova dischi). Con driver identici, il sospetto e' kernel-side: consegna IRQ AHCI
(routing IOAPIC/PIRQ nuovo della 1.1) -> comandi in timeout -> zero dischi.
SERVE IL LOG SERIALE di quel boot per inchiodarlo.

CD/DVD vs partizione (entrambi i kernel) — catena verificata e SANA sulla carta:
firma PxSIG distingue ATA/ATAPI; le partizioni annunciano volume_fs (fat32/exfat,
class 0) e gli ottici class 01/05 (volume_fs 0); il match DAS a filtri ortogonali
non puo' confonderli; partition_exfat.das esiste con mount fs=exfat. Il sintomo
riportato deve nascere altrove (decodifica token al mount? topologia porte?):
servono la topologia esatta (che porta ha il disco, che porta il lettore) e il log.

Verificato: sweep pulito (UP+SMP).
# MainDOB — PORT dalla 1.0: overflow IPC per payload > 64 KiB (build 59)

Esplorando "i fix per alleggerire DobWrite" sull'albero 1.0: i sorgenti di DobWrite
sono IDENTICI al byte nei due kernel, e lo stub differisce solo per i miei
b51/b58 — la differenza vera e' nel KERNEL. La 1.0 ha un percorso di consegna per i
payload oltre IPC_BUF_SIZE che la 1.1 trapiantata non ha mai ricevuto: e' il motivo
per cui la' la pagina di DobWrite (upload monoblocco da ~6 MiB) arrivava intera, e
il commento originale lo dichiara ("file reads, framebuffers").

Portato (semantica 1.0, adattata alla 1.1):
  1. proc/process.h — ipc_overflow_vaddr/pages/capacity nel PCB: regione overflow
     PER-PROCESSO e PERSISTENTE (high-water mark). A regime la consegna e' una sola
     memcpy: zero churn pmm/paging sui client streaming.
  2. syscall/syscall.c — ipc_free_overflow + percorso medio/grande in
     ipc_copy_to_user_buf: vregion in 0x6000_0000 (fallback ricerca larga), frame
     contigui con fallback per-frame, rollback completo su alloc parziale, clamp
     finale con UNA riga di log per boot (niente piu' strip silenziosi). Adattamenti
     1.1: firma vregion_alloc con backing_id; vm_lock attorno a grow/riuso (SMP;
     contesto receive, puo' dormire). Al teardown del processo pensa la distruzione
     generica dell'AS (la regione e' una vregion utente committata come le altre).

Scelta consapevole: bande (b58) e segmentazione cmdbuf (b51) RESTANO — viaggiano nel
fast path da 64 KiB ed evitano il picco di kmalloc kernel-side dello snapshot per i
frame grandi. L'overflow ripara alla radice tutti i protocolli monoblocco rimasti
(letture file grandi in testa) e i futuri.

Verificato: sweep -fsyntax-only pulito su kernel (UP+SMP); userspace invariato da b58.
# MainDOB — FIX: icone sparite dopo il resize + DobWrite a pagina bianca sulla 1.1 (build 58)

Due bug distinti, entrambi sul percorso texture del compositor.

FIX A — icone della ribbon sparite dopo UN resize (esplora file, DobWrite,
DobPicture). Contratto documentato nello stub client: al resize il WM distrugge il
tex pool della finestra e il client azzera il proprio mirror SENZA mandare TEX_FREE,
poi ri-alloca alla prima BlitBuffer. Il nuovo percorso resize (b54) conservava il pool
lato server ("miglioria"): il client ri-allocava sopra gli orfani, il pool (16 slot)
si riempiva e le alloc successive fallivano — icone nel vuoto. Ripristinato il
contratto: win_apply_geometry distrugge il pool insieme alla vecchia surface.

FIX B — DobWrite: ribbon ok, DOCUMENTO SEMPRE BIANCO su tutta la 1.1 (mai
funzionato dal cambio di trasporto). La pagina e' una BlitBuffer grande che viaggia
come GUI_WIN_TEX_UPDATE monoblocco (payload = w*h*4, ~6 MiB): il buffer IPC del
ricevente strippa i payload oltre 64 KiB (vedi b51) e il handler faceva
"if (!payload) break" — upload morto in silenzio, testura rimasta a zero, blit
pixel-alpha di soli zeri = niente, corpo bianco col solo caret (che e' un fill
separato). Sulla 1.0 il contenuto viaggiava via SHM: per questo la' funzionava.
Cura: upload A BANDE DI RIGHE INTERE — arg2 = riga iniziale, arg3 = numero righe
(0 = legacy monoblocco, invariato per le testure piccole) — e il WM applica ogni
banda subito con dv_texture_update_region: NESSUNO stato di riassemblaggio
(fire->shoot->forget), una banda persa lascia una striscia stantia che il prossimo
upload risana da solo.

Nota E500 (annotata, non implementata): la pagina di un editor come testura VRAM
(~6 MiB) non sta negli 8 MiB dell'Armada accanto al primary. Candidato: le texture
DV_TEX_FLAG_DYNAMIC in RAM di sistema (le texture SONO surface: il backing SYSRAM
b52 e' gia' pronto).

Verificato: sweep -fsyntax-only pulito (compositor, stub); kernel/driver invariati
da b57.
# MainDOB — FIX RADICE: sbrk nel boomerang agiva sul processo SBAGLIATO — heap del driver corrotto, "schermo che si auto-fagocita" (build 57)

Sintomo (b56, screenshot): la finestra dell'esploratore viva e trascinabile ma il suo
corpo era hash di righe blu-desktop e nere-wizard con brandelli del SUO stesso testo
sfilacciato — lo schermo dentro lo schermo. Firma di blocchi heap SOVRAPPOSTI nel
driver: la surface del corpo condivideva memoria con la shadow, la compose scriveva il
desktop ATTRAVERSO la surface e poi la blittava gia' trita.

Radice (modello migrating-thread): dv_surface_create/destroy — e quindi malloc/free
del driver per i backing SYSRAM e per la shadow — girano DENTRO la fase in-driver del
boomerang: CR3 del DRIVER installato, ma process_current() e' il CHIAMANTE. sys_sbrk
usava process_current(): aggiornava brk_current e vm_regions del chiamante mentre
paging_map_page mappava nel CR3 del driver. Contabilita' e mappature su processi
DIVERSI; con chiamanti diversi (wizard, esploratore) la malloc del driver riceveva
break incoerenti fra loro e heap_grow restituiva blocchi sovrapposti a shadow/backbuf.
Il danno cresce col numero di client e con la taglia delle surface: invisibile nei
collaudi b52-b54 (un client, surface piccole), esploso con l'esploratore (secondo
processo + churn da 6 MB del resize).

Fix:
  1. kernel/syscall/driver.c — video_boomerang_in_driver_phase() (slot per-CPU
     ACTIVE, stessa sorgente del recupero fault b48) e
     video_boomerang_as_process(): il processo il cui AS e' installato ADESSO —
     il driver video nella fase in-driver, process_current() altrimenti.
     Prototipi in video_boomerang.h.
  2. kernel/syscall/syscall.c — sys_sbrk usa video_boomerang_as_process():
     contabilita' del brk e mappature vivono nello STESSO processo, quello del CR3
     attivo. E' il contratto giusto per QUALUNQUE syscall AS-affine chiamata dalla
     fase in-driver; oggi sbrk e' l'unica che il driver eserciti (malloc/free).
  3. drivers/bga — hardening trovato in audit: bga_gpu_reset_full ora libera i
     backing sys_pixels prima di azzerare le tabelle (prima: leak a ogni GPU reset).

Nota di collaudo: questo bug puo' aver lasciato contabilita' heap gonfiata nel
processo di dobinterface nelle build 52-56 (regioni segnate committed senza pagine
reali nel SUO AS): la 57 va provata da boot pulito.

Verificato: sweep -fsyntax-only pulito su kernel (UP+SMP) e bga; compositor/libdob
invariati da b56.
# MainDOB — FIX: view-through residuo in single buffer — shadow framebuffer in bga (build 56)

Sintomo (b55): il grosso dei lampi sparito, ma restava il view-through delle finestre
SOTTOSTANTI nelle zone di sovrapposizione. Causa: in single buffer la compose scrive
nel buffer VISIBILE strato per strato (backbuf -> finestra sotto -> finestra sopra):
lo scanout puo' cogliere gli stati intermedi. Le superfici (b54) hanno reso finali i
pixel DI OGNI STRATO, ma non l'ordine con cui gli strati toccano lo schermo.

Cura: shadow framebuffer nel driver, il pattern classico dei WM single-buffer
(shadowfb). Sotto BGA_DOUBLE_BUFFERING == 0:

  1. bga_state — G->shadow: staging fullscreen in RAM di sistema, allocata da
     apply_mode alle dimensioni del modo, liberata da primaries_free. NULL (malloc
     fallita) = fallback compose diretta, corretta ma con gli stati intermedi.
  2. dv_compose — back_desc punta alla shadow (sys_pixels; il dispatch di
     surface_pixels e' quello di b52): TUTTA la composizione avviene fuori schermo.
  3. dv_page_flip — presentazione = UNA fast_copy32 sequenziale shadow -> primary:
     l'unica scrittura che lo scanout veda mai sono pixel finali. Il toggle di pagina
     viene saltato (le pagine coincidono gia' dalla b53). Fence firmata come prima.
  4. dv_compose_rect_present (nuova, prototipo in bga_state.h) — il case
     DV_COMPOSE_RECT del transport smette di essere un alias muto di dv_compose: la
     compose resta piena (BGA non ha lo scissor) ma la PRESENTAZIONE copia la sola
     banda richiesta. Senza questo, fb_flip_rect (che non chiama page_flip) avrebbe
     composto nella shadow senza mai presentare: schermo fermo.

Bilancio: zero VRAM aggiuntiva (il punto per gli adattatori da 8 MiB: il "doppio
buffer" e' migrato in RAM di sistema, che abbonda), una copia ~3 MiB per frame a
1024x768 — sequenziale, il pattern shadowfb storico dell'hardware d'epoca. La copia
gira nella fase boomerang a IF=0 come gia' la compose: latenza IRQ per-frame circa
raddoppiata rispetto alla sola compose, entro il budget del bounded spin (b48). Il
percorso double buffering (interruttore a 1) e' intatto e ignora la shadow.

Nota per il porting mach64/E500: stesso innesto (shadow + present-copy); il suo
compose_rect scissorato vero potra' copiare la sola banda anche in compose.

Verificato: sweep -fsyntax-only pulito (-Wall -Wextra pieni) sui tre TU bga; kernel,
libdob e compositor invariati da b55.
# MainDOB — FIX di link b54: dv_surface_clear non raggiungibile + igiene warning (build 55)

Il link di dobinterface falliva su dv_surface_clear: l'opcode era dichiarato nel
protocollo e IMPLEMENTATO in bga, ma mancavano i due anelli in mezzo — il case nel
dispatcher del driver e il wrapper libdob. La sweep -fsyntax-only non linka e non
poteva accorgersene: lezione registrata (le funzioni nuove vanno cercate anche nel
transport e in libdob, non solo nel header).

  1. drivers/bga/bga_transport_fast.c — case DV_SURFACE_CLEAR (a0 = handle,
     a1 = colore packed, stessa convenzione di DV_FILL_RECT; nessun payload).
  2. libdob/src/video.c — wrapper dv_surface_clear (dv_call2 + pack_color).
     L'implementazione driver esistente (fast_fill32 contiguo) e' corretta anche per
     le surface SYSRAM: pitch_words == width in entrambi i backing.

Igiene warning segnalate dalla build reale (i686-elf-gcc):
  3. Indentazioni fuorvianti (doppio if sulla stessa riga) separate in
     win_v_draw_text, mc_v_fill_rect, mc_v_draw_text.
  4. Sign-compare: cast espliciti in win_v_blit_inline (G_BLIT_SCRATCH_DIM) e nel
     guard di GUI_WIN_TEX_ALLOC (SCREEN_W/H).
  5. compositor_repaint_rect e icon_draw_band: orfane da quando i repaint parziali
     promuovono a full — sono la base del prossimo stadio (dirty-rect con le
     superfici), annotate __attribute__((unused)) con commento invece di cancellarle.

Verificato: sweep -fsyntax-only pulito con -Wall -Wextra PIENI (niente soppressioni)
su compositor, libdob e bga; kernel invariato (UP+SMP pulito).
# MainDOB — REFACTOR: finestre e backbuf a SUPERFICI, compose = soli blit (build 54)

Chiude i "lampi infiniti e view-through" osservati in single buffer (b53): erano il
clear->replay del modello retained eseguito nel buffer VISIBILE. Ora ogni piano di
disegno grande e' una surface in RAM di sistema (DV_SURF_FLAG_SYSRAM, b52) disegnata
UNA volta per cambiamento con draw diretti; la compose blitta pixel FINALI dal basso
verso l'alto — niente stati intermedi da nascondere, quindi il single buffer e' pulito
per costruzione e il double buffering (depennato in b53) diventa un'opzione, non un
obbligo. Sull'Armada E500 (8 MiB VRAM) questo libera il secondo primary.

Protocollo e driver:
  1. libdob + bga — dv_blit_pixel_alpha: variante per-pixel del blit diretto (stessa
     convenzione del ramo use_pixel_alpha delle cmdlist: 0xFF000000 = trasparente).
     Trasporto: arg1 di DV_BLIT_ALPHA (prima sempre 0) = use_pixel_alpha. Serve al
     bake dei record blit_inline/blit_tex, che ne erano privi nel percorso diretto.

Compositor (dobinterface):
  2. window_t: via dv_cmdlist_t cmdlist + cmdlist_cap, dentro dv_surface_t body_surf
     (SYSRAM, surf_w x surf_h). win_alloc_video crea surface+layer (.source=surface);
     win_free_video la distrugge. Il tex pool ora SOPRAVVIVE al resize.
  3. Visitor win_v_*: emettono draw diretti sulla surface del corpo (logica identica:
     clipping, reveal math del typed-text, scratch ring per blit_inline). blit_inline/
     blit_tex via dv_blit_pixel_alpha.
  4. win_rebuild_cmdlist -> win_bake: chrome + sfondo corpo + replay del last_cmdbuf
     in draw diretti, una volta per cambiamento (consumatori di surface_dirty
     invariati). Lo sfondo bianco del corpo PRIMA del replay rende il corpo opaco: i
     pixel non coperti dal client non lasciano piu' trasparire i layer sottostanti
     (uno dei view-through storici). win_grow_cmdlist eliminata; niente piu' tetti di
     capacita' ne' pressione sul pool cmdlist per le finestre.
  5. Resize (win_apply_geometry): nuova surface -> blit_stretched della vecchia come
     segnaposto -> rebind del layer (handle stabile) -> destroy della vecchia; il
     client riceve GUI_EVT_RESIZE come prima e il frame fresco sovrascrive.
  6. Backbuf: da cmdlist 64 KiB a surface SYSRAM fullscreen. fb_fill_rect/hline/vline,
     font_draw_string, blit di icone (pixel-alpha) e thumbnail MC sono draw diretti;
     compositor_rebuild apre con dv_surface_clear(COLOR_BG) — che fa anche da sfondo
     desktop. RITIRATO il layer bg a z=-100: il suo fill fullscreen rigiocato a ogni
     compose era il lampo bianco principale. Al cambio modo la surface backbuf viene
     ricreata all'estensione nuova (crea -> rebind -> distruggi).
  7. last_cmdbuf resta come DATO (animazione di apertura, thumbnail Mission Control,
     ricostruzione post-release): non e' piu' sul percorso di compose. La sua
     eliminazione (thumbnail via downscale+download, retention scoped all'anim) e' il
     prossimo passo annotato.

Invariati: toast e wpanel (cmdlist piccole su layer propri), pacing 16 ms, percorso
cursore, protocollo client (il cmdbuf segmentato b51 e' ortogonale).

Verificato: sweep -fsyntax-only pulito su kernel (UP+SMP), bga (3 TU), libdob,
compositor e stub. Da collaudare in QEMU: spostamento/resize finestra e mouse senza
lampi ne' view-through in single buffer; icone e widget del tray con la trasparenza
giusta; typed-text all'apertura; Mission Control; cambio risoluzione.
# MainDOB — DEPENNATO (temporaneo): double buffering del driver bga (build 53)

Banco di prova per il refactor "finestre a superfici": con BGA_DOUBLE_BUFFERING = 0
(drivers/bga/main.c) il driver alloca UNA sola pagina primary e fa coincidere back e
front (primary[1] == primary[0]): tutto il resto del codice — flip, compose,
scanout_program_back — gira identico, ma la compose disegna nel buffer VISIBILE. I
flash intermedi del modello retained (clear -> replay di tutte le cmdlist) diventano
cosi' OSSERVABILI: sono la baseline che il refactor deve azzerare (compose = soli blit
di pixel finali). Libera meta' del budget primary (~3 MiB a 1024x768) — cio' che serve
agli adattatori da 8 MiB (Armada E500). Interruttore, non rimozione: a refactor
compiuto il double buffering torna OPZIONALE per-driver, non obbligato dal compositor.
Tre punti sotto #if: primaries_alloc (seconda pagina), primaries_free (guardia contro
il doppio free della pagina condivisa — resta anche a interruttore su 1, e' corretta
in entrambi i modi), VIRT_HEIGHT (h invece di h*2).

Verificato: sweep -fsyntax-only pulito.
# MainDOB — FONDAMENTO: surface in RAM di sistema nel driver video (build 52)

Primo passo del piano "finestre a superfici" (fire->shoot->forget + single buffer sugli
adattatori con poca VRAM, es. Compaq Armada E500 / 8 MiB). Solo strato abilitante: nessun
comportamento cambia finche' il compositor non lo usa.

  1. libdob/include/dob/video.h — DV_SURF_FLAG_SYSRAM: surface coi pixel nella RAM di
     sistema del driver. Zero costo VRAM, zero quota VRAM. Per contenuti composti via
     blit (corpi finestra).
  2. drivers/bga — surface_t.sys_pixels; surface_pixels() dispatcha sysram/VRAM (le
     primitive sw_* non distinguono); create/destroy/vproc-detach gestiscono il backing
     giusto. Le texture in bga SONO surface (dv_texture_create delega): il percorso di
     bake dei blit_tex non richiede opcode nuovi.

Piano dei passi successivi (annotato per il changelog, non ancora implementato):
  - dobinterface: corpo finestra = surface SYSRAM disegnata UNA volta all'Invalidate
    (draw diretti dv_* via visitor); layer.source = surface; via le cmdlist per-finestra
    (grow/rebuild/pool) e il replay per-compose. Il minimize smette di liberare il video
    (surface sysram = zero VRAM): restore istantaneo. last_cmdbuf resta SOLO come dato
    per l'animazione di apertura (scope: durata anim) e per le thumbnail di Mission
    Control (da migrare a downscale+download in un passo dedicato).
  - resize: nuova surface + blit_stretched della vecchia come segnaposto + frame fresco
    dal client (evento gia' esistente).
  - compose = soli blit di pixel finali -> il single buffer diventa possibile senza
    flash (mai clear-then-redraw nel visibile): sull'E500 libera il secondo primary
    (~3 MiB su 8). Poi dirty-rect per non riblittare lo schermo intero a ogni cursore.

Verificato: sweep -fsyntax-only pulito su kernel (UP+SMP), bga, compositor e stub (le
sole warning residue preesistono).
# MainDOB — FIX: cmdbuf oltre 64 KiB strippato in silenzio dall'IPC — finestre "morte" a contenuto congelato (build 51)

Sintomo: dopo un po' che si usa un programma, i suoi click smettono di avere effetto e
il contenuto non si aggiorna piu' (il resto del sistema e' sano; la finestra si
ridimensiona ma mostra il vecchio contenuto). Permanente finche' il programma vive.

Radice: un mismatch di tetti fra tre strati del trasporto Invalidate. Il cmdbuf client
cresce col contenuto della finestra (tetto client 1 MiB); sys_post lo snapshotta senza
limite; ma alla CONSEGNA ipc_copy_to_user_buf ha il tetto del buffer IPC del ricevente
(IPC_BUF_SIZE = 64 KiB) e sopra quella soglia azzera il payload IN SILENZIO. Il WM
riceve GUI_WIN_INVALIDATE senza payload -> "keep last_cmdbuf" -> rigioca per sempre il
vecchio frame. L'app riceve i click, ridisegna, flusha: ogni flush oltre soglia viene
strippato di nuovo. La soglia si attraversa proprio usando il programma (righe di
listview, testo dell'editor, celle di tabella): "funziona, poi si spegne". Lo stesso
percorso serviva i widget.

Fix — trasporto segmentato del cmdbuf (protocollo dobui, non tocca l'ABI IPC):

  1. libdob/include/dob/dobui_cmdbuf.h — DOBUI_CMDBUF_SEG_BYTES (32 KiB) e contratto
     dei segmenti: arg1=seq, arg2=segmenti totali (0/1 = monoblocco legacy, zero
     copie), arg3=byte totali; il segmento 0 inizia col magic.
  2. boot/dobinterface/DobInterface_stub.c — invalidate_flush spedisce a segmenti; il
     caso comune (frame piccolo) resta UN messaggio identico a prima.
  3. boot/dobinterface/main.c — blocco standardizzato "cmdbuf segmentato:
     riassemblaggio" (cmdbuf_reasm_t + cmdbuf_reasm_ingest) prima di window_t; stato
     per-finestra e per-widget, liberato alla distruzione. Buco di sequenza (segmento
     perso su porta piena) = parziale scartato, il frame successivo risana da solo.
     Gli handler WIN/WIDGET_INVALIDATE consumano solo cmdbuf COMPLETI.
  4. kernel/syscall/syscall.c — un payload strippato per superamento del buffer del
     ricevente ora viene nominato UNA volta per boot sulla seriale: e' un errore del
     mittente (deve segmentare) e perderlo in silenzio e' costato la diagnosi.

Rimosso di passaggio: blocco /* DIAG */ di log-spam residuo nel handler
GUI_WIDGET_INVALIDATE.

Verificato: sweep -fsyntax-only pulito su kernel (UP+SMP), compositor e stub (le sole
warning residue in main.c preesistono al cambio).
# MainDOB — Reimpianto ordinato dei fix della caccia al freeze, senza impalcatura diagnostica (kernel 1.1 build 50)

Base: trapiantato-13 (b36). I fix maturati nella caccia al freeze (b37..b49) sono stati
reimpiantati uno a uno rispettando la struttura degli sheet (blocchi standardizzati in
alto, logica ad alto livello in fondo, sezioni a bandiera), con commenti riscritti in
forma fattuale (razionali di progetto, non diario di caccia). TUTTA l'impalcatura
diagnostica e' rimasta fuori: niente stallwatch/[WDOG], niente dump di censimento
(scheduler/thread/porte/sync), niente contatori fire/done per linea IRQ, niente battito
del boomerang, niente contatore di drop IPC globale.

Fix reimpiantati (con il file che li ospita):

  1. time/units.h — ns_to_ms/ns_to_us in divisione vera (udiv64_u32). La
     moltiplicazione reciproca overflowava il prodotto: clock in ms modulo ~16384 ms.
  2. time/clock.{c,h} — clamp monotono globale SMP (cmpxchg8b): clock_now_ns non
     regredisce mai fra CPU con TSC sfasati.
  3. arch/x86/tsc.h — guardia delta-negativo nel reader ns-precision (TSC vCPU
     indietro rispetto all'ancora: risponde l'ancora, mai riancorare da li').
  4. arch/x86/tsc.c — riancoraggio a scrittore UNICO eletto (xchg trylock) + check di
     stantieta': chiuso il seqlock a scrittori multipli (ancora strappata -> tempo
     fermo -> kernel tickless muto: il freeze totale a seriale muta).
  5. arch/x86/lapic.c — politica timer: sempre one-shot sul clock di bus, mai
     TSC-deadline (con TSC locale fermo/sfasato la deadline non scatta mai).
  6. arch/x86/{isr.c,irq.c,irq.h,intr.c} — preemption onorata a OGNI uscita dal
     kernel (epilogo comune isr_dispatch + hook post-EOI su entrambi i dispatcher);
     absorb_spurious 7/15 disattivato sotto consegna IOAPIC (uccideva l'intera classe
     di vettori device 0x2x della CPU).
  7. time/event.c — floor da 250 ms per CPU idle, qualunque sia l'agenda: rete contro
     i kick di resched persi.
  8. proc/{thread,scheduler,process}.{c,h} — suite lifecycle: lock unico delle
     transizioni di vita/morte (thread_publish / thread_begin_death / reap_detach),
     reap sulla home-core (niente reaper-UAF cross-core), claim single-winner del
     finalize (niente PCB double-free), teardown instradato sulla home-core,
     scheduler_idle_has_work come cintura anti-starvation di idle_entry.
  9. syscall/video_boomerang.asm + syscall/driver.c + arch/x86/isr.c — flag BM_ACTIVE
     nello slot per-CPU; recupero su fault del dispatcher (rilascio del lock,
     ripristino CR3+frame del chiamante, rc=DV_ERR_RESET: chiamante vivo, mai piu'
     lock leakato + chiamante ucciso al posto del colpevole); spin sul lock LIMITATO
     con bail DV_ERR_BUSY (una CPU non puo' piu' essere murata a IF=0 da un lock
     irrecuperabile).
 10. boot/inputd/main.c — drain periodico di sicurezza da 500 ms (fronte edge perso
     su RTE mascherata: un byte incastrato nell'8042 vive al massimo mezzo secondo).
 11. drivers/ata/main.c — poll cooperativo in tempo reale (via gli spin da 12-60 s a
     prio 2 che mettevano in convoglio DobFileSystem).
 12. kernel/Makefile — dipendenze dagli header (-MMD -MP + -include): due TU non
     possono piu' disaccordare su una static inline dopo il cambio di un header.

Non portati (diagnostica pura): krt/stallwatch.{c,h} e la sua init in kernel.c;
scheduler_stall_dump; thread_mutex_waiters_dump; thread_blocked_census_dump;
ipc_port_stall_dump; ipc_sync_waiters_dump; irq_lines_dump + contatori fires/dones;
user_timer_pool_used; timer_heap_count/cap; battito g_bm_* del boomerang;
ipc_dropped_total (resta il report locale s_full_reports).

Verificato: sweep -fsyntax-only completo (UP e SMP) pulito. nasm non disponibile in
questo ambiente: l'asm del boomerang (BM_ACTIVE, .lock_bail) richiede la prima build
come verifica finale; etichette e liveness dei registri controllate a mano.
# MainDOB — RIMOZIONE: eliminato interamente il sottosistema ModuleLink (MLK) (build 146)

Rimozione completa dell'impianto ModuleLink (.mlk): feature mai decollata che nel tempo ha
accumulato problemi di sicurezza e affidabilita' (address space condiviso via trampolino con
switch di CR3, stack per-chiamante, parsing dell'header del file su input non fidato). Non
disattivata: cancellata, con recupero di spazio in sorgente e nelle immagini.

Kernel. Rimossi kernel/mlk/ (mlk.c, mlk.h) e la chiamata mlk_init() in kmain (le fasi di boot
successive sono rinumerate di conseguenza: 8=Event Groups, 9=Syscalls, ...). Da process_t
eliminati tutti i campi di stato del trampolino (mlk_saved_eip/esp/cr3, mlk_in_call,
mlk_trampoline_mapped), lo stato di trasferimento bidirezionale e la cache per-processo
mlk_stack_cache[], piu' la chiamata mlk_cleanup_process() nel teardown del processo. Rimossi i
cinque handler (sys_mlk_load/resolve/call/return/unload) e le relative registrazioni in
syscall_table.

Syscall. Liberati i numeri 70, 71, 72, 73 e 78 (SYS_MLK_LOAD/CALL/UNLOAD/RETURN/RESOLVE) lato
kernel e in libc (sys/syscall.h; rimossi i wrapper mlk_* in unistd.h). I numeri restano
volutamente non riassegnati per non spostare le altre syscall.

Companion (gli unici due consumatori). calculator: math_operations non e' piu' un .mlk caricato
a runtime ma e' compilato dentro calculator.mdl — le cinque funzioni intere (square, cube, sqrt,
cos_fp, sin_fp) sono riassorbite; rimossi caricamento/scaricamento del modulo, il badge di stato
"MLK" e mlk.ld. benchmark: rimosso il Test 8 "MLK round-trip" (companion ping inline da 64 byte)
e i suoi contatori; NUM_TESTS passa da 8 a 7.

Toolchain e immagini. Eliminato tools/mkmlk.c (packager .mlk host) e la sua regola in
tools/Makefile; rimossa la copia di math_operations.mlk da mklive.sh, mkbootdisk.sh, da
DobInstaller (categoria CAT_MLK) e da MainDOB_Setup. Documentazione (README, architecture, mem)
aggiornata: rimossi l'albero mlk/, la sezione "MLK Bubble Address Space" e la voce MLK dalle
tabelle.

---

# MainDOB — FIX: USB morto su hardware con IOAPIC (PCI INTx non instradato in modalita' APIC) (build 145)

Sintomo dal ferro: USB completamente inerte sull'Acer Extensa 5220 (chipset GL960 +
southbridge ICH8M) — inserisco un pendrive, nessuna icona, niente. Sul Compaq Armada E500
(440BX + PIIX4) e sui Compaq Pentium II-III lo stesso pendrive funziona: icona all'inserimento,
finestra di esplora file, scomparsa al disinserimento. Dopo il dup-fix dei companion UHCI
(build precedente) usbdiag sull'Extensa conferma che il registry NON e' piu' il problema: 5
controller registrati con nomi distinti (usb_uhci .. usb_uhci_4), init hardware OK, PCI
8086:2830, dispositivo FISICAMENTE presente su porta 1 (PORTSC=0x0083: CCS|CSC), USBSTS=0x0024
con Resume Detect acceso (il silicio ha rilevato l'evento), ELCR IRQ11 in level mode. MA:
"IRQ ricevuti = 0", FSM ferma "In attesa (Global Suspend)". Il diag stesso sentenzia:
"Dispositivo presente ma zero eventi: IRQ muto e FSM mai partita".

Diagnosi: il driver UHCI e' interrupt-driven (parcheggia il controller in Global Suspend e si
sveglia su un resume-IRQ invece di pollare PORTSC) e prende la sua IRQ dal registro PCI
Interrupt Line (=11). Ma kmain(), su ogni macchina con IOAPIC, chiama intr_switch_to_ioapic()
che maschera l'8259 e instrada SOLO le linee legacy 0-15 (acpi_resolve_gsi sulle ISA, con gli
override del MADT). In modalita' APIC il byte PCI Interrupt Line e' privo di significato (e' il
valore in modalita' 8259): l'INTx dell'UHCI dell'ICH8 e' instradato dal chipset a una GSI PCI
dell'IOAPIC — sulle southbridge ICH, PIRQA..PIRQH -> GSI 16..23 — che intr_switch_to_ioapic
NON programma. Quella redirection entry resta mascherata (ioapic_init maschera tutti gli input)
-> l'interrupt e' droppato -> il resume-IRQ non arriva mai -> la FSM non lascia Global Suspend
-> nessuna enumerazione, nessuna icona. L'ELCR che il driver setta con cura e' inutile: l'8259
e' mascherato. Causa radice: il kernel non risolve PCI INTx -> PIRQ -> GSI in APIC mode (niente
ACPI _PRT / interprete AML), e pirq.c sa instradare solo bridge PIIX (PIC mode).

Spiega l'A/B in modo netto: Armada E500 (PIIX4) e Pentium II-III sono macchine SENZA IOAPIC ->
ioapic_init() == false -> si resta in PIC mode -> il BIOS lascia PCI INTx -> PIRQ -> 8259
intatto e il PCI Interrupt Line valido -> IRQ 11 consegnata -> USB funziona. L'Extensa HA un
IOAPIC -> switch -> Interrupt Line morto -> USB muto. Il dup-fix era necessario (i 5 UHCI ora
vivono) ma non sufficiente: sopra c'era questo secondo strato di consegna interrupt.

Fix (sblocco, reversibile): nuovo flag di build IOAPIC_DELIVERY in config.mk (default 0),
propagato come -DMAINDOB_IOAPIC_DELIVERY via KERNEL_IOAPIC_FLAGS in kernel/Makefile. In
kmain() la chiamata intr_switch_to_ioapic() e' ora gated dietro #ifdef MAINDOB_IOAPIC_DELIVERY.
Default (flag off): l'8259 resta il controller degli IRQ dei dispositivi -> il PCI Interrupt
Line torna valido -> le interruzioni PCI (USB/AHCI/...) sono consegnate sulla linea legacy che
il driver legge, esattamente il path per cui i driver sono scritti (ELCR level-mode, Interrupt
Line). L'IOAPIC resta comunque scoperto, mappato e interamente mascherato da ioapic_init
(invariato): semplicemente non gli si cede la consegna.

Perche' non rompe nulla: (1) il timer LAPIC e' indipendente — e' consegnato localmente via il
proprio vettore LAPIC, mai attraverso PIC o IOAPIC -> il clock batte identico. (2) Gli IPI SMP
usano il LAPIC (lapic_send_ipi), non l'IOAPIC -> bring-up degli AP invariato. (3) intr_line_mask
/intr_line_unmask e intr_route_line_to_cpu degradano gia' al PIC quando g_ioapic_active==false
(verificato in intr.c) -> tutte le linee tornano a irq_mask/irq_unmask, il re-route per-CPU e'
no-op. (4) Nessun driver/servizio dipende dall'IOAPIC attivo: l'unico riferimento a
intr_switch_to_ioapic/g_ioapic_active e' interno a intr.c piu' la chiamata gated in kernel.c
(grep su kernel/ drivers/ boot/ libdob/). (5) Le macchine senza IOAPIC erano gia' su questo
esatto path -> per loro zero differenza. Il driver UHCI NON e' toccato.

NON e' la correzione definitiva per l'hardware moderno: per riabilitare l'IOAPIC serve
implementare il routing PCI INTx -> PIRQ -> GSI in APIC mode (lo swizzle device->PIRQ esiste
gia' in pirq.c; poi PIRQ -> GSI 16-23 secondo l'ICH8, infine ioapic_route_gsi + unmask di quella
GSI; l'infrastruttura regge, MAX_IRQ_FORWARD=32 copre 16-23). Con quello, build con
IOAPIC_DELIVERY=1 tornera' corretto e si riavranno i benefici IOAPIC (IRQ > 15, steering
per-CPU) anche su ICH-class.

Verificato: plumbing del flag controllato col preprocessore (cpp seleziona il ramo PIC di
default e lo switch con -DMAINDOB_IOAPIC_DELIVERY); braces del blocco gated bilanciate. NON
cross-buildato con i686-elf-gcc in questo ambiente (toolchain assente). Da validare sul ferro:
riprovare sull'Extensa 5220 — atteso comportamento identico all'Armada (icona pendrive
all'inserimento, scomparsa al disinserimento). Nota: la LEGSUP che usbdiag rilegge a 0x3500
(invece dello 0x2000 scritto) e' innocua per la consegna — sono bit di trap-status 60h/64h che
si riscattano per la normale attivita' della tastiera PS/2; USBPIRQDEN (0x2000) e' acceso, quindi
l'USB e' instradato su PIRQ e non su SMI.

---

# MainDOB — FIX REGRESSIONE: boot crash su hardware senza LAPIC (build 144)

Regressione introdotta dal lavoro SMP su hardware legacy mono-CPU privo di local APIC
(es. Compaq con PIIX3 / Pentium II-III). Sintomo dal ferro: KERNEL PANIC all'avvio subito
dopo "[SCHED] MPSC inbox self-test: OK" —

    Page Fault in kernel mode, CR2 = 0x00000020, not-present read.

Diagnosi: 0x20 e' l'offset del registro LAPIC ID (LAPIC_REG_ID). lapic_read(0x20) con il
LAPIC non mappato fa s_lapic_mmio[0x20/4] == *(uint32_t*)0x20 -> fault leggendo proprio
l'indirizzo 0x20. Catena: su questa macchina cpu_has(CPUF_APIC)==0, quindi tick_source_init()
NON chiama lapic_init() (s_lapic_mmio resta NULL) e usa il PIT ("LAPIC unavailable, falling
back"). Poi scheduler_init() esegue sched_resched_ipi_selftest(), gated solo su #ifdef
MAINDOB_SMP (compile-time) e NON sulla disponibilita' del LAPIC a runtime: chiama
lapic_get_id() -> lapic_read(LAPIC_REG_ID) -> NULL deref -> panic. In breve: il kernel SMP
assumeva sempre un LAPIC e non degradava su ferro che non ce l'ha.

Fix (4 punti, tutti gated sulla disponibilita' reale del LAPIC):
 1. arch/x86/lapic.c + lapic.h: nuovo predicato lapic_available() == (s_x2apic ||
    s_lapic_mmio != NULL). Unica fonte di verita' "il LAPIC e' toccabile adesso".
 2. proc/scheduler.c (sched_resched_ipi_selftest): salta il self-test se !lapic_available()
    -> niente lapic_get_id() sul NULL. E' la correzione diretta del crash.
 3. arch/x86/smp.c (smp_boot_aps): se !lapic_available() degrada a uniprocessore PRIMA di
    percpu_smp_init() (che legge l'APIC id del BSP) e dell'INIT-SIPI-SIPI. Difensivo: chiude
    lo stesso crash sul caso senza-LAPIC-ma-ACPI-riporta-piu'-CPU (sul Compaq mono-CPU era
    gia' coperto dalla guardia enabled<=1, ma cosi' e' robusto in generale).
 4. (gia' presente nel codice, confermato corretto) percpu_current() salta l'APIC finche'
    g_smp_active e' false -> il fallback di this_cpu() non tocca il LAPIC senza SMP attivo.

Perche' non rompe nulla: lapic_register_resched_callback() e lapic_resched_count() (chiamati
incondizionatamente a init) toccano solo variabili, niente MMIO -> verificato. Con il
self-test saltato il sistema parte single-core sul PIT; gli IPI di wake/shootdown restano
dormienti perche' solo il BSP e' mai online (wake_placement -> self, shootdown -> local-only).
Su hardware CON LAPIC lapic_available() e' true e tutto gira identico a prima.

Verificato: lapic.c / scheduler.c / smp.c 0 error / 0 warning in SMP={0,1} x {release,debug};
sweep completo kernel 56/56 in entrambe le config. Non buildato con i686-elf-gcc qui:
type-check con gcc -m32 sui flag reali. Da validare sul ferro: riprovare il boot sul Compaq —
atteso superamento del panic e avvio uniprocessore su PIT.

---

# MainDOB — SMP fix #A: scrittura ICR dell'IPI atomica contro le interruzioni (build 143)

Correttezza IPI (xAPIC). lapic_send_ipi(), in modo xAPIC, programma l'IPI con DUE scritture
MMIO separate: ICR_HI (destinazione) e ICR_LO (comando, la scrittura che FA partire l'IPI).
La coppia non era protetta: se un'interruzione si interpone tra HI e LO e il suo handler
manda a sua volta un IPI — un IRQ di dispositivo che risveglia un thread con home su un altro
core via scheduler_unblock, oppure l'handler del resched-IPI che inoltra un thread migrato —
sovrascrive ICR_HI e il NOSTRO IPI viene consegnato alla CPU SBAGLIATA.

Reale, non teorico: esistono mittenti che inviano con IF=1. sched_post_ctl() (-> kick di
resched, via thread_destroy/scheduler_request_destroy) non fa irq_save e gira con l'IF del
chiamante, raggiungibile da contesto syscall/processo; e il self-test di boot invia
esplicitamente con IF=1. La conseguenza di un IPI dirottato e' un resched/teardown perso o
recapitato al core sbagliato -> ancora hang/comportamenti erratici sotto SMP.

Fix (arch/x86/lapic.c, lapic_send_ipi, ramo xAPIC): cli/restore attorno a ICR_HI + ICR_LO +
poll del delivery-status. Tutto e tre dentro la regione protetta: HI non puo' essere
clobberato prima di LO; nessun writer interposto puo' toccare l'ICR mentre il nostro invio e'
pending (delivery-status alto); e lo status che leggiamo e' il nostro. NON un lock: ogni core
ha il proprio LAPIC/ICR, non c'e' nulla da bloccare ne' modo di deadlockare. Finestra = due
scritture MMIO + latenza di consegna hardware (microsecondi). NO-OP per i moltissimi
chiamanti gia' a IF=0 (path scheduler sotto irq_save, TLB shootdown sotto s_tlb_lock): popf
ripristina il loro IF=0. Solo i mittenti con IF=1 guadagnano la protezione.

x2APIC invariato: l'ICR e' un'unica wrmsr a 64 bit, gia' atomica, senza delivery-status da
pollare -> nessuna race, nessuna modifica.

Verificato: lapic.c 0 error / 0 warning in SMP={0,1} x {release,debug}; sweep completo kernel
56/56 in entrambe le config. Non buildato con i686-elf-gcc qui: type-check con gcc -m32 sui
flag reali. Da validare sul ferro: il fix e' difensivo e a impatto nullo sui path IF=0, quindi
non dovrebbe cambiare nulla di osservabile salvo eliminare IPI sporadicamente mal-recapitati.

---

# MainDOB — SMP fix: lost-wakeup nel context switch (race wait-queue ↔ unblock cross-core) (build 142)

BUG di concorrenza, candidato forte per le installazioni inaffidabili (install che non
avanza, hang intermittente a uno step della barra). Sintomo non-deterministico = firma di
race; questa e' nel cuore del risveglio IPC, su cui l'I/O storage (installer <-> driver <->
IRQ) fa affidamento a ogni settore.

Catena della race (solo SMP — in UP non esiste: un core, niente unblock cross-core, niente
IPI di resched, tutto sotto cli senza finestra):
 1. wait_queue_sleep() (proc/wait.c): marca current->state = THREAD_BLOCKED, accoda sulla
    wait-queue, RILASCIA wq->lock (IF torna a 1 sul core A), poi chiama scheduler_yield().
 2. Nella finestra IF=1 prima del cli di scheduler_yield(), il core B esegue
    wait_queue_wake_one() -> scheduler_unblock(t): t e' BLOCKED -> state = THREAD_READY, e
    poiche' t appartiene ad A lo posta nell'inbox di A + manda un resched IPI ad A.
 3. L'IPI viene consegnato ad A DENTRO quella finestra -> scheduler_on_resched_ipi() ->
    inbox_drain() ri-accoda t nella runqueue di A mentre t e' ANCORA current (state READY).
 4. scheduler_yield() prosegue: pick_next() ripesca t (lo rimuove dalla runqueue) e chiama
    do_switch(t) con t == current.
 5. do_switch() faceva `if (next == prev) return;` USCENDO prima di `next->state =
    THREAD_RUNNING`. -> t continua a girare con state == THREAD_READY.

Conseguenza: alla successiva preemption di slice, requeue_or_handoff_current() viene saltato
(requeue solo dei thread RUNNING) -> t viene tolto dalla CPU e NON rimesso in runqueue ->
thread perso per sempre -> hang. Intermittente perche' richiede che l'IPI cada nella finestra
stretta tra il rilascio del lock e il cli.

Fix (proc/scheduler.c, do_switch): sul ramo no-op (next == prev) si ripristina RUNNING, ma
SOLO se lo stato e' READY -- cosi' si annulla il transitorio READY introdotto dall'unblock
cross-core senza mai "resuscitare" un blocco/sleep/morte deliberati (BLOCKED/SLEEPING/DEAD
restano intatti). Chirurgico: tocca solo il caso next==prev (raro), gli altri chiamanti di
do_switch passano next!=prev (verificato: resched-ipi, slice_callback, unblock-preempt hanno
la guardia next!=current; yield e block_current sono le uniche vie al ramo no-op).

Nota sulla causa radice: la finestra IF=1 tra release(wq->lock) e cli in wait_queue_sleep e'
il vero buco; questo fix ne neutralizza l'esito dannoso in modo minimale. Una chiusura piu'
profonda (tenere IF=0 dal pre-release fino al yield) e' un cambiamento piu' invasivo,
rimandato di proposito per non introdurre regressioni nel path piu' caldo del kernel.

Verificato: scheduler.c 0 error / 0 warning in SMP={0,1} x {release,debug}; sweep completo
kernel 56/56 in entrambe le config SMP. Non buildato con i686-elf-gcc qui (assente):
type-check con gcc -m32 -ffreestanding sui flag reali del Makefile. Da validare sul ferro:
installazione ripetuta (il sintomo era intermittente) — atteso che non si pianti piu' a uno
step. Se rimangono hang, il prossimo sospetto e' la consegna dell'IRQ di completamento sotto
il nuovo routing IOAPIC/GSI (il CHANGELOG b140 segnala _PRT non parsato).

---

# MainDOB — SMP perf #1: diagnostico di piazzamento fuori dal release (build 141)

Primo intervento della campagna "SMP piu' lento del single-core": rimozione, dal path
caldo, di una stampa diagnostica non gated.

scheduler_assign_home_cpu() (proc/scheduler.c) e' chiamata a OGNI creazione di processo e
faceva tre kprintf non condizionati (intestazione + un kprintf per ogni CPU online che
riportava il carico + esito). kprintf prende vga_lock (lock GLOBALE) e scrive in MMIO VGA
(lento): su SMP ogni altro core che logga o piazza lavoro si serializzava su quel lock a ogni
piazzamento. In UP era solo lento; in SMP era un punto di serializzazione cross-core costante.

Fix: le stampe sono ora dietro #ifdef MAINDOB_DEBUG. In release (default orientato alle
prestazioni) spariscono del tutto -> niente vga_lock, niente MMIO, niente serializzazione tra
core sul piazzamento. In debug restano per diagnostica. NESSUN cambiamento alla decisione di
piazzamento: sched_core_load() e g_sched[].running_idle sono letture pure, il blocco era
puramente osservativo. sched_core_load() resta referenziata da sched_pick_home_cpu(), quindi
nessun warning "unused" in release.

Verificato: scheduler.c compila 0 error / 0 warning nelle 4 combinazioni SMP={0,1} x
{release,debug}; sweep completo del kernel 56/56 in SMP=1 e SMP=0. Non compilato con
i686-elf-gcc qui (assente) ma type-check completo con gcc -m32 -ffreestanding sui flag reali
del Makefile. Da validare sul ferro: stesso carico multi-processo prima/dopo, atteso calo del
tempo speso in vga_lock e migliore scaling alla creazione di processi.

Pulizia collaterale (stesso build): kernel/boot/bootfs.c ridefiniva KERNEL_VMA (0xC0000000u)
mentre includeva gia' arch/x86/paging.h che lo definisce identico senza suffisso 'u' ->
warning di ridefinizione (artefatto del merge SMP, che ha spostato la definizione canonica
nell'header di paging). Rimossa la ridefinizione locale; il valore arriva da paging.h. Kernel
ora a ZERO warning sotto -Wall -Wextra.

ATTENZIONE invariata dal merge b140: il kernel + i driver vanno comunque buildati con la
cross-toolchain reale per intercettare derive d'ABI; il type-check host non copre i 5 file
NASM, il link finale, ne' l'ABI i686-elf.

---

# MainDOB — MERGE three-way: kernel SMP/moderno + ramo storage/exFAT/TRIM (build 140)

Merge a tre vie. Base comune (nonno) = v1_5_37_copyintegrity (1.0.0.420.134). I due rami:
- ramo "storage" (questo lavoro): fix capacita' SATA LBA28/48, SMART su AHCI, SSD+TRIM su IDE,
  block_trim() + azione DobDisk, audit/doc exFAT. (1.0.0.420.139)
- ramo "SMP" = maindob_smp_patched_8 (1.0.0.435.134): mega aggiornamento kernel — IOAPIC +
  LAPIC + MADT/GSI, SMP (smp.c, ap_trampoline, percpu, tlb, fpu), futex, DMA kernel, e i
  fast_entry/main dei driver video aggiornati.

Esito del diff a tre vie: i rami sono quasi ortogonali. 4 soli file toccati da entrambi:
- kernel/kernel.h  -> tenuta la REVISION 435 dell'SMP; BUILD = max(139,134)+1 = 140.
- kernel/kernel.c  -> base SMP; riapplicata la rimozione di "popups" dalla lista moduli (il
  ramo storage ha rifattorizzato boot/popups/main.c in stub).
- Makefile         -> base SMP; riaggiunta la regola di build di DobPopup_toast_stub.c.
- kernel/syscall/syscall.c -> base SMP (i suoi 119 cambi: futex/smp/dma/percpu/tlb); riapplicato
  sopra il fix poweroff ACPI S5 del ramo storage (guardia pm1a_cnt; niente scrittura dei magic
  da emulatore nei port che collidono col PM1_CNT reale del PIIX4 dell'Armada E500 -> niente
  crash; park cli;hlt dopo SLP_EN). Le due regioni non si sovrapponevano: applicazione pulita.
Tutto il resto: kernel/driver-video/config dal ramo SMP; storage/exFAT/UI/programmi dal ramo
storage (78 file modificati + 8 aggiunti sovrapposti su base SMP, -boot/popups/main.c).

USB hotplug Extensa 5220 (ICH8): il motivo per cui questo merge serviva. Ora kmain fa
acpi_init -> pirq_init -> ioapic_init; su macchine con IOAPIC si usa IOAPIC + GSI risolta via
MADT. Hotplug prende l'IRQ dal PCI Interrupt Line (0x3C) e intr.c lo instrada sull'IOAPIC.
RISERVA: nessun parsing dell'_PRT ACPI ancora -> il kernel si fida del 0x3C impostato dal BIOS.
In APIC mode il BIOS dell'Extensa di norma ci mette la GSI corretta, quindi le companion UHCI
dovrebbero ricevere l'IRQ di resume e l'hotplug funzionare. Da VALIDARE sul ferro; se non
enumera ancora, manca solo il routing INTx->GSI via _PRT.

ATTENZIONE: non compilato qui (manca i686-elf-gcc). Il merge e' corretto strutturalmente
(diff a tre vie, conflitti risolti, bilanciamento simboli/graffe verificato), ma essendo un
merge di kernel + driver da rami diversi, DEVE essere buildato per intercettare eventuali
derive d'ABI tra il kernel/libc SMP e i driver sovrapposti del ramo storage.

---

# MainDOB — exFAT (.mem): audit del path per i dischi, live + residente

Controllo periodico ("meglio prevenire che curare") dell'integrazione exFAT, che per i dischi
passa per exfat.mem (PIC ELF shared object caricato via dob_mem_load). Verdetto: nessun bug
funzionale. Una sola correzione, di documentazione (sotto).

Verificato, situazione per situazione:
- Disponibilita' del .mem: exfat.mem viene messo in TUTTI i percorsi -- immagine live (mklive.sh),
  disco di boot da make (mkbootdisk.sh) e installazione (MainDOB_Setup). Warning esplicito se
  assente dal sorgente. Niente "cannot open exfat.mem" se la build lo produce.
- Caricamento cross-process: un'istanza secondaria (USB / partizione dati) carica exfat.mem con
  dobfs_Open(EXFAT_MEM_PATH). Il path inizia con /SYSTEM/, e lo stub DobFileSystem instrada
  /SYSTEM e /DATA al servizio FISSO "DobFileSystem" (il root), non all'istanza secondaria (che e'
  il disco dati e non ha /SYSTEM/.../exfat.mem). Quindi il .mem si carica sempre dal volume di
  sistema. Nessuna dipendenza circolare.
- Live vs residente: setup_live_mode() viene chiamato SOLO per il mount primario (root). Un mount
  secondario ha live_mode=false a prescindere da come ha bootato il sistema, quindi disk_read/
  write_sectors vanno sul disco FISICO via block layer (ata/ahci/usbms), non sul blob RAM. Un
  disco dati exFAT e' quindi leggibile/scrivibile anche in live; il root resta read-only (il gate
  e' in disk_write_sectors, e in live vale solo per il root). Le write exFAT passano per
  disk_write_sectors, quindi ereditano lo stesso gate.
- Offset partizione: applicato dentro disk_read/write_sectors (lba + fs.partition_lba); le
  callback rd/wr del .mem ricevono settori partition-relative da 512 byte, come da contratto.
- Robustezza mount(): valida firma "EXFAT   "@3, jump 0xEB, 0x55AA, bps_shift in [9..12],
  bps_shift+spc_shift <= 25 (no overflow della dimensione cluster), root_cluster >= 2, e propaga
  gli errori di I/O dallo scan della root dir. Un volume estraneo o corrotto viene rifiutato
  pulito (NULL), niente crash ne' accessi fuori range (i read sono comunque limitati dal bound
  del block layer).
- Settori 512/4096: il .mem traduce internamente BytesPerSectorShift in unita' da 512 byte (sia
  nel read/write sia in mkfs), quindi un volume exFAT con settore logico 4096 e' gestito qui, non
  dal chiamante.

Correzione (solo commenti): exfat_api.h, l'header di exfat.c e un commento in mkbootdisk.sh
dichiaravano la write/format come "Phase 2/3, stub -3" e il .mem come "read-only (Phase 1)". E'
falso e fuorviante: write set completo (write/ftrunc/create/mkdir/unlink/rename/flush) E mkfs
sono implementati ed esportati in __mem_exports. Commenti aggiornati a "full read-write + mkfs".

Osservazione (fuori scope, gia' nota): l'handler DOBFS_FORMAT (path .mem-via-IPC) resta
ATA-centrico nel ricavare la taglia; non e' usato dal path dei dischi/penne, che formatta via
exfat_ops.c con sector size esplicito. exfat_mkfs nel .mem, invece, e' gia' parametrico
(sectors + bytes_per_sector), non 512-hardcoded.

Nessuna modifica al codice eseguibile; build bumpato per policy (anche i commenti contano).

---

# MainDOB — TRIM reale end-to-end: block_trim() su ATA+AHCI + azione DobDisk

Il TRIM smette di essere parita' di sola superficie: ora e' invocabile e testabile.

Block layer (libdob block.c/.h):
- block_disk_t guadagna trim_supported (popolato da ata_enumerate e ahci_enumerate dai flag
  driver gia' esistenti).
- Nuovo vtable .trim e adapter ata_trim_op / ahci_trim_op:
  * ATA: arg0=lba, arg1=count, arg2=disk (il driver accetta solo lo slot 0, l'unico con DMA).
  * AHCI: arg0=port, arg1=lba, arg2=count (il driver valida porta SSD+TRIM).
- block_trim(i, lba, count): bound contro la capacita' (mai oltre la fine) e split in chunk da
  <=65536 settori (il limite di una entry DSM, 0=65536), una chiamata driver per chunk. Cablato
  in entrambe le classi (ata_class, ahci_class).

Driver ATA: l'handler ATA_OP_TRIM ora onora arg2=disk e rifiuta gli slot != 0 / senza DMA
(prima era inchiodato a slot 0 a prescindere -> avrebbe potuto TRIMmare il disco sbagliato se
chiamato con un selettore diverso). Gate invariato: present && is_ssd && trim_supported.

UI (programs/DobDisk): nuova azione "TRIM regione…" nel menu contestuale di una regione LIBERA
(SEL_FREE). Esegue il TRIM esattamente su quell'intervallo NON allocato [free_start_lba,
+free_sectors): scelta sicura (nessuna partizione coinvolta) e utile (prestazioni SSD). Gated
su SSD+trim_supported con popup d'errore chiaro altrimenti (es. SSD IDE non primary-master), e
conferma POPUP_YESNO prima di agire. Su successo aggiorna lo status; su fallimento popup
d'errore.

Sicurezza: l'azione parte solo su spazio libero, solo su SSD con TRIM, dietro conferma; il
block layer fa il bound sulla capacita'; i driver rifiutano media/slot non idonei. Catena di
controllo a piu' livelli, coerente col vincolo "tecnologia sotto consenso umano".

Da validare sul ferro (ora possibile): selezionare una regione libera su un SSD reale (IDE
primary-master o SATA) e lanciare "TRIM regione…"; verificare DOB_OK e l'assenza di errori
[ata]/[sata] nel log. Su HDD o disco senza TRIM l'azione dev'essere rifiutata col popup.

Impatto: i path read/write/identify/SMART non cambiano. Tutto bilanciato, sizeof(ata_disk_info_t)
invariato, simboli coerenti.

---

# MainDOB — IDE: backport rilevamento SSD + opcode TRIM (parita' con AHCI)

Pareggia l'ATA con l'AHCI sui due aspetti che mancavano lato IDE.

SSD (cablato e visibile fino a DobDisk):
- ata_disk_info_t guadagna is_ssd e trim_supported (riempiendo 2 dei byte di padding gia'
  presenti: la struct resta 64 byte, offset di total_sectors/model invariati -> wire
  driver<->block layer immutato).
- identify_extract_ssd_trim() legge word 217 (==1 -> non-rotating -> SSD) e word 169 bit 0
  (-> TRIM supportato); chiamato sia per il primary master sia per gli slot 1..3.
- ATA_OP_LIST_DISKS riporta i due flag; block.c (ata_enumerate) mappa is_ssd ->
  BLOCK_KIND_SSD. Ora un SSD IDE compare come SSD in DobDisk, come gia' su SATA.

TRIM (opcode + esecuzione, gated; parita' di superficie con l'op-4 AHCI):
- Nuovo ATA_OP_TRIM (23, arg0=lba arg1=count).
- ata_dsm_trim(): DATA SET MANAGEMENT (comando 0x06, Features=0x01) come trasferimento DMA-out
  di un blocco-range da 512 byte con una entry (LBA in [47:0], count in [63:48], 0=65536). La
  dma_xfer esistente era inchiodata a READ/WRITE DMA EX, quindi questa e' una via DMA-out
  dedicata. Richiede DMA attiva (dma_ok) -- niente DSM in PIO; gli SSD con TRIM girano in DMA.
  Solo primary master (slot 0), l'unico con path DMA in questo driver, esattamente come dma_xfer.
- Handler ATA_OP_TRIM con gate di sicurezza identico all'AHCI: rifiuta tutto cio' che non sia
  uno slot-0 presente, is_ssd E trim_supported. Non puo' quindi mai partire in modo distruttivo
  su un HDD o su un disco senza TRIM.

IMPORTANTE (vale anche per l'AHCI): il TRIM NON e' cablato nel block layer ne' in DobDisk --
non esiste block_trim() e nessuno invoca l'opcode. E' parita' di superficie: il giorno in cui
si aggiunge block_trim(), entrambi i driver sono pronti. Finche' non c'e' un chiamante, la via
DSM-out resta inerte e NON validata su hardware: va testata sul ferro (sull'SSD reale) solo
quando block_trim() viene introdotto.

Impatto: il path read/write/identify/SMART dell'ATA e' invariato; nessun tocco all'AHCI.
Tutto bilanciato, sizeof(ata_disk_info_t) invariato (64B), simboli coerenti.

---

# MainDOB — SATA: supporto SMART (AHCI), parita' con l'IDE per il pulsante "SMART…"

DobDisk ha gia' la voce di menu "SMART…" e chiama block_get_smart() per leggere i 512 byte di
dati SMART (con i nomi attributo che coprono anche gli SSD: wear leveling, erase/program fail,
power-loss protection, ...). Funzionava sui dischi IDE (ata_class espone .get_smart =
ata_smart_op, op 21) ma NON sui dischi SATA: ahci_class non aveva .get_smart, quindi
block_get_smart() ritornava false e il pannello falliva su AHCI. Questa patch chiude il buco.

Lato driver (drivers/ahci/main.c):
- Nuovo opcode AHCI_OP_GET_SMART (21, arg0 = porta, reply.payload = 512 byte SMART raw),
  parallelo a ATA_OP_GET_SMART.
- port_smart_read_data(): emette SMART READ DATA via AHCI -- comando 0xB0 con sub-comando 0xD0
  nel registro Features e i magic byte 0x4F/0xC2 in LBA mid/hi. issue_cmd carica il
  sub-comando nelle Features quando command == ATA_CMD_SMART (stesso schema del feature=0x01
  del TRIM); i magic byte arrivano impacchettati nell'argomento lba (ATA_SMART_MAGIC_LBA). Lo
  SMART e' opzionale: un disco che non lo supporta aborta (PxTFD.ERR) e issue_cmd ritorna
  false; l'ottico e' escluso (niente SMART).
- Handler AHCI_OP_GET_SMART: valida porta/presenza, rifiuta l'ottico, copia in reply_buf[p].

Lato libreria (libdob/src/block.c):
- ahci_smart_op() (gemello di ata_smart_op, arg0 = porta per l'AHCI) cablato in ahci_class
  come .get_smart. Nessun'altra modifica: DobDisk e block_get_smart() erano gia' table-driven.

Sugli SSD non c'era nulla da aggiungere lato AHCI: il driver gia' rileva l'SSD (word 217 ->
DEV_SSD -> BLOCK_KIND_SSD nel block layer) e gia' supporta il TRIM (AHCI_OP_TRIM op 4, comando
DATA_SET_MGMT). Semmai e' l'ATA a essere indietro: ata_disk_info_t non porta un flag SSD/TRIM
e ata_enumerate marca ogni disco come BLOCK_KIND_HDD, senza comando TRIM. Backport opzionale,
fuori da questa patch.

Impatto: additivo. Path read/write/identify AHCI invariati; nessun tocco al driver ATA.
Tutto bilanciato (graffe/parentesi), simboli coerenti.

---

# MainDOB — SATA: fix capacità IDENTIFY (LBA28/48) + niente dischi fantasma a capacità 0

Sintomo dal campo (Acer Extensa 5220, AHCI/SATA reale): l'editor partizioni rileva il
DISCO ma NON le partizioni e nemmeno lo spazio libero (elenco vuoto). In QEMU lo stesso
disco SATA mostra tutto; sull'Armada E500 (IDE puro, FAT32) le partizioni si vedono sempre.

Catena causale (tracciata layer per layer, build 134):
- drivers/ahci/main.c `port_setup`: porta su la porta, vede DET==3 + firma SIG_ATA, marca il
  disco present e type=DEV_HDD PRIMA di leggere l'IDENTIFY -> il disco compare comunque.
- `port_identify` ricavava la capacità SOLO dai word 100-103 (LBA48), senza fallback ai
  word 60-61 (LBA28), ed era void: un IDENTIFY fallito o un disco che riporta la taglia solo
  in 60-61 lasciava sector_count = 0.
- `AHCI_OP_LIST_PORTS` riporta la porta come HDD anche con sector_count == 0.
- libdob/src/block.c: total_sectors = 0, e block_read fa `if (lba + count > total_sectors)
  return false` -> OGNI lettura, LBA 0 compresa, fallisce.
- programs/DobDisk `load_current_mbr`: `if (!block_read(idx,0,1,sec)) return` -> niente MBR ->
  zero partizioni E zero spazio libero. Entrambi i sintomi collassano su total_sectors == 0.

Perche' QEMU/Armada andavano: QEMU presenta un disco con LBA48 (word 100-103 popolati);
l'Armada usa il driver ATA, che gia' faceva la cosa giusta (identify_extract_total_sectors
sceglie LBA48 oppure LBA28 secondo il word 83 bit 10). L'AHCI era rimasto indietro.
Non era un hardcoding: init_hardware scandisce correttamente il registro PI (Ports
Implemented), non la porta 0.

Fix (drivers/ahci/main.c, solo `port_identify` + il suo call site in `port_setup`):
1. Parse capacita' robusto, identico alla logica provata dell'ATA: LBA48 (word 100-103) se la
   feature 48-bit e' annunciata (word 83 bit 10) E il valore e' non-zero, altrimenti LBA28
   (word 60-61).
2. Retry dell'IDENTIFY (4 tentativi, backoff 150 ms): un disco appena uscito da COMRESET puo'
   rispondere not-ready/ABRT al primo comando su silicio reale (QEMU risponde istantaneo).
3. `port_identify` ora ritorna bool. Per un HDD/SSD, capacita' 0 o IDENTIFY fallito ->
   la porta NON viene annunciata come block device (present=false, port_setup ritorna false):
   meglio un "nessun disco" pulito che un disco fantasma a 0 settori che l'utility mostra ma
   non puo' leggere ne' scrivere in sicurezza (block_write si fida di total_sectors). Il path
   "porta vuota" del chiamante ri-arma comunque l'hotplug-listen per un futuro re-plug.
4. Diagnostica in bring-up (lba48/cap48/cap28 -> sectors, per tentativo; e riga esplicita di
   fallimento) per leggere sul ferro vero esattamente cosa decide la porta.

Impatto: nullo su QEMU (word 100-103 restano validi) e nullo sul path ATA dell'Armada
(immutato). Le partizioni AHCI compilano, graffe/parentesi bilanciate.

Da validare su hardware (Extensa 5220, AHCI): le righe "[sata] port N IDENTIFY ok: ..." nel log
diagnostico devono mostrare sectors > 0 (e se cap28 e' la sorgente, la fix e' confermata).
Fuori scope, traccia separata: in modalita' IDE-compatibile l'Extensa non rileva NESSUN disco
(il driver ATA parte ma l'IDENTIFY sui suoi slot legacy non risponde) -- diagnosi da fare con
atadiag sul ferro prima di toccare il probe IDE, per non regredire l'Armada.

---

# MainDOB — USB: audit dell'intera pipeline + fix ordering exFAT superfloppy

Rilettura completa della pipeline USB ("meglio prevenire che curare"). Verdetto: nessun bug di
correttezza nella catena dati/format. Una sola correzione applicata (sotto). Tutti i componenti
toccati compilano (0 errori), 0 simboli libgcc a 64 bit.

Catena verificata layer per layer:
- usb_mass_storage: transport (hc_ctrl/hc_bulk, bounds ok), Bulk-Only Transport (bot_xfer: retry NAK
  del CBW all-or-nothing dal pacchetto 0, fallimento data-phase -> reset-recovery che azzera i
  toggle, dCSWTag verificato, phase-error -> recovery), comandi SCSI (READ/WRITE(10), READ
  CAPACITY(10), SYNC CACHE; CDB big-endian corretti), e soprattutto la mappatura LBA512<->nativo in
  vread/vwrite: corretta per 512/2048/4096, gli unaligned leggono il blocco nativo coprente e copiano
  la porzione giusta, vwrite fa read-modify-write quando non allineato (preserva i byte circostanti) e
  quando allineato span_sect*512 == nb*native_block esatto (nessun byte stantio); nb*native_block <=
  BOT_DATA_MAX sempre (nessun overflow del bounce). Op handler con bound corretti (count <=
  CLIENT_MAX_SECT, WRITE valida payload_size). Detach: posta DOBFS_SHUTDOWN a dobfs_N poi _exit;
  perdita di un write in-flight su rimozione a sorpresa inevitabile (fisica), dati chiusi al sicuro
  (flush su close).
- block.c (classe usbms): read/write troncano lba a uint32 (soffitto 2TB documentato), chunk 32 <=
  128 (nessun INVALID), enumerazione e flush op-4 corretti.
- exfat_ops (formattatore usato per le penne): layout sector-size-aware, checksum di boot che coprono
  i settori 0-10 (sector 11 = checksum, backup 12-23 specchiato), FAT (catena bitmap, upcase/root
  EOC), bitmap, upcase, root dir; il check root_cluster < bps/4 garantisce che le entry FAT stiano nel
  settore logico 0; zero_region corretto.
- DobFileSystem: disk_read/write aggiungono l'offset di partizione e, per usbms, usano il layout
  ATA-style (arg0=lba) perche' find_ata_driver mette use_ahci=true SOLO se il provider inizia per "ah"
  -- per "usbms_N" resta false, che e' esattamente il layout che usbms op 1/2 si aspetta. Binding del
  provider e parsing di provider=usbms_N (strncpy in buffer da 32, niente troncamento) corretti.
  disk_flush gated su mount_secondary: no-op sul root di boot, attivo (op-4) sui mount USB.
- DobDisk: creazione partizione allineata a 2048 (4KB-aligned per la prima partizione), format via
  exfat_ops + flush, default del sector size al nativo.
- usb_uhci (controller di default): bulk transfer corretto -- toggle avanza per pacchetto eseguito e
  viene persistito per EP, split multi-run rispetta XFER_MAX_TD, short-packet IN termina il transfer,
  NAK mid-data -> -4 -> reset-recovery (niente corruzione silenziosa). UHCI_XFER_MAX=8192 >
  BOT_DATA_MAX=4096 (bounce abbondante).
- DAS "Pendrive USB": doppio-click usa op 66 (monta un volume) e su penna vuota dice "usa DobDisk";
  la voce "Formatta..." usa op 67 (solo bring-up) quindi una penna VUOTA/senza filesystem si puo'
  formattare.

Fix applicato (drivers/usb_mass_storage/main.c, find_volume_lba): il controllo dell'exFAT superfloppy
(volume exFAT a LBA 0, senza MBR) stava DOPO il parsing MBR. Una penna exFAT superfloppy ha comunque
0xAA55 a 510, quindi partition_mbr_parse la trattava come MBR e scansionava i byte del boot-code exFAT
come "partizioni"; una entry-spazzatura con sectors != 0 e un type byte FAT/exFAT avrebbe prodotto un
LBA fasullo e il mount sarebbe fallito. La firma "EXFAT   " a offset 3 e' univoca dell'exFAT (mai in un
MBR reale), quindi ora si testa PRIMA del parsing MBR: nessun falso positivo sui dischi partizionati
(per loro la firma e' assente -> si passa al loop MBR come prima), e gli exFAT superfloppy montano
correttamente a LBA 0.

Osservazioni minori (non bug nel path USB attuale, segnalate per completezza):
- Soffitto 2TB: READ CAPACITY(10) ritorna 0xFFFFFFFF per dischi > 2TB -> native_count=0 -> il device
  appare vuoto (rifiutato in sicurezza dai bound, non corrotto). Penne > 2TB non esistono in pratica.
- exfat_format / op DOBFS_FORMAT (il path .mem) e' ATA-centrico (dimensione via ATA IDENTIFY, 512
  hardcoded): NON usato per le penne (DobDisk usa exfat_ops), ma fallirebbe se qualcuno instradasse un
  format USB tramite DOBFS_FORMAT. Incoerenza latente, fuori scope; sistemabile in futuro.
- Allineamento partizioni: la prima parte a 2048 (allineata); partizioni multiple successive ereditano
  l'allineamento della fine della precedente -- solo questione di performance su penne 4Kn
  multi-partizione, non correttezza.
- SYNCHRONIZE CACHE per-operazione sulle scritture exFAT USB (close/mkdir/unlink/rename) rallenta la
  creazione massiva di file: trade-off accettato (durabilita'), identico al path FAT32.

Da verificare su hardware/VM (non bug di codice, punti di validazione): il round-trip format exfat_ops
-> mount exfat.c (ordine entry root dir, checksum); che usb_ehci/usb_xhci onorino lo stesso contratto
(op 3/4/7/8/9 + bulk) di usb_uhci; il comportamento su rimozione a sorpresa.

---

# MainDOB — USB: formattazione exFAT (flush di persistenza + sector size nativo)

Obiettivo: formattare penne USB in exFAT, con cura per la taglia dei settori personalizzabile.
Indagando si scopre che quasi tutta la feature c'era gia':

- DobDisk (programs/DobDisk) formatta gia' in exFAT: la dialog ha un dropdown filesystem (FAT32 /
  exFAT), un dropdown **sector size selezionabile dall'utente (512 / 4096)**, cluster e numero di
  FAT; sceglie exfat_ops o fat32_ops e scrive via block layer; imposta il tipo MBR a exFAT (0x07).
  Il commento "Solo FAT32 supportato" in cima al file era vecchio.
- libdob/src/exfat_ops.c e' un formattatore exFAT completo "mkfs.exfat over the block layer",
  sector-size-aware (512 e 4096), con mkfs_options_t.bytes_per_sector.
- Il block layer (libdob/src/block.c) scopre gia' le penne (classe usbms in driver_classes[],
  usbms_enumerate via op 3 capacity) e DobDisk le vede come BLOCK_BUS_USB.
- L'icona "Pendrive USB" ha gia' la voce "Formatta..." che fa il bring-up della penna e lancia
  DobDisk --select usbms_<token>.

Il gap vero era UNO: **nessun flush a livello block**. Dopo che DobDisk scrive le strutture exFAT e
l'MBR via block_write -> usbms (cache di scrittura del device), niente le committava con l'op-4
SYNCHRONIZE CACHE -> il format restava in cache e si perdeva estraendo la penna.

Fix:
1. Flush del block layer (libdob/src/block.c, block.h). Aggiunto un metodo opzionale .flush al
   block_driver_class_t (stesso schema di .get_smart): usbms lo implementa con l'op-4 (usbms_flush_op
   -> SYNCHRONIZE CACHE), ata/ahci lo lasciano NULL (no-op: non hanno una cache da committare cosi'
   e comunque ignorano l'op-4). Esposta la funzione pubblica block_flush(i). DobDisk la chiama dopo
   ogni mkfs riuscito (fmt_dialog_commit) e dopo ogni scrittura dell'MBR (commit_current_mbr), quindi
   ora il format (strutture + tipo partizione) viene committato sulla penna.
2. Sector size nativo (la "attenzione alla taglia settori"). usbms_enumerate gia' leggeva il blocco
   nativo dalla capacity (reply.arg1); ora lo si espone in block_disk_t.native_sector_size (512 o
   4096; default 512 per ata/ahci). La dialog di DobDisk preseleziona il sector size nativo quando si
   passa a exFAT (fmt_dialog_open + fmt_apply_constraints), cosi' una penna 4Kn si formatta allineata
   (niente read-modify-write ne' write amplification). L'utente puo' comunque scegliere 512.

Atteso: icona "Pendrive USB" -> "Formatta..." -> DobDisk -> filesystem exFAT, sector size (default =
nativo), cluster, etichetta -> la penna si formatta in exFAT, il tipo MBR diventa exFAT, e il format
sopravvive all'estrazione (SYNCHRONIZE CACHE). Poi si rimonta e si legge/scrive (path della sessione
precedente).

Verifica: block.c e DobDisk compilano coi flag reali (0 errori), 0 simboli libgcc a 64 bit. Da
provare in QEMU con un device usb-storage che punta a un'immagine vuota/con vecchio fs, oppure su
hardware reale con snapshot: il format puo' corrompere il supporto, usare media di scarto.

---

# MainDOB — USB: lettura e scrittura penne exFAT

Obiettivo: montare, leggere e scrivere penne USB formattate exFAT. L'infrastruttura USB c'era gia'
quasi tutta -- host controller (UHCI/EHCI/xHCI), usb_common, e un driver mass-storage (BOT/SCSI) che
registra usbms_<N>, parla il block-protocol come AHCI, gestisce le penne 4K-native (mappa LBA512 ->
blocchi nativi, read-modify-write) e implementa READ(10)/WRITE(10)/SYNCHRONIZE CACHE. Mancavano due
cose.

1. Rilevamento exFAT (drivers/usb_mass_storage/main.c). find_volume_lba trovava solo FAT32
   (partition_type_is_fat32) e su una penna exFAT tornava "nessun volume" -> il click non montava
   nulla. Ora accetta anche le partizioni exFAT (MBR type 0x07, partition_type_is_exfat) e il caso
   superfloppy (firma "EXFAT   " a offset 3 senza MBR), e riporta il tipo fs. op_prepare_volume passa
   fs=exfat a DobFileSystem (informativo: il mount comunque auto-rileva la firma exFAT nel boot
   sector e instrada a exfat.mem). Tutto il resto del flusso (icona "Pendrive USB", spawn del mount
   secondario legato a usbms_<N>, vista DobFiles) e' identico al FAT32 ed era gia' provider-agnostic.

2. Persistenza in scrittura su rimozione (boot/DobFileSystem/main.c). handle_close per l'exFAT
   committava i metadati (ex.api->flush) ma NON chiamava disk_flush() (op-4): su AHCI e' irrilevante
   (write-through), ma su penna la cache del device non veniva svuotata -> scrivi, estrai, perdi i
   dati. Aggiunto disk_flush() (= SYNCHRONIZE CACHE sul provider) dopo ogni operazione exFAT che
   cambia metadati: chiusura file (solo se il file e' dirty, per non flushare le letture), mkdir,
   unlink, rename, e i file appena creati ("touch") vengono marcati dirty cosi' la chiusura li
   committa. disk_flush() gira solo sui mount secondari e manda l'op-4 al provider legato (usbms_<N>
   per la penna), quindi e' no-op sulla root di boot e innocuo sui mount AHCI secondari.

Atteso: doppio-click sull'icona "Pendrive USB" di una penna exFAT -> si monta, si naviga in DobFiles,
si leggono e scrivono file; estraendo la penna i dati restano (SYNCHRONIZE CACHE alla chiusura).

Verifica: usb_mass_storage e DobFileSystem compilano coi flag reali (0 errori; i 4 warning
sign-compare di usbms sono pre-esistenti in hc_bulk, non toccati); DobFileSystem 0 simboli libgcc a
64 bit. Da provare in QEMU con un device usb-storage che punta a un'immagine formattata exFAT
sull'host, oppure su hardware reale con snapshot.

---

# MainDOB — fix schermo nero: phase2_init ora rispetta needs: (dobinterface aspetta "video")

Progresso: phase2_init legge la Startup_modules dall'exFAT e avvia tutti gli 11 moduli. Ma
dobinterface (il desktop) muore subito: "FATAL: dv_vproc_attach failed (rc=-4)" -> schermo nero. Nei
log il driver video (bga) viene avviato da hotplug MOLTO dopo (PID 16, dopo lo scan PCI), mentre
dobinterface (PID 11) ci prova subito e fallisce.

Causa: phase2_init avviava i moduli in ordine IGNORANDO le clausole needs:. dobinterface dichiara
"driver primary needs:video": al boot normale il kernel lo parcheggia finche' il servizio "video" non
si registra (bga fa dob_registry_register("video", port)). phase2_init non lo faceva.

Fix (programs/phase2_init/main.c): per ogni modulo si estrae "needs:NAME" dai flag e, prima dello
spawn, si chiama dob_registry_wait(NAME, 8000) — esattamente il parcheggio che fa il kernel. Cosi'
dobinterface aspetta che "video" sia registrato prima di partire. hotplug e' piu' in alto nella lista
(quindi gia' in esecuzione) e, mentre phase2_init e' bloccato sull'attesa, scansiona il PCI e avvia
bga, che registra "video" e sblocca l'attesa. Timeout di 8s per non bloccare all'infinito se una
dipendenza non arrivasse.

Atteso al prossimo boot: una riga "[phase2_init] dobinterface needs:video -- waiting", poi (dopo che
bga registra video) "[phase2_init] spawn dobinterface drv=1 pid=.." SENZA il FATAL, e il desktop che
finalmente compare.

Verifica: phase2_init compila pulito coi flag reali; 0 simboli libgcc a 64 bit a -O0/-O2/-Os. La
riga di dobinterface nella Startup_modules generata dall'installer e' confermata "driver primary
needs:video".

---

# MainDOB — FIX (causa certa): il sandbox bloccava phase2_init sull'area /SYSTEM/CONFIG

La diagnostica ha chiuso il caso: la struttura e' TUTTA sull'exFAT (la lista mostra
/SYSTEM/CONFIG/Startup_modules, 1361 byte), ma dobfs_Open falliva mentre dobfs_List riusciva. La
differenza: handle_list NON chiama sandbox_check (la navigazione e' libera), handle_open SI'. E in
sandbox_check /SYSTEM/CONFIG/ e' area RISERVATA: l'apertura richiede config_area_allowed(pid), una
whitelist per nome processo che NON includeva phase2_init. In piu', leggere/spawnare i moduli da
/SYSTEM/OperatingSystem/ richiede il bypass driver, e phase2_init non era un driver.

Due correzioni mirate:
- boot/DobFileSystem (config_area_allowed): aggiunto phase2_init alla whitelist dell'area
  /SYSTEM/CONFIG, cosi' puo' leggere Startup_modules dopo il pivot.
- Installer (BOOTSTUB): la riga di phase2_init nella Startup_modules minimale ora ha il flag
  "driver" (oltre a needs:DobFileSystem). Cosi' il kernel lo promuove a driver e bypassa i controlli
  di accesso file per /SYSTEM/OperatingSystem/ (lettura dei .mdl da avviare) e /SYSTEM/DRIVERS/.

Con questo phase2_init dovrebbe finalmente: leggere la Startup_modules dall'exFAT, e avviare i moduli
di fase-2 (hotplug, GUI, ecc.) caricandoli dall'exFAT, con make_driver sui driver. La GUI dovrebbe
comparire.

NB: serve ricompilare e RI-installare (la fix di DobFileSystem va sullo stub FAT32, e la riga
Startup_modules con "driver" la scrive l'installer). I log di debug di phase2_init restano: al
prossimo boot dovrebbero mostrare "Startup_modules: 1361 bytes" e una riga "spawn ... pid=..." per
ogni modulo.

Verifica: DobFileSystem e installer compilano puliti coi flag reali; DobFileSystem 0 simboli libgcc
a 64 bit.

---

# MainDOB — diagnostica: perche' la root exFAT non ha /SYSTEM/CONFIG/Startup_modules

Il test ha mostrato: il pivot riesce, ma phase2_init non riesce ad aprire
/SYSTEM/CONFIG/Startup_modules sulla root exFAT. L'installer aveva riportato successo nello scrivere
sull'exFAT (CONFIG e' andato a buon fine, BOOTSTUB pure), quindi la scrittura "e' riuscita" nel mount
secondario ma il file non e' leggibile al boot. Per inspezione exfat.c scrive le voci di directory su
disco (write-through, stessa disk_write_sectors del FAT32 che persiste), quindi non si vede un bug
ovvio: serve vedere cosa c'e' davvero sull'exFAT. Questo commit aggiunge SOLO diagnostica (+ una
piccola correzione), non il fix.

- phase2_init: se non riesce ad aprire Startup_modules, ora elenca (dobfs_List) e stampa il
  contenuto di "/", "/SYSTEM" e "/SYSTEM/CONFIG" sulla root exFAT. Cosi' al boot si vede fino a che
  punto la struttura scritta dall'installer e' effettivamente su disco.
- Installer (FINALIZE): in modalita' split, ri-legge in-sessione dall'exFAT (dobfs_StatOn su
  dobfs_9998) /SYSTEM, /SYSTEM/CONFIG e /SYSTEM/CONFIG/Startup_modules e logga l'esito + la
  dimensione. Se qui e' leggibile ma al boot no -> persistenza tra-mount; se non e' leggibile
  nemmeno qui -> la scrittura non ha funzionato davvero.
- Correzione: /DATA in modalita' split ora viene creato sull'exFAT (volume dati), non sulla FAT32
  stub (prima FINALIZE lo creava sul target di default = FAT32).

Verifica: phase2_init e installer compilano puliti coi flag reali; graffe bilanciate. Per il prossimo
giro servono DUE log: quello dell'installazione (riga "exFAT readback: ...") e quello del boot (righe
"[phase2_init] list ..."). Da li' si capisce se il problema e' nella scrittura o nella persistenza.

---

# MainDOB — fix phase2_init: promozione driver + log (pivot OK, ma GUI assente)

Il test in VM ha confermato che il pivot funziona ("root pivoted to exFAT volume" + Ready), ma il
sistema restava fermo al framebuffer BGA: nessuna GUI. Causa probabile: phase2_init avviava TUTTI i
moduli con spawn_file_sync, senza promuovere a driver quelli che lo richiedono. I moduli con flag
"driver" (driver video, dobinterface, hotplug, ...) hanno bisogno di make_driver() per accedere a
hardware/porte — esattamente cio' che fa il kernel per i driver di boot. Senza, il driver video e la
GUI partono senza privilegi e il desktop non compare mai.

- phase2_init ora, per ogni riga della Startup_modules, rileva il flag "driver" e, dopo uno spawn
  SINCRONO (spawn_file_sync, cosi' il one-shot non esce prima che lo spawn sia completato), chiama
  make_driver(pid) sui moduli driver. I non-driver restano spawn semplici.
- Aggiunto output di debug (debug_print) in ogni fase: attesa pivot, apertura/byte della
  Startup_modules su exFAT, e per ogni modulo "spawn <nome> drv=<0/1> pid=<pid>", piu' il conteggio
  finale. Cosi' il prossimo boot mostra esattamente dove eventualmente si ferma.
- Timeout attesa pivot portato da 5 a 10 s.

Verifica: compila pulito coi flag reali; 0 simboli libgcc a 64 bit (-O0/-Os). Da riprovare in VM:
servono i nuovi log "[phase2_init] ..." per confermare il fix o individuare il prossimo punto.

---

# MainDOB — installer: split FAT32-stub / root-exFAT (chiude "tutto in FAT32")

Il pezzo che mette davvero il sistema sull'exFAT. Quando l'utente sceglie l'exFAT come volume
definitivo (casella in "Selezione disco"), l'installer ora installa il grosso del sistema
SULL'exFAT e lascia sulla FAT32 solo lo stub di boot. Senza la scelta, installazione FAT32 classica
identica a prima (zero regressioni).

Infrastruttura:
- Funzioni di scrittura (install_copy_file/_write_string/_mkdir_p_target/_copy_optional/
  _write_visible) ora scrivono su g_target_service invece del fisso dobfs_9999.
- Nuovo target dobfs_9998 = mount secondario sulla partizione exFAT di root. In VERIFY, in modalita'
  split, viene montato (spawn DobFileSystem --mount lba=<exfat> id=9998 fs=exfat) e verificato.
  g_split = (exFAT trovata && casella spuntata).
- install_tick instrada per fase: OS/DRIVERS/PROGRAMS/CONFIG -> exFAT in split (FAT32 in classico);
  GRUB/KERNEL/BOOTSTUB/FINALIZE -> sempre FAT32.

Nuova fase INST_PHASE_BOOTSTUB (solo split): scrive sulla FAT32 lo stub di fase-1 — driver disco di
boot (ata/ahci), DobFileSystem(+manifest), exfat.mem, modules, phase2_init — una Startup_modules
MINIMALE di fase-1, e il marcatore /SYSTEM/CONFIG/Root_volume con l'indice di partizione exFAT.
Le fasi OS/DRIVERS/PROGRAMS/CONFIG hanno gia' scritto il sistema completo (con la Startup_modules
COMPLETA) sull'exFAT.

Rimosso il vecchio modello additivo (Definitive_volume + riga exfat_attach in Startup_modules):
era l'approccio sbagliato, sostituito dal pivot + phase2_init.

Sequenza di boot risultante: GRUB+kernel da FAT32; il kernel carica la fase-1 dalla FAT32 (driver,
DobFileSystem, modules, phase2_init) e spegne bootfs; DobFileSystem legge Root_volume, monta exfat.mem
dalla FAT32 e fa il pivot della root su exFAT; phase2_init attende il pivot e avvia il resto dalla
Startup_modules dell'exFAT. Root e contenuti finiscono sull'exFAT.

Verifica: compila pulito coi flag reali; 0 simboli libgcc a 64 bit (-O0/-Os); graffe bilanciate.
AVVISO IMPORTANTE: l'intera catena (fase-1 -> pivot -> fase-2 -> sistema su exFAT) e' verificata SOLO
in compilazione. Niente di tutto cio' e' provato su hardware; un errore qui = sistema non avviabile.
Provare in VM con snapshot ripristinabile. Limiti noti da tenere a mente: phase2_init avvia i moduli
nell'ordine della lista, senza onorare le clausole needs: (i servizi devono tollerare l'ordine
attendendo le proprie dipendenze nel registry); il timeout di attesa del pivot in phase2_init e' 5 s.

Resta il pezzo (2): impostazione di visibilita' della FAT32-stub sul desktop, sul modello di
show_system_partition.

---

# MainDOB — root su exFAT: marcatore a indice + caricatore di fase-2

Avanzamento del modello root-su-exFAT. Due pezzi, entrambi verificati in compilazione; come il
pivot, sono inerti finche' l'installer (prossimo pezzo) non dispone lo split sui due volumi.

DobFileSystem — marcatore robusto:
- /SYSTEM/CONFIG/Root_volume ora contiene l'INDICE di partizione (0..3) dell'exFAT di root, non
  l'LBA. A ogni boot DobFileSystem rilegge l'MBR, prende la voce a quell'indice, verifica tipo
  exFAT + firma "EXFAT   " al boot-sector, e ne ricava l'LBA fresco. Sopravvive a ripartizionamenti
  (finche' l'exFAT resta nello stesso slot MBR) e, se lo slot non e' piu' un exFAT valido, niente
  pivot (si resta su FAT32). Nuova funzione resolve_root_volume_lba (sostituisce la versione a LBA).

Nuovo modulo programs/phase2_init — caricatore di fase-2:
- Il kernel carica i moduli SOLO dalla FAT32 (lettore bootfs interno) e poi lo spegne. Nel modello
  a stub, sulla FAT32 stanno solo i moduli di fase-1 (driver disco, DobFileSystem, modules, e
  questo loader); tutto il resto (hotplug, GUI, settings, input, programmi) sta sull'exFAT.
- phase2_init e' un one-shot (needs:DobFileSystem). Attende che il pivot sia completato (il mount
  root riporta fs_type == "exfat" via dobfs_GetMountedOn), poi legge /SYSTEM/CONFIG/Startup_modules
  — che ora risolve sull'exFAT — e avvia ogni modulo non di fase-1 con spawn_file_sync, che legge il
  .mdl via DobFileSystem, cioe' DALL'exFAT. Questo e' possibile perche' spawn_file_sync legge dalla
  root corrente; DobFileSystem stesso NON puo' farlo (IPC verso se' -> deadlock), per questo la
  fase-2 e' un processo separato.
- Su un'installazione FAT32 classica non c'e' pivot: l'attesa scade, phase2_init esce senza fare
  nulla. Zero regressioni. La lista phase1_names in phase2_init va tenuta allineata ai moduli che
  l'installer mette sulla FAT32.
- Registrato nel programs/Makefile (PROGRAMS, EPS_phase2_init := DobFileSystem, NO_DOBUI).

Verifica: DobFileSystem e phase2_init compilano puliti coi flag reali; 0 simboli libgcc a 64 bit
(-O0/-O2/-Os); graffe bilanciate. La sequenza di boot in due fasi va provata su hardware reale.

Prossimi pezzi: (1) installer che dispone stub-FAT32 (fase-1 + exfat.mem + marcatore +
Startup_modules minimale che elenca phase2_init) e root-exFAT (tutto il resto + Startup_modules
completa); (2) impostazione di visibilita' della FAT32-stub, sul modello di show_system_partition.

---

# MainDOB — root su exFAT: pivot in DobFileSystem (primo pezzo)

Inizio del modello corretto: FAT32 = stub minimo di boot, poi la radice passa all'exFAT.
Questo commit fa SOLO il pezzo in DobFileSystem (il pivot). Da solo non cambia nulla: serve
l'installer (pezzo successivo) che scriva il marcatore e disponga lo split sui due volumi.

Meccanismo (boot, solo mount root, non-live):
- Dopo aver montato la FAT32 come root iniziale (come prima), DobFileSystem legge il marcatore
  /SYSTEM/CONFIG/Root_volume sulla FAT32. Contiene l'LBA assoluto (decimale) della partizione
  exFAT di root. Se assente -> nessun pivot, root FAT32 classica (zero regressioni).
- Se presente: carica exfat.mem DALLA FAT32 con un lettore INTERNO (resolve_full_path +
  read_cluster), non via dobfs_Open. Questo scioglie il nodo cane-coda: ensure_exfat_loaded usa
  una IPC verso "DobFileSystem" — cioe' verso se stesso — che in main(), prima del loop dei
  messaggi, andrebbe in deadlock. Il lettore interno evita l'IPC.
- Poi ri-punta l'offset di partizione sull'exFAT e commuta il mount a exFAT (exfat_try_mount).
  Da quel momento ogni accesso a file (/SYSTEM, programmi, /DATA) viene servito dall'exFAT.
- Sicurezza: i due read avvengono mentre la root e' ancora FAT32; solo dopo si commuta. Se il
  mount exFAT fallisce, l'offset FAT32 viene ripristinato e il sistema resta su FAT32.
- Il sandbox (/SYSTEM, /DATA) e' basato sul percorso, quindi continua a valere identico dopo il
  pivot, senza modifiche.

Nuovi helper (boot/DobFileSystem/main.c): fat32_read_file_internal, load_exfat_mem_internal,
read_root_volume_config. Tutti a 32 bit (nessuna dipendenza libgcc).

Verifica: compila pulito con i flag reali; 0 simboli libgcc a 64 bit (-O0/-O2/-Os); graffe
bilanciate. La logica e' verificata a livello di compilazione: la commutazione "in volo" della
root va provata su hardware reale. Inerte finche' l'installer non popola l'exFAT e non scrive
/SYSTEM/CONFIG/Root_volume sulla FAT32.

Prossimi pezzi: (1) installer che separa stub-FAT32 e root-exFAT e scrive il marcatore;
(2) impostazione di visibilita' della FAT32-stub sul desktop, sul modello di show_system_partition.

---

# MainDOB — FIX CRITICO: exfat.mem non veniva copiato (nessun mount exFAT funzionava)

Causa per cui le icone exFAT sul desktop davano errore e l'exFAT non veniva mai usato
("installa tutto in fat32"): exfat.mem viene COSTRUITO ma non finiva sul sistema.
DobFileSystem lo carica a runtime (dob_mem_load) per montare qualunque volume exFAT; senza,
ensure_exfat_loaded() fallisce ("cannot open exfat.mem") e OGNI mount exFAT salta — sia le
icone-partizione sul desktop sia l'aggancio del volume definitivo all'avvio. Quindi l'exFAT
restava inerte e tutto sembrava finire sulla FAT32.

- tools/mklive.sh: ora copia exfat.mem in /SYSTEM/OperatingSystem/DobFileSystem/ sull'immagine
  live (prima lo faceva solo mkbootdisk.sh). Necessario sia per montare exFAT dal live, sia
  perche' l'installer copia il sistema PROPRIO da questo albero live.
- programs/MainDOB_Setup (install_phase_os): ora copia exfat.mem sul target, come gia' avviene
  per iso9660.mem del cdrom. Senza, il sistema installato non poteva montare alcun exFAT.

Con questo, le icone exFAT sul desktop si montano (lettura/scrittura) e exfat_attach aggancia
davvero il volume definitivo all'avvio.

Wizard (sistemati i punti segnalati):
- La casella "Usa come volume definitivo" e' stata SPOSTATA dalla schermata "Parametri sistema"
  alla schermata "Selezione disco": compare sotto la lista, riferita al disco selezionato, e
  nomina la partizione exFAT trovata (o avvisa che non ce n'e' nessuna su quel disco). Il suo
  stato di spunta sopravvive al cambio di selezione.
- "Riepilogo installazione" ora mostra una riga "Volume definitivo: <partizione exFAT>" quando la
  casella e' spuntata, oppure "nessuno (solo FAT32)".

Verifica: programs/MainDOB_Setup/main.c compila pulito con i flag reali, graffe bilanciate;
tools/mklive.sh passa bash -n. Link finale con `make` + i686-elf.

---

# MainDOB — exFAT visibile: icone desktop + scelta esplicita nell'installer

Correzioni ai problemi del test: le partizioni exFAT non comparivano da nessuna parte e l'installer
non rendeva esplicita la registrazione del volume definitivo.

Desktop (le partizioni exFAT ora compaiono e si montano):
- Lo scanner partizioni (libdob/dob/partition) emetteva una subdevice solo per le partizioni FAT32
  (tipo 0x0B/0x0C). Ora emette anche per le exFAT (0x07) e marca la subdevice con il filesystem
  giusto: aggiunto VOLUME_FS_EXFAT, helper partition_type_is_exfat(), filtro di scansione esteso e
  emit_appeared che imposta volume_fs in base al tipo.
- Il parser DAS (hotplug) ora riconosce volume_fs = exfat (oltre a fat32).
- Nuovo config/DAS/partition_exfat.das: stesso meccanismo del fat32 (icona sul desktop, doppio
  clic monta via DobFileSystem secondario con fs=exfat — quindi in lettura/scrittura), icona
  tinta di verde per distinguerla. Viene copiato sull'immagine live dal loop generico dei .das e,
  avendo category=storage, l'installer lo seleziona e copia come gli altri.

Installer (scelta esplicita del volume definitivo):
- La schermata "Parametri sistema" ora mostra una casella "Usa come volume definitivo: Disco N,
  partizione M, exFAT (X GB)" quando sul disco di destinazione viene trovata una partizione exFAT
  (oltre alla FAT32 di boot). Spuntata = la registra (auto-aggancio all'avvio); tolta = solo FAT32.
  Se non c'e' nessuna exFAT, compare la nota "Nessuna partizione exFAT trovata sul disco". Cosi' il
  processo non e' piu' silenzioso: l'utente vede se l'exFAT e' stata rilevata e decide.
- La scrittura di Definitive_volume, la riga in Startup_modules e la copia di exfat_attach ora
  avvengono solo se la casella e' spuntata (st.use_exfat_definitive).

Nota sull'architettura: il sistema avviabile resta sulla FAT32 per scelta ("il kernel usa la sua
solita partizione fat32"); l'exFAT e' il volume definitivo di grande capacita' che viene agganciato
a runtime — ora pero' e' visibile (desktop + wizard), scrivibile e selezionabile.

Verifica: libdob/src/partition.c compila pulito e resta 0 dipendenze libgcc a 64 bit (-O0/-O2/-Os);
boot/hotplug/das.c e programs/MainDOB_Setup/main.c compilano puliti con i flag reali; graffe
bilanciate. Link finale con `make` e i686-elf.

---

# MainDOB — installer: registra la partizione exFAT "definitiva" (MainDOB_Setup)

Secondo pezzo del percorso exFAT, lato scrittore: l'installer ora riconosce e registra il volume
exFAT definitivo, completando la catena "kernel da FAT32 + attacca exFAT".

Modello additivo (coerente con la scelta architetturale): il sistema completo e avviabile resta
sulla partizione FAT32 come prima; in piu', se sul disco di installazione c'e' una partizione
exFAT oltre alla FAT32 di boot, l'installer la registra come volume definitivo (la grande exFAT
montata a ogni avvio — la sola via per dischi > ~120 GB).

In programs/MainDOB_Setup:
- detect_exfat_definitive(): all'inizio di INST_PHASE_CONFIG scansiona l'MBR del disco target e
  cerca una partizione exFAT diversa dalla FAT32 di boot. La conferma con la firma "EXFAT   " nel
  boot sector (non solo il tipo 0x07, che vale anche NTFS/IFS). Prima exFAT trovata vince.
- Quando presente: (1) aggiunge la riga /SYSTEM/PROGRAMS/exfat_attach/exfat_attach.mdl con
  needs:DobFileSystem allo Startup_modules generato; (2) scrive /SYSTEM/CONFIG/Definitive_volume
  con la stringa --mount esplicita (stesso provider/selector del disco target, lba della
  partizione exFAT, fs=exfat); (3) copia exfat_attach.mdl sul target. Fallimenti non fatali: il
  sistema avvia comunque da FAT32, solo senza auto-aggancio.

L'helper exfat_attach (gia' presente sull'immagine live, copiato dal loop generico di mklive) legge
quel config all'avvio e monta l'exFAT via un DobFileSystem secondario — che, grazie al routing
exFAT del server, e' a pieno titolo in lettura/scrittura.

Flusso completo ora possibile: in DobDisk crei sul disco una FAT32 di boot e una exFAT (formattando
entrambe); l'installer installa sulla FAT32, rileva l'exFAT e la registra; al riavvio il sistema la
aggancia da solo.

Verifica: programs/MainDOB_Setup/main.c compila pulito con i flag reali (-m32 -ffreestanding
-nostdinc -Wall -Wextra + include del progetto), zero diagnostica; graffe bilanciate. Link finale
con `make` e i686-elf.

---

# MainDOB — boot: aggancio automatico dell'exFAT "definitiva" all'avvio (exfat_attach)

Primo pezzo del percorso "installa su exFAT / exFAT nativa", lato lettore. L'architettura
concordata: il kernel avvia dalla solita partizione FAT32 (kernel + GRUB + exfat.mem + helper) e
poi "attacca" la grande partizione exFAT definitiva — la sola opzione per dischi > ~120 GB.
A runtime l'aggancio e' un mount secondario exFAT, che esiste gia' ed e' leggi/scrivi; mancava
l'automatismo all'avvio.

- Nuovo programma one-shot programs/exfat_attach: all'avvio legge /SYSTEM/CONFIG/Definitive_volume
  e, se presente, lancia un DobFileSystem secondario con --mount sulla partizione indicata
  (spawn_file, come gia' fanno USB e DobFiles). Il file di config E' la stringa di argomenti
  --mount (una riga di coppie key=value, es. "provider=ahci,selector=0,lba=2099200,id=7,fs=exfat"):
  l'helper la passa cosi' com'e' e lascia validare al parser --mount del server. fs= e'
  informativo — fat32_mount legge il boot sector e instrada a exfat.mem se vede la firma exFAT,
  quindi il volume viene montato in lettura/scrittura comunque. Config assente o vuoto = no-op.
- Contratto del config fissato (scelta dell'utente: config esplicita scritta dall'installer). La
  scrittura del config + il layout a due partizioni sono il prossimo pezzo (installer).
- Build: aggiunto exfat_attach a programs/Makefile (PROGRAMS, EPS_exfat_attach := DobFileSystem,
  NO_DOBUI — nessuna UI). Il loop generico di tools/mkbootdisk.sh copia gia' il .mdl in
  /SYSTEM/PROGRAMS/exfat_attach/; aggiunta la riga in Startup_modules dopo DobFileSystem con
  needs:DobFileSystem (parcheggia finche' la root non e' su). Sui dischi senza config e' inerte.

Verifica: programs/exfat_attach/main.c compila pulito con i flag reali (-m32 -ffreestanding
-nostdinc -Wall -Wextra); tools/mkbootdisk.sh passa bash -n. Link finale con `make` e i686-elf.

---

# MainDOB — DobFileSystem: exFAT a tutti gli effetti leggi/scrivi + etichetta GET_MOUNTED

Il server DobFileSystem aveva gia' l'instradamento completo verso exfat.mem (open/stat/read/
readdir e anche write/create/mkdir/unlink/rename/ftrunc/flush), ma era documentato come
"read-only / Phase 4 stub" — un residuo di quando exfat.c non aveva ancora le scritture. Ora
exfat.c implementa tutte le scritture (testate su host), quindi un volume exFAT montato e' a
pieno titolo leggi/scrivi: niente e' piu' rifiutato (resta read-only solo la modalita' live-CD,
per lo stesso gate generico che vale anche per FAT32).

- GET_MOUNTED ora riporta "exfat" quando il volume montato e' exFAT (prima era sempre "fat32",
  ignorando il tipo reale): cosi' DobDisk e chiunque interroghi il mount vedono il filesystem
  corretto.
- Aggiornati i commenti obsoleti che descrivevano exFAT come read-only.

Effetto pratico: una pendrive USB formattata exFAT (formattazione gia' possibile da DobDisk via
exfat_ops) viene montata, letta e SCRITTA come una FAT32 — copertura del terzo obiettivo (USB
exFAT leggi/scrivi/formatta). Restano da affrontare l'installazione su exFAT e il boot da exFAT
(il nodo e' la catena di avvio: GRUB che legge exFAT e la disponibilita' di exfat.mem prima del
montaggio della root quando la root stessa e' exFAT).

Verifica: boot/DobFileSystem/main.c compila pulito con i flag reali del build (stub minimo per
l'header EPS DobFiles.h, assente in questo ambiente). Il link finale resta `make` con i686-elf.

---

# MainDOB — DobDisk: correzioni al dialog di formattazione (focus, free space, tipo exFAT)

Correzioni ai problemi emersi al primo test del dialog di formattazione.

- I widget del dialog non rispondevano (textbox non cliccabile/scrivibile, dropdown del
  filesystem che restava su FAT32): la causa era che il dialog pilotava i widget a mano
  bypassando il focus manager, ma e' proprio dobfocus_OnClick a impostare il focus (necessario
  perche' la textbox accetti i tasti) e a gestire correttamente i dropdown. Ora il dialog usa un
  focus manager dedicato: dopo l'Init i widget vengono tolti dal focus manager globale
  (dobfocus_auto_unregister) e registrati in un dob_focus_t privato del dialog; click/tasti/scroll/
  drag/release passano per dobfocus_OnClick/OnKey/OnScroll/OnDrag/OnRelease su quel manager. Cosi'
  la textbox prende il focus e scrive, i dropdown si aprono e selezionano, e i widget del dialog
  non vengono piu' hit-testati insieme a quelli dell'UI principale quando il dialog e' chiuso.
- La formattazione di una partizione "vergine" (spazio libero -> "Crea partizione…") usava ancora
  il vecchio flusso a popup con FAT32 fisso. Ora act_create crea la partizione, aggiorna la lista e
  apre lo stesso dialog di formattazione sul nuovo slot; il filesystem e il tipo MBR finale vengono
  scritti dall'OK del dialog. (Se si annulla, la partizione resta creata ma non formattata e si puo'
  formattare dopo.)
- mbr_type_label non conosceva exFAT: una partizione exFAT appariva come "sconosciuto". Aggiunta la
  voce per MBR_TYPE_EXFAT (0x07) -> "exFAT".
- Dopo la formattazione il dialog ora ricostruisce la lista delle partizioni (cosi' la riga mostra
  il nuovo filesystem e la partizione appena creata compare) e riseleziona la partizione formattata
  aggiornandone dettaglio e barra d'uso.

Verifica: programs/DobDisk/main.c compila pulito con i flag reali del build
(-m32 -ffreestanding -nostdinc -Wall -Wextra + tutti gli include del progetto), zero diagnostica;
graffe bilanciate. Il backend (exfat_ops/fat32_ops/fs_ops) e' invariato rispetto alla verifica
precedente (formatter exFAT testato su host a 512/4096 con cluster auto ed esplicito, senza
dipendenze libgcc). Il link finale resta da fare con `make` e il cross-compiler i686-elf.

---

# MainDOB — DobDisk: dialog di formattazione articolato (FAT32/exFAT, settore, cluster, label)

La formattazione in DobDisk non usa piu' la sequenza di popup (conferma YesNo + InputBox per
l'etichetta): apre una sotto-finestra modale, disegnata dentro la finestra stessa di DobDisk, in
cui si scelgono tutti i parametri prima di scrivere.

Dialog (programs/DobDisk/main.c):
- Overlay modale: mentre e' aperto, ogni evento (click/move/release/dblclick/key/scroll/panel) e'
  instradato ai suoi controlli e la UI principale e' inerte; draw_all() lo disegna sopra a tutto.
  I widget sono pilotati a mano (nessun accoppiamento col focus manager), riusando dropdown e
  textbox di libdobui.
- Campi: Filesystem (FAT32 / exFAT), Settore logico (512 / 4096 byte), Dimensione cluster
  (Automatica / 4-128 KB), Etichetta (textbox), Numero di FAT (2 / 1).
- Vincoli applicati a ogni interazione: con FAT32 il settore e' forzato a 512 e il campo settore
  e' disabilitato (il reader FAT32 monta solo 512 byte), il numero di FAT e' selezionabile; con
  exFAT il settore 512/4096 e' libero e il numero di FAT e' forzato a 1 (exFAT usa una sola FAT).
- Mostra info partizione (numero, tipo, dimensione), l'avviso che tutti i dati saranno cancellati
  e, se la partizione e' montata, un avviso di smontaggio.
- Tasti: Esc = Annulla, Invio = OK; l'etichetta accetta testo quando ha il focus.
- OK (fmt_dialog_commit): legge i controlli, costruisce mkfs_options (label, num_fats,
  bytes_per_sector, cluster_size), sceglie exfat_ops o fat32_ops e chiama mkfs su
  (cur_disk_idx, start_lba, sectors). In caso di errore mostra un popup (per FAT32 ricorda il
  limite cluster di 64 KB). In caso di successo imposta il tipo MBR (0x07 exFAT / 0x0C FAT32 LBA),
  fa commit/rescan, aggiorna stato, dettaglio e barra d'uso. act_format() ora si limita a rifiutare
  la partizione di boot e ad aprire il dialog.

Backend (libdob):
- mkfs_options_t esteso con `cluster_size` (byte, potenza di 2; 0 = automatico in base alla
  dimensione del volume; dev'essere >= un settore logico). Onorato da entrambi i formatter:
  exfat_ops ricava il clus_shift (max 32 MB), fat32_ops ricava i settori per cluster (1..128, cioe'
  cluster massimo 64 KB; valori non validi -> mkfs fallisce).

Verifica: programs/DobDisk/main.c compila pulito con i flag reali del build
(-m32 -ffreestanding -nostdinc -Wall -Wextra e tutti gli include del progetto), zero diagnostica;
graffe bilanciate. exfat_ops/fat32_ops/fs_ops compilano puliti; exfat_ops resta senza dipendenze
libgcc a 64 bit a ogni livello di ottimizzazione. Test funzionale su host del backend cluster: il
formatter nativo a 512 e 4096 byte, con cluster sia automatico sia esplicito (32 KB e 64 KB),
produce volumi che il reader exFAT verificato monta, con round-trip create/write/read + mkdir +
create annidato; tutti i test passati. Nota: DobDisk dipende da stub generati da EPS non presenti
in questo ambiente, quindi qui e' verificato a livello sintattico/di tipi (con i flag reali) ma il
link finale va fatto con `make` e il cross-compiler i686-elf.

---

# MainDOB — exFAT formatter in libdob (exfat_ops): DobDisk puo' formattare exFAT

Aggiunta (Fase 2 del lavoro sul dialog di formattazione di DobDisk). DobDisk e MainDOB_Setup
formattano le partizioni chiamando direttamente l'astrazione fs_ops di libdob (non l'IPC
DOBFS_FORMAT). Era registrato solo fat32_ops; ora c'e' un exfat_ops parallelo.

libdob:
- exfat_ops (nuovo file src/exfat_ops.c): formatter exFAT nativo sopra il block layer, che
  rispecchia l'algoritmo di exfat.mem (gia' validato). mkfs scrive boot region + backup + boot
  checksum, FAT, Allocation Bitmap, Up-case Table compressa (60 byte, a-z->A-Z) e root directory
  con le entry critiche, tutto via block_write. detect legge il settore 0 e controlla
  0x55AA + "EXFAT   ". Supporta settori logici da 512 E 4096 byte (parametro `sectors` = settori
  fisici da 512; geometria in settori logici; un settore logico = 1 o 8 settori fisici). Tutta
  l'aritmetica e' a 32 bit (fs_ops passa un conteggio settori a 32 bit), quindi nessun helper
  libgcc a 64 bit. MBR type 0x07.
- mkfs_options_t esteso con `uint32_t bytes_per_sector` (0 = 512), usato da entrambi i formatter.
- fs_ops registry: aggiunto &exfat_ops; fs_ops_for_mbr_type / _for_name lo trovano.
- fat32_ops.mkfs: rifiuta bytes_per_sector != 512, perche' il reader FAT32 di MainDOB monta solo
  settori da 512 byte (main.c). Il 4096 e' quindi offerto solo per exFAT (il cui reader lo regge
  via s512_shift).
- partition.h: aggiunta la costante MBR_TYPE_EXFAT (0x07).
- Makefile: exfat_ops.c aggiunto alle sorgenti di libdob e exfat_ops.o al set DOBDISK_EXTRA
  (condiviso da DobDisk e MainDOB_Setup).

Verifica: fs_ops.c / fat32_ops.c / exfat_ops.c compilano puliti; exfat_ops.c non contiene tipi a
64 bit (nessun rischio libgcc). Test FUNZIONALE su host del formatter nativo a ENTRAMBE le
dimensioni (512 e 4096): exfat_ops.mkfs su un disco RAM -> exfat_ops.detect riconosce il volume ->
il reader exFAT verificato (exfat.mem) lo monta -> create/write/read (round-trip identico) +
mkdir + create annidato. TUTTI I TEST PASSATI.

Prossimo (Fase 3): il dialog di formattazione articolato in DobDisk (sotto-finestra modale con
filesystem, settore logico, dimensione cluster, etichetta, n. FAT) che sceglie fat32_ops/exfat_ops
e chiama mkfs con le opzioni.

---

# MainDOB — exFAT mkfs: supporto settori logici 512 e 4096 byte

Aggiunta (Fase 1 del lavoro sul dialog di formattazione di DobDisk). exfat_mkfs ora accetta
bytes_per_sector 512 O 4096 (BytesPerSectorShift 9 o 12). Il parametro `sectors` resta il numero
di settori FISICI da 512 byte della partizione; internamente la mkfs calcola VolumeLength e tutta
la geometria in SETTORI LOGICI (sectors >> (bps_shift-9)) e scrive tramite il callback a 512 byte
moltiplicando per s512 (1 o 8 settori fisici per settore logico, helper wr_lsec). La regione di
boot, il boot checksum (su 11 settori logici), la ExtendedBootSignature (offset bytes_per_sector-4)
e il riempimento del settore di checksum sono tutti relativi alla dimensione del settore logico.
La dimensione del cluster e' scelta in byte dalla dimensione del volume (4KB/32KB/128KB) e non e'
mai inferiore a un settore logico. Il resto del driver (lettura, scrittura, FAT, bitmap, directory)
era gia' byte-accurato e indipendente dalla dimensione del settore (usa s512_shift), quindi nessun
altro cambiamento e' stato necessario.

Verifica: exfat.c compila pulito su i686 (-m32); zero helper libgcc a 64 bit a -O0/-O1/-Os/-O2;
zero int, zero asm. Test FUNZIONALE su host eseguito a ENTRAMBE le dimensioni (512 e 4096 byte):
mkfs (boot signature, "EXFAT   ", BytesPerSectorShift, boot checksum ricalcolato, backup) + mount
del volume formattato + create/write/read (round-trip identico) + mkdir + rename + unlink (dir non
vuota rifiutata) + unmount/remount (persistenza). TUTTI I TEST PASSATI a entrambe le dimensioni.

Prossimo (Fase 2): exfat_ops in libdob + estensione di mkfs_options_t (tipo FS + settore logico) +
supporto 4096 nella mkfs FAT32, cosi' che DobDisk possa formattare exFAT/4096 via fs_ops. Poi
(Fase 3) il dialog di formattazione articolato in DobDisk.

---

# MainDOB — exFAT Fase 5: mkfs (formattazione) — DRIVER exFAT COMPLETO

Aggiunta (nessun bump di versione; Fase 5, ultima). Implementata la formattazione exFAT
(exfat_mkfs) in exfat.c e cablata in DobFileSystem. Con questo il driver exFAT scritto da zero
e' COMPLETO: lettura, scrittura, create, mkdir, unlink, rename, truncate e formattazione.

exfat.c — exfat_mkfs (scrive un volume exFAT vergine):
- Regione di boot (settori 0-11) + backup (12-23): Main Boot Sector con tutta la geometria
  (VolumeLength, FatOffset/Length, ClusterHeapOffset, ClusterCount, FirstClusterOfRootDirectory,
  FileSystemRevision 1.00, BytesPerSectorShift, SectorsPerClusterShift, NumberOfFats=1,
  PercentInUse, BootCode 0xF4, BootSignature 55AA); Extended Boot Sectors 1-8 con
  ExtendedBootSignature; Boot Checksum (settore 11) calcolato per spec (Fig. 1: somma rotativa
  sui settori 0-10, saltando i byte 106/107 VolumeFlags e 112 PercentInUse) e ripetuto nel settore.
- FAT: FatEntry[0]=media (F8FFFFFF), FatEntry[1]=FFFFFFFF, catene per bitmap/up-case/root, resto 0.
- Allocation Bitmap (cluster 2): primi N bit a 1 (i cluster di bitmap+up-case+root), resto 0.
- Up-case Table: invece della tabella raccomandata (enorme), una tabella COMPRESSA minima e
  valida di 60 byte che copre 0000-FFFF con il solo folding ASCII a-z -> A-Z, col suo TableChecksum
  (Fig. 3). E' conforme alla spec ED e' coerente col NameHash del driver (anch'esso solo ASCII).
- Root Directory: entry critiche Volume Label (0x83, etichetta opzionale), Allocation Bitmap
  (0x81, FirstCluster=2, DataLength=bitmap_bytes), Up-case Table (0x82, TableChecksum,
  FirstCluster, DataLength=60), poi EOD.
- Vincoli: solo settori da 512 byte (bytes_per_sector!=512 -> -3); dimensione cluster scelta dalla
  dimensione del volume (<=256MB->4KB, <=32GB->32KB, oltre->128KB); volume minimo 1 MB.

DobFileSystem (main.c):
- exfat_format(): ottiene la dimensione disco via ATA IDENTIFY, carica exfat.mem, chiama mkfs
  (il volume copre dal base partizione alla fine del disco). I callback scrivono gia' relativi
  alla partizione.
- Handler DOBFS_FORMAT: nuovo selettore di filesystem in arg0 (0 = FAT32, default e
  retrocompatibile; 1 = exFAT) + etichetta opzionale dal payload. Dopo il format, fat32_mount()
  rileva la firma "EXFAT   " e monta via exFAT.

Verifica: exfat.c e main.c compilano puliti su i686 (-m32); NESSUN helper libgcc a 64 bit a
-O0/-O1/-Os/-O2; zero istruzioni int (nessun int 0x85), zero asm inline. Nessuno stub residuo.
Test FUNZIONALE su host (disco RAM, exfat.c compilato nativo): mkfs -> verifica boot signature,
"EXFAT   ", BytesPerSectorShift, NumberOfFats, boot checksum (ricalcolato) e backup; poi mount
del volume formattato -> create/open/write/flush -> riapertura e read (round-trip identico) ->
mkdir + create annidato -> rename (nome nuovo ok, vecchio sparito, dati preservati) -> unlink
(dir non vuota rifiutata, dir vuota ok) -> unmount + remount (le cancellazioni persistono).
TUTTI I TEST PASSATI (0 fallimenti).

Limiti noti: il folding maiuscolo e' solo ASCII, quindi nomi con caratteri accentati/non-ASCII
hanno un NameHash approssimato e Windows potrebbe non trovarli per nome (i dati restano integri);
timestamp fisso (2024-01-01, niente RTC); directory assunte FAT-chained; nessun rollback
transazionale; rename su destinazione esistente rifiutato. DF su exFAT riporta ancora zeri e
GET_MOUNTED riporta "fat32" (cosmetici). Follow-up: integrazione fs_ops/DobDisk piu' profonda e
scelta exFAT nell'installer.

---

# MainDOB — exFAT Fase 4c: unlink + rename (percorso di scrittura COMPLETO)

Aggiunta (nessun bump di versione; Fase 4c, ultima del percorso di scrittura exFAT — resta solo
mkfs). Implementati unlink (cancella file E directory vuote) e rename (sposta/rinomina) in
exfat.c e instradati da DobFileSystem. Il percorso di scrittura exFAT e' ora COMPLETO tranne la
formattazione: lettura, scrittura, create, mkdir, unlink, rename e truncate funzionano tutti.

exfat.c — nuovi helper + operazioni:
- dir_set_delete: marca un entry-set come cancellato azzerando il bit InUse (bit 7) del TypeCode
  di ogni entry (0x85->0x05, 0xC0->0x40, 0xC1->0x41). Diventano "unused" (NON end-of-directory),
  quindi la scansione della directory prosegue oltre — niente troncamento accidentale.
- dir_is_empty: una directory e' vuota se non contiene nessun entry-set File in uso.
- unlink: risolve il path; rifiuta la root; se e' una directory non vuota -> errore (come FAT32:
  DENIED); libera la catena di cluster (bitmap + FAT; contiguo o FAT-chain); marca l'entry-set
  cancellato. Gestisce sia file sia directory vuote, come l'unlink di FAT32.
- rename: preserva TUTTI i metadati. Legge File+Stream vecchi (attributi, timestamp, NoFatChain,
  ValidDataLength, FirstCluster, DataLength) e ricostruisce l'entry-set col nuovo nome (FileName
  entries + NameLength + NameHash + SetChecksum), senza toccare i dati.
  - Fast path: stessa directory e stesso numero di entry -> riscrittura sul posto.
  - Percorso generale (directory diversa o lunghezza nome diversa): scrive il nuovo set nel
    genitore di destinazione (estendendolo se serve), poi cancella il vecchio. I cluster dei dati
    sono CONDIVISI, non liberati (lo spostamento e' solo di metadati).

DobFileSystem (main.c): unlink e rename instradati a exfat.mem; rename usa i path gia' validati.
unlink mappa "directory non vuota" -> DENIED; rename mappa "destinazione esistente" -> DENIED,
coerente con FAT32.

Verificato: exfat.c e main.c compilano puliti su i686 (-m32); NESSUN helper libgcc a 64 bit a
-O0/-O1/-Os/-O2; zero istruzioni int (nessun int 0x85), zero asm inline. Unico stub rimasto:
exfat_mkfs.

Limiti noti (follow-up): nessun rollback transazionale (durante un rename cross-directory l'entry
esiste due volte per un istante; in caso di crash chkdsk lo segnala, nessuna corruzione dei dati);
directory assunte FAT-chained; rename su una destinazione esistente e' rifiutato (nessun replace).

Prossimo: Fase 5 = mkfs (formattazione exFAT) + integrazione fs_ops/DobDisk.

---

# MainDOB — exFAT Fase 4b: create + mkdir (copia di file nuovi sulla pendrive)

Aggiunta (nessun bump di versione; Fase 4b del lavoro exFAT). Implementati create e mkdir in
exfat.c e abilitati da DobFileSystem: ora si possono CREARE file e cartelle nuovi su un volume
exFAT — cioe' copiare file su una pendrive exFAT da DobFiles. Si appoggia sul motore di
scrittura di Fase 4a. unlink/rename restano per la Fase 4c.

Conforme alla specifica Microsoft exFAT (verificati al byte: layout File entry 0x85 / Stream
Extension 0xC0 / File Name 0xC1, SetChecksum, e l'algoritmo NameHash sul nome up-cased).

exfat.c — nuovi helper:
- Encoding nome UTF-8 -> UTF-16LE (BMP; sequenze a 4 byte -> '?'), con validazione dei
  caratteri vietati da exFAT.
- NameHash (spec 7.6.4): stessa rotazione del SetChecksum sul nome up-cased. Up-casing ASCII
  (a-z -> A-Z, tabella mandatory dei primi 128) -> per i nomi ASCII l'hash combacia con quello
  di Windows; i nomi non-ASCII sono approssimati (limite noto).
- dir_find_free: cerca slot di directory liberi consecutivi (EOD/unused) in una directory
  FAT-chained, ESTENDENDOLA (alloca + azzera nuovi cluster) se non c'e' spazio.
- write_entry_set: costruisce e scrive l'entry-set completo (File + Stream + FileName), con
  timestamp fisso (2024-01-01; MainDOB non ha un orologio cablato qui) e SetChecksum.
- resolve_parent: separa il path in directory genitore + componente finale.

exfat.c — operazioni:
- create: file vuoto (FirstCluster=0, DataLength=0, attributo Archive). Cluster e dimensione
  li riempie il successivo open+write+close (motore 4a).
- mkdir: alloca e azzera un cluster (directory vuota, tutti EOD), attributo Directory,
  ValidDataLength==DataLength. Le directory exFAT non hanno "." e ".." (corretto).
- Se l'inserimento estende una directory NON-root, ne aggiorna la DataLength nel genitore
  (riuso di entry_meta_commit) cosi' il path di lettura vede le nuove voci.

DobFileSystem (main.c): open su mount exFAT ora gestisce O_CREATE (crea il file se assente, poi
lo apre) e O_TRUNC (tronca a 0 un file esistente) -> il flusso di copia di DobFiles
(O_WRITE|O_CREATE|O_TRUNC) funziona. mkdir e' instradato a exfat.mem. unlink/rename restano
rifiutati (Fase 4c).

Verificato: exfat.c e main.c compilano puliti su i686 (-m32); NESSUN helper libgcc a 64 bit a
-O0/-O1/-Os/-O2; zero istruzioni int (nessun int 0x85), zero asm inline, exfat.mem
self-contained. Build completa col cross-compiler a te.

Limiti noti (follow-up): nomi non-ASCII con hash approssimato (Windows potrebbe non trovarli per
nome); timestamp fisso; directory assunte FAT-chained; nessun rollback transazionale.
unlink + rename: Fase 4c.

---

# MainDOB — exFAT Fase 4a: motore di scrittura (file esistenti: write / extend / truncate)

Aggiunta (nessun bump di versione; Fase 4a del lavoro exFAT). Implementato il MOTORE di
scrittura exFAT in exfat.c e abilitata la scrittura su file ESISTENTI da DobFileSystem. E' la
base load-bearing del percorso di scrittura (allocatore, catena FAT, aggiornamento entry-set):
testabile da sola modificando un file gia' presente su una pendrive exFAT e verificandolo su
un'altra macchina. create/mkdir/unlink/rename restano per le Fasi 4b/4c.

exfat.c — nuove primitive:
- Allocatore a bitmap: alloc_cluster scandisce l'Allocation Bitmap per un bit libero (con hint
  next_free), free_cluster/free_chain liberano. La bitmap e' trattata come contigua da
  bitmap_cluster (layout di mkfs e caso comune on-disk).
- Scrittura catena FAT: fat_set scrive una entry FAT mantenendo coerente la cache di lettura.
  Solo FAT1 (num_fats>1 TexFAT non mantenuto).
- ensure_clusters estende la catena; converte un file contiguo (NoFatChain) in catena FAT alla
  prima estensione (stende i link FAT sul run esistente, poi aggancia i nuovi cluster).
- Localizzazione entry-set: dir_cur_t traccia un indice di entry; dir_next_file/resolve/open
  catturano (dir_cluster + set_offset) del File entry, finora placeholder. dir_entry_io da'
  accesso casuale alle entry di una directory FAT-chained.
- entry_meta_commit riscrive lo Stream (FirstCluster/DataLength/ValidDataLength/flag NoFatChain)
  e ricalcola la SetChecksum sull'intero set.

exfat.c — operazioni:
- write: estende (alloca+catena), riempie di zeri l'eventuale gap [valid,pos) (exFAT non ha
  buchi sotto ValidDataLength), scrive i dati settore per settore (RMW sui bordi parziali),
  aggiorna size/valid in memoria e marca dirty.
- ftrunc: crescita (alloca; i nuovi byte si leggono come zero perche' avanza DataLength ma non
  ValidDataLength) o restringimento (libera i cluster in coda).
- flush: committa i metadati differiti via entry_meta_commit; un file pulito e' no-op.

DobFileSystem (main.c): open su mount exFAT ora permette O_WRITE/O_APPEND su file esistenti
(O_CREATE/O_TRUNC restano rifiutati -> 4b); handle_write instrada le fd exFAT a exfat.mem; il
seek oltre EOF e' consentito alle fd scrivibili (il write riempie il gap). I metadati si
committano alla chiusura (come il percorso FAT32). mkdir/unlink/rename restano rifiutati.

Verificato: exfat.c e main.c compilano puliti su i686 (-m32); NESSUN helper libgcc a 64 bit a
-O0/-O1/-Os/-O2 (la matematica cluster usa shl64/shr64 + shift costanti + op a 32 bit, mai
divisioni/shift a 64 bit per valore runtime); zero istruzioni int (nessun int 0x85), zero asm
inline, exfat.mem self-contained (memcpy/memset propri, solo simboli interni). Build completa
col cross-compiler a te.

Limiti noti (follow-up): directory assunte FAT-chained per l'aggiornamento entry-set (root e
sottocartelle Windows lo sono; entry oltre il primo cluster di una rara directory contigua
multi-cluster non supportate); nessun rollback transazionale (un write fallito a meta' puo'
lasciare cluster orfani, recuperabili da chkdsk, nessuna corruzione); ValidDataLength==size sui
file scritti (no preallocazione sparsa). create/mkdir/unlink/rename: Fasi 4b/4c.

---

# MainDOB — exFAT Fase 3: routing FAT32/exFAT in DobFileSystem (lettura end-to-end)

Aggiunta (nessun bump di versione; Fase 3 del lavoro exFAT). DobFileSystem ora monta e
LEGGE volumi exFAT delegando a exfat.mem (Fase 1), instradando le richieste in base al
filesystem rilevato. È il passo che rende il tutto testabile end-to-end: una pendrive
exFAT si monta, si naviga e si leggono i file (anche >4GB) da DobFiles.

Rilevamento + caricamento:
- fat32_mount() legge il settore 0 e, se trova "EXFAT   " all'offset 3, instrada il
  volume a exfat.mem invece di parsare il BPB FAT32. Tutti i call site di fat32_mount
  (try_mount, REMOUNT, ecc.) ereditano il rilevamento.
- exfat.mem e' caricato in modo LAZY (una volta per processo), letto da
  /SYSTEM/OperatingSystem/DobFileSystem/exfat.mem via lo stub dobfs (la root FAT32 e'
  su -> niente circolarita': solo le istanze secondarie montano exFAT) e mappato con
  dob_mem_load. libdob/mem.o e' gia' sulla link line.
- Callback rd/wr = disk_read_sectors/disk_write_sectors (partition-relative, 512 byte).

Routing degli handler: open/stat/read/readdir/close/seek controllano ex.active (o
fd->is_exfat) e servono exFAT prima di toccare la geometria FAT32. La struct fd guadagna
is_exfat + un exfat_file_t; le fd FAT32 restano invariate (alloc_fd azzera lo slot). stat
riporta la size a 64 bit (arg0/arg2, type in arg1, dal wire di Fase 2); readdir emette le
stesse righe "name\tD|F\tsize" del cdrom; read fa streaming via exfat.mem in sector_buf;
seek posiziona exfat_file.pos.

Read-only in Fase 3: le entry di scrittura di exfat.mem sono stub (Fase 4), quindi su un
mount exFAT open con intento di scrittura e mkdir/unlink/rename ritornano DOB_ERR_REJECTED;
scrivere su una fd exFAT (aperta read-only) e' gia' negato dal controllo O_WRITE.

Non toccato: floppy/cdrom/DobArchive (stesso protocollo, restano FAT32-agnostici). Limiti
noti (follow-up): DF su un mount exFAT riporta zeri e GET_MOUNTED riporta fs_type "fat32"
(solo cosmetico in DobDisk; la protezione no-format usa disk+lba, non il tipo FS); la size
nei listing resta a 32 bit (dobfs_dirent_t.size — la copia usa la size a 64 bit di Stat).
Scrittura exFAT, mkfs e case-folding Unicode: fasi successive.

Verificato: main.c compila pulito sul target i686 (-m32) con tutte le branch di routing;
le firme delle callback combaciano con exfat_api.h. Build completa col cross-compiler a te
(gli header EPS sono generati al build).

---

# MainDOB — Protocollo dobfs a 64 bit (Fase 2: offset/dimensioni per file >4GB)

Aggiunta (nessun bump di versione; Fase 2 del lavoro exFAT). Prepara il protocollo del
filesystem e il server FAT32 a rappresentare offset e dimensioni a 64 bit, cosi' che il
routing exFAT (Fase 3) possa gestire file >4GB end-to-end. FAT32 resta invariato
funzionalmente (i suoi file sono <=4GB: la parola alta e' sempre 0).

Cosa cambia:
- dobfs_fd_t (server): offset e file_size da uint32_t a uint64_t.
- dobfs_stat_t.size: da uint32_t a uint64_t.
- dobfs_Seek: da long/long a int64_t/int64_t (offset e ritorno).
- Wire SEEK: l'offset a 64 bit viaggia come (arg3:arg1), whence in arg2; la posizione
  di ritorno come (reply.arg1:reply.arg0). I campi alti stanno in slot arg PRIMA
  inutilizzati (arg3, reply.arg1), quindi parola bassa e whence mantengono la posizione
  storica.
- Wire STAT: size come (arg2:arg0); type RESTA in arg1. size_hi e' in arg2 (prima
  inutilizzato).

Perche' floppy/cdrom/DobArchive NON sono toccati: tutti e tre implementano DOBFS_SEEK/
STAT ma gestiscono solo file <=4GB. Poiche' (a) il framework server azzera arg0..arg3
della reply prima di ogni dispatch (libdob/src/server.c) — e DobArchive fa memset — e
(b) i nuovi campi a 64 bit sono in slot prima inutilizzati, i loro slot "hi" valgono 0
e restano wire-compatibili senza modifiche. Lo stub aggiornato ricostruisce il valore a
64 bit; per i backend <=4GB la parola alta letta e' 0 = corretto.

Niente __udivdi3/__lshrdi3 (userspace non linka compiler-rt): i percorsi byte<->cluster
del server FAT32 operano sempre su valori <=4GB (un file exFAT in Fase 3 sara' letto via
il .mem, non da queste routine), quindi l'operando uint64 viene castato a (uint32_t)
prima di ogni divisione per bytes_per_cluster. Nessun divide/shift a 64 bit per valore a
runtime; handle_seek usa solo shift costanti (>>32).

Limite mantenuto in handle_seek: il backend FAT32 rifiuta posizioni oltre 4GB-1 (un file
FAT32 non puo' superarle). Il wire a 64 bit serve a exFAT, i cui fd la Fase 3 instradera'
su un path di seek dedicato anziche' su handle_seek.

Compatibilita': i client (spawn, DobFiles, calculator, MainDOB_Setup, DobArchive)
compilano invariati — la build usa -Werror solo per implicit-function-declaration, non
per i narrowing, e dobfs_Seek con (long)off promuove a int64_t. Verificato:
DobFileSystem_stub.c e main.c (target i686, -m32) compilano puliti.

---

# MainDOB — Supporto exFAT (Fase 1: parser read-only come .mem)

Aggiunta (nessun bump di versione; Fase 1 di un lavoro a fasi). Motivazione: file
>4GB e settori logici non-512 (512..4096), che FAT32 non copre. exFAT e' realizzato
come SHARED OBJECT .mem (boot/DobFileSystem/exfat.{c,h}) sul modello di
drivers/cdrom/iso9660.mem: una sola implementazione servira' sia i mount nativi sia
le pendrive, dato che DobFileSystem.mdl e' un unico binario per entrambi. I/O
esclusivamente tramite due callback rd/wr a settori da 512 byte, partition-relative
(saranno disk_read_sectors/disk_write_sectors di DobFileSystem). Contratto pubblico
in boot/DobFileSystem/exfat_api.h.

Cosa c'e' in Fase 1 (READ-ONLY): mount (parse+validazione boot region "EXFAT   "/
0xAA55/0xEB, geometria, scansione root per Allocation Bitmap 0x81 e Up-case Table
0x82), unmount, stat, open, read (catene FAT + file contigui NoFatChain, zero-fill
oltre ValidDataLength), readdir. Parsing dei set di directory con verifica del
SetChecksum, nomi UTF-16LE->UTF-8 (BMP), risoluzione path. write/create/mkdir/unlink/
rename/ftrunc/mkfs sono stub che ritornano -3; flush ritorna 0 (read-only: niente di
sporco). Il read-only non puo' corrompere un volume: per questo arriva per primo.

Vincoli onorati:
- Nessuna collisione con int 0x85 (video boomerang del kernel): in exfat.c lo 0x85 e'
  SOLO la costante-dato EXFAT_ENTRY_FILE (TypeCode di directory), confrontata come
  byte. Zero istruzioni `int`, zero asm inline (verificato): un byte di dato non puo'
  scatenare un vettore di interrupt — namespace diversi.
- Niente libgcc: il .mem non linka compiler-rt, quindi nessuna divisione/
  moltiplicazione/shift a 64 bit per valore a runtime (pullerebbe __udivdi3/__muldi3/
  __lshrdi3 = link fallito). Tutta l'aritmetica a 64 bit e' shift costanti, add/sub/
  compare, o passa per shl64()/shr64() fatti a mano. Zero `/`, `%`, `*` su uint64_t.

Build: boot/Makefile compila+linka exfat.mem in due passi con ld diretto (-shared
-nostdlib --no-undefined -Bsymbolic) e verifica ET_DYN, identico a iso9660.mem; e'
prerequisito order-only di DobFileSystem.mdl e nel target all. tools/mkbootdisk.sh lo
copia in /SYSTEM/OperatingSystem/DobFileSystem/exfat.mem (inerte finche' il routing
non arriva in Fase 3).

Limiti noti di Fase 1 (follow-up): case-folding solo ASCII per il match dei nomi (la
Up-case Table e' localizzata ma non applicata); filename con surrogati astrali -> '?';
la lettura di file frammentati ri-percorre la FAT da first_cluster ad ogni chiamata
(O(n^2); i file contigui sono O(1); una cache a 1 settore di FAT limita l'I/O). Fasi
successive: offset a 64 bit nel protocollo dobfs, routing FAT32-vs-exFAT in
DobFileSystem, path di scrittura, mkfs + fs_ops/DobDisk.

---

# MainDOB — Copia HDD->pendrive: i transitori di SCRITTURA non venivano ritentati ("operazione completata con N errori")

Sintomo (hardware reale): trascinare un file dal disco fisso a una pendrive fallisce
ai primi tentativi; ogni operazione fallita mostra "Operazione completata con N
errori" e serve riprovare piu' volte prima che vada. Nessun bump di versione.

Causa: asimmetria nel loop di copia + transitori di scrittura.
- Il loop di copia (programs/DobFiles, op_do_chunk) ritenta le LETTURE corte fino a
  OP_IO_RETRIES volte, ma sulle SCRITTURE chiama op_note_error al primo wr != rd:
  ZERO retry. La pendrive e' proprio il lato che fa transitori (commit NAND, NAK, o
  la prima scrittura dopo il wake della porta UHCI su silicio reale), quindi i suoi
  hiccup diventavano subito errori per-file. Il "riprovare l'intera operazione"
  dell'utente era, di fatto, il retry della scrittura mancante. Prima del fix
  copy-integrity questi stessi errori erano SILENZIOSI (file troncati/vuoti); ora
  sono visibili come "N errori" — stessa radice, ora osservabile.
- Un retry ingenuo a livello DobFiles sarebbe SCORRETTO: handle_write avanza
  f->offset PRIMA del flush del cluster (dentro il while, per-cluster), quindi su
  flush fallito l'fd e' gia' avanzato — ri-scrivere lo stesso chunk lo metterebbe
  all'offset sbagliato (gap/sovrapposizione = corruzione).

Fix (al posto giusto e idempotente): boot/DobFileSystem/fd_flush_write ritenta lo
STORE del cluster fino a FD_FLUSH_RETRIES volte, con FD_FLUSH_RETRY_MS di attesa tra
i tentativi. Reissue dello stesso cluster e' idempotente: su fallimento write_cluster
non tocca stato e fd_flush_write non ha ancora azzerato buf_dirty/buf_pos, quindi ogni
tentativo riscrive esattamente gli stessi byte allo stesso LBA. Per i cluster parziali
il read-modify-write e' fatto UNA volta prima del ciclo, cosi' il retry e' un puro
store. Copre tutti i percorsi di scrittura (copia, salvataggio editor, flush a
close/fsync); e' l'analogo lato-scrittura del retry di lettura.

Compromesso: su un guasto PERSISTENTE (es. chiavetta write-protected, CSW status
sempre 1) i tentativi si sommano al costo del bot_xfer fallito (fino a ~3 s ciascuno
tra timeout/NAK), quindi un cluster davvero non scrivibile fallisce dopo qualche
secondo invece che subito. Il caso comune (transitorio che si risolve) costa al piu'
un tentativo extra. FD_FLUSH_RETRIES/FD_FLUSH_RETRY_MS sono tarabili.

---

# MainDOB — UHCI: overflow del pool TD su endpoint bulk a max-packet piccolo (corruzione DMA)

Bug solo-su-hardware-reale, trovato continuando la recon lettura/scrittura. Nessun
bump di versione. EHCI/xHCI restano bozze e non sono toccati.

## usb_uhci / uhci_bulk_xfer — TD pool overflow (OOB write nel DMA)

Un trasferimento bulk e' ceil(len/mps) pacchetti, un TD ciascuno. mps e' legalmente
8/16/32/64 a full-speed, non sempre 64. Il build single-shot scriveva xfer_td[n]
senza limite contro XFER_MAX_TD (136, dimensionato per mps=64): con mps=8 una fase
dati da 4 KB sono 512 TD, ben oltre il pool, e il loop sforava xfer_td[] corrompendo
la struttura DMA successiva (bounce buffer / QH / frame list); poi il controller
faceva bus-master su TD spazzatura. QEMU non lo inciampava mai (mps=64); su silicio
full-speed reale con bulk a mps piccolo si corrompe memoria al primo trasferimento
multi-settore.
- Fix: la transazione viene ora schedulata in run da al piu' (XFER_MAX_TD-2) TD. La
  data toggle e' gia' persistente per-endpoint (toggle_*_map), quindi attraversa i
  run come richiede il bus. A mps=64 l'intero UHCI_XFER_MAX entra in un run solo ->
  nessun cambio di comportamento per il caso comune; solo i device a mps piccolo
  vengono spezzati. Guard "n >= XFER_MAX_TD -> -1" nel build come backstop. Le
  condizioni d'errore (NAK -4, stall -2, timeout -1) e la semantica short-IN sono
  preservate; un -4 a meta' transazione porta comunque al reset-recovery del livello
  superiore (toggle azzerate, retry pulito), quindi nessun rischio di desync da
  progresso parziale.

## usb_uhci / dispatcher BULK — rimosso l'overload di arg2

UHCI_OP_BULK_XFER leggeva use_window = (arg2 == 1) mentre lo stesso arg2 era gia' il
max-packet dell'endpoint. Il path window e' ritirato (il kernel nega la mmap della
RAM di un altro processo) e un mps reale non e' mai 1: codice morto ma ambiguo.
Rimosso: l'OUT copia sempre dal payload IPC, l'IN restituisce sempre xfer_buf; arg2
e' ora solo il max-packet. (L'op GET_WINDOW resta, innocua e inutilizzata.)

---

# MainDOB — I/O error masking residuo nel block layer + irrobustimento Bulk-Only Transport

Seguito dell'analisi copy-integrity sul percorso lettura/scrittura USB. Tre
correzioni, nessun bump di versione.

## block.c — gli adapter di scrittura mascheravano gli errori del provider

do_call() promette nel suo commento "Returns true on DOB_OK from the driver" ma
ritornava solo lo stato di TRASPORTO di dob_ipc_call: sys_send (kernel) restituisce
il risultato del round-trip IPC, non reply.code — quest'ultimo viene solo copiato
nella struct del chiamante. I tre adapter di scrittura (ata_write_op, ahci_write_op,
usbms_write_op) ritornavano direttamente do_call(): un errore lato driver
(reply.code != DOB_OK) veniva visto come scrittura RIUSCITA. Gli adapter di lettura
si salvavano per caso grazie al controllo su reply.payload; i write no. Stesso bug
gia' corretto in DobFileSystem/disk_write_sectors e gia' tappato a valle in
MainDOB_Setup con un read-back di verifica.
- Fix alla radice in do_call(): ora ritorna true solo se IPC == DOB_OK E
  reply->code == DOB_OK. Allinea il codice al contratto gia' documentato e copre in
  un colpo write, format (fat32_ops mkfs) e i percorsi di DobDisk. Il workaround di
  verifica in MainDOB_Setup diventa ridondante (lasciato com'e', innocuo).

## usb_mass_storage / bot_xfer — Bulk-Only Transport piu' robusto sui desync

- dCSWTag non veniva confrontato col dCBWTag inviato. Una CSW vecchia o duplicata
  rimasta nel pipe dopo un recovery veniva accettata, accoppiando lo stato sbagliato
  al comando sbagliato (write fallita letta come "good", o viceversa). Ora un tag
  mismatch -> bot_reset_recovery + errore.
- bCSWStatus == 2 (Phase Error) veniva ritornato come semplice fallimento di comando,
  senza reset: il comando successivo desincronizzava. Ora innesca bot_reset_recovery
  come da spec BOT 1.0 (6.5/6.7).

## usb_uhci — verificato, nessuna modifica

Il mancato retry del NAK (-4) nella FASE DATI di bot_xfer e' stato indagato e si
conferma CORRETTO cosi': uhci_bulk_xfer NON avanza la toggle map quando ritorna -4
(rc==2 esce prima dell'avanzamento), e una fase dati multi-pacchetto puo' NAKare a
meta' con la toggle hardware del device gia' avanzata. Un retry in place
ri-spedirebbe dal primo pacchetto con toggle disallineata -> proprio la corruzione
cercata. CBW e CSW possono ritentare perche' sono a pacchetto singolo. Il path
attuale (data-phase -4 -> bot_fail -> reset toggle -> retry pulito dal livello
superiore) e' la scelta sicura.

---

# MainDOB — Copie vuote/troncate in silenzio: errori di I/O mascherati come EOF / scrittura corta

Sintomo: le copie verso/da pendrive a volte risultano vuote o tagliate a meta', in
modo intermittente e in entrambe le direzioni, senza errore visibile.

Causa (radice, separata dagli stalli): un errore di I/O su disco NON veniva mai
segnalato come errore — veniva presentato come un trasferimento corto, che chi sta
sopra scambiava per fine-file / completamento.
- handle_read (dobfs): se la lettura di un cluster fallisce a meta' file, fa break e
  ritorna DOB_OK con i byte parziali. Se fallisce al primo cluster del chunk, ritorna 0
  — indistinguibile da EOF.
- handle_write (dobfs): se il flush di un cluster fallisce, fa break e ritorna DOB_OK
  con i byte parziali (anzi sovra-contati). Indistinguibile da una scrittura riuscita.
- Loop di copia (DobFiles): trattava rd<=0 come EOF ("EOF or error — advance") e
  contava l'intero chunk come scritto anche su scrittura corta. Quindi un singolo
  trasferimento USB fallito (NAK/busy/toggle, intermittente) troncava la copia in
  silenzio: vuota se al primo chunk, tagliata se a meta'.

Fix:
- boot/DobFileSystem/main.c (handle_write): su fd_flush_write fallito ritorna
  DOB_ERR_INTERNAL invece di mascherare l'errore come scrittura corta. Lo stub mappa il
  codice <0 a -1, quindi il chiamante ora VEDE l'errore di scrittura. (fd_flush_write
  torna false solo su errori di scrittura disco — nessun caso benigno, fix pulito.)
- programs/DobFiles/main.c (loop di copia chunked):
  * Lettura: distingue EOF reale da errore mascherato confrontando i byte copiati con la
    dimensione della sorgente (op.file_done vs op.file_size, gia' tracciati). Un rd==0
    con file_done < file_size = lettura finita in anticipo (errore), non EOF.
  * Ritenta la lettura corta fino a OP_IO_RETRIES volte: la causa tipica e' un disturbo
    USB transitorio che si risolve, e il reset-recovery del driver riallinea il data
    toggle sul tentativo fallito. Un fill fallito NON avanza l'offset, quindi il retry
    rilegge lo stesso punto in sicurezza.
  * Scrittura: rileva wr != rd (ora un errore di scrittura ritorna <0).
  * Su fallimento duro (lettura o scrittura): errore VISIBILE (op_note_error) e rimozione
    della destinazione parziale (dobfs_Unlink) — niente piu' file troncati lasciati li'
    in silenzio. EOF genuino (file_done == file_size) chiude e prosegue come prima.

Effetto: una copia o riesce completa, o fallisce in modo visibile e recuperabile (con
retry automatico dei disturbi transitori); mai piu' un file corto o vuoto spacciato per
riuscito. La correzione di handle_write rende gli errori di scrittura visibili anche agli
altri scrittori (es. salvataggio dell'editor).

Resta noto: handle_read continua a mascherare gli errori di LETTURA verso altri lettori
(es. caricamento dell'editor) — le copie sono coperte dal controllo di dimensione in
DobFiles, ma il caricamento dell'editor su un file di pendrive difettoso potrebbe ancora
troncare. Fix universale (propagare l'errore da handle_read) disponibile su richiesta.

Toccati: boot/DobFileSystem/main.c, programs/DobFiles/main.c.

---

# MainDOB — Stalli ripetuti durante copia su pendrive (flush sincrono per-scrittura) + Reset Recovery BOT

Sintomo: copiando un file grande su pendrive il trasferimento si "impunta" piu' volte
per parecchio tempo (a 0%, a meta', e soprattutto a 100%). Non e' un blocco infinito
ne' un errore: e' un'attesa lunga.

Causa (diagnosi corretta): l'attesa NON e' polling sprecato. xfer_run esce gia' appena
il TD si completa, quindi aspetta esattamente quanto serve al device; il "freeze" e' il
tempo reale di commit del device su NAND. Il moltiplicatore era la SYNCHRONIZE CACHE
emessa da usbms dopo OGNI scrittura: forzava un flush SINCRONO batch per batch, in serie
con il trasferimento (zero sovrapposizione). Ogni batch quindi aspettava il proprio flush,
e al 100% si sommava lo svuotamento finale della cache del device.

Fix 1 (allentamento del flush — il vero rimedio):
- drivers/usb_mass_storage/main.c: rimossa la scsi_sync_cache() dal path di scrittura
  (case 2 WRITE). I dati ora entrano nella cache del device e vengono scritti su NAND in
  BACKGROUND, sovrapposti ai batch successivi (throughput = velocita' sostenuta del device,
  non piu' write+flush serializzati).
- drivers/usb_mass_storage/main.c: aggiunta op 4 = FLUSH (SYNCHRONIZE CACHE on demand,
  best-effort). Opcode 4 era libero (usati 1,2,3,66,67).
- boot/DobFileSystem/main.c: nuova disk_flush() (op 4 verso il provider, risultato ignorato)
  chiamata in coda a fat_flush() — il punto di commit dei metadati, eseguito a ogni close /
  mkdir / delete. Un solo SYNCHRONIZE CACHE li' svuota tutta la cache del device (dati +
  dirent + FAT insieme), quindi la durabilita' resta garantita a granularita' di close.
  GATED su mount_secondary: solo la pendrive emette FLUSH; il FS di boot (path ATA) resta
  identico (e usbms serve comunque solo la pendrive). Effetto: stalli a meta' spariti, il
  100% si riduce al solo flush residuo (corto se il background ha tenuto il passo).

Fix 2 (Reset Recovery BOT — niente piu' replug fisico):
- drivers/usb_mass_storage/main.c: nuova bot_reset_recovery() = Bulk-Only Mass Storage Reset
  (class request 0xFF, wIndex=0: interfaccia singola, tutte le chiavette comuni) + CLEAR_HALT
  e reset toggle su entrambi gli endpoint bulk IN/OUT. Chiamata (via bot_fail()) su OGNI
  fallimento di bot_xfer DOPO che il CBW e' gia' partito (CBW/dati/CSW falliti o CSW malformato).
  Senza, un trasferimento che lascia il device a meta' sequenza desincronizzava il comando
  successivo e mandava tutto in stallo fino allo scollegamento fisico; ora la macchina a stati
  del device viene resettata e il comando dopo riparte pulito. Scatta solo sui fallimenti, il
  percorso normale e' invariato.

Nota: una prima ipotesi (rilevazione "veloce" del NAK in xfer_run, per arrendersi prima)
e' stata SCARTATA: avrebbe fatto fallire i flush lunghi ma legittimi (proprio il caso del
100% su file grande) e, peggio, disabilitato ogni sync successivo. La logica di attesa di
xfer_run e' rimasta invariata.

Toccati: drivers/usb_mass_storage/main.c, boot/DobFileSystem/main.c. drivers/usb_uhci/main.c
NON modificato.

---

# MainDOB — Salvataggio/apertura cross-processo su volume montato (pendrive/partizione) via dialogo

Bug: aprire l'editor, scrivere e salvare su una pendrive scelta col "Monta" nel
dialogo Salva restituiva errore e non salvava. Stesso problema aprendo un file da
pendrive via dialogo.

Causa (lacuna di design esposta dal fix hijack precedente, NON regressione di un
write): il dialogo restituisce al chiamante SOLO un path nudo ("path only — caller
saves the file itself"). Lo stub dobfs instrada per PREFISSO di path, uguale in ogni
processo: /SYSTEM,/DATA -> FS boot; /u<n> -> "floppy"; tutto il resto -> SVC_DEFAULT
(il FS locale del processo). Il floppy funziona cross-processo proprio perche' i suoi
path sono /u<n>, cablati a "floppy" ovunque. Ma una partizione/pendrive monta come
dobfs_<token> con path tipo "/file": lo stub dell'editor li manda al FS LOCALE (non
alla chiavetta), dove il sandbox nega la scrittura fuori da /DATA -> errore. Il fix
hijack aveva sbloccato la NAVIGAZIONE del dialogo sulla pendrive, ma il path tornava
senza l'informazione di QUALE servizio FS usare.

Fix (additivo, trasparente ai chiamanti, riusa OpenOn/StatOn gia' presenti):
- boot/DobFileSystem/DobFileSystem_stub.c: dobfs_Open e dobfs_Stat riconoscono un path
  QUALIFICATO "servizio:/realpath" (un ':' prima del primo '/', con '/' subito dopo) e
  delegano a dobfs_OpenOn/dobfs_StatOn sul servizio esplicito. La mappa vfd->(servizio,
  fd_reale) creata da OpenOn porta poi ogni Write/Close successivo al FS giusto. I nomi
  FAT non possono contenere ':' e i path reali iniziano con '/', quindi non c'e' collisione
  con i path normali: un ':' piu' avanti nel path (es. "/DATA/a:b") non scatta.
- programs/DobFiles/main.c: alla conferma del dialogo (apri e salva), il path di ritorno
  viene qualificato con il servizio (dobfs_get_service) "servizio:/path" quando il volume
  montato NON e' raggiungibile dal routing di default del chiamante — cioe' tutto tranne
  il FS boot ("DobFileSystem", gia' di default ovunque) e il floppy (via /u<n>). Boot e
  floppy restano path nudi (retro-compatibili).

Effetto: l'editor (invariato) ora salva e apre correttamente su pendrive/partizioni
scelte col Monta nel dialogo. Completa la feature mount-hijack per il caso cross-processo.
Desktop e drag&drop invariati.

# MainDOB — "Monta" ora dirotta la finestra anche per partizioni e pendrive (non apre piu' una nuova finestra)

Feature mancante (non un bug): nel "Monta" di Esplora file (e nei dialoghi
apri/salva), cliccare un floppy o un CD navigava la finestra/dialogo CORRENTE sul
nuovo filesystem; cliccare una partizione o una pendrive apriva invece una FINESTRA
NUOVA.

Causa: il meccanismo di hijack e' completo tranne un anello. Il flag
`hijack_target_port` (la finestra che ha aperto la vista Monta) viaggia cosi':
`do_enter_mount` -> ICON_ACTIVATED -> hotplug `ctx.hijack_target_port` -> il
primitive DAS `ipc_call` lo inoltra come **arg0** ad ogni chiamata dell'action. I
driver floppy/cdrom leggono arg0 e lo passano a `dobfiles_OpenMount` (0 = nuova
finestra, !=0 = dirotta quella finestra). Ma per partizioni/pendrive l'action
finisce con `ipc_call dobfs_$token 21` (OPEN_VIEW), e l'handler `handle_open_view`
di DobFileSystem **ignorava msg** e passava `0u` hardcoded -> sempre finestra nuova.

Fix (un solo punto): `handle_open_view` ora legge `msg->arg0` (il
`hijack_target_port` inoltrato dal DAS) e lo passa a `dobfiles_OpenMount`, identico
al pattern del driver floppy. Lo stesso handler serve sia le partizioni (dobfs
--mount) sia le pendrive (la dobfs secondaria che usbms spawna via PREPARE_VOLUME),
quindi l'unica modifica copre entrambe.

Il resto della catena era gia' generico e non e' stato toccato: lo stub
`dobfiles_OpenMount` impacchetta `service\0root\0` e invia FILES_CMD_MOUNT a quella
porta; l'handler FILES_CMD_MOUNT in DobFiles fa `dobfs_set_service(service)` +
`read_directory()` (e risponde PRIMA di leggere, per non andare in deadlock quando
il chiamante e' il servizio montato). Vale per qualunque servizio: `floppy` come
`dobfs_<token>`.

Comportamento desktop invariato: il doppio-click sull'icona del dispositivo passa
`hijack_target_port = 0` (lo imposta dobinterface), quindi continua ad aprire una
finestra nuova. Cambia solo il click DENTRO la vista Monta / un dialogo, che ora
naviga in place.

---

# MainDOB — Drag-and-drop lento: la FAT ora si scrive a "dirty-range", non a finestra intera

Sintomo: copiare un file medio-grande (es. quack.mp2) sulla pendrive riesce, ma
arrivata al 100% la finestra di progresso resta ferma alcuni secondi prima di
dichiarare "completato". Quei secondi sono il flush FINALE della FAT in
`dobfs_Close`.

Causa: `fat_flush` (e il flush al cambio finestra in `fat_cache_load`) riscriveva
SEMPRE l'intera finestra di cache FAT — `FAT_CACHE_SECTORS` = 128 settori —
moltiplicata per le 2 copie FAT = ~256 settori a ogni flush, anche se era cambiato
un solo entry. Su USB, con la latenza di commit per scrittura, sono i secondi di
attesa dopo il 100%.

Fix: dirty-range tracking della finestra FAT.
- Due variabili `fat_dirty_lo`/`fat_dirty_hi` tengono l'intervallo di settori
  (offset dalla base della finestra) effettivamente modificati.
- `fat_write` estende l'intervallo al settore dell'entry toccato.
- `fat_flush` riscrive SOLO `[lo,hi]` (x2 copie), non i 128 settori. Un salvataggio
  tipico tocca una manciata di settori FAT -> da ~256 scritture a 2-4.
- `fat_cache_load` al cambio finestra ora chiama `fat_flush` (stesso dirty-range,
  niente duplicazione di codice). Il caricamento legge ancora l'intera finestra
  (corretto: si scrive poco, si legge tutto).

Nessun cambiamento di correttezza: si scrivono esattamente gli stessi byte FAT che
sono cambiati, solo senza trascinare i settori immutati. Vale per tutte le scritture
(drag-and-drop, editor, format), non solo per la pendrive.

---

# MainDOB — L'editor non svuota piu' un file esistente: O_TRUNC reso non distruttivo (atomico)

Sintomo: modificare con l'editor un file di testo che ha gia' del contenuto sul
volume USB lo SVUOTA (il nome resta, il contenuto sparisce, verificato sia su
MainDOB sia su un PC esterno). Il drag-and-drop di un file NUOVO invece funziona.

Causa: l'editor salva con O_TRUNC. Il vecchio O_TRUNC era DISTRUTTIVO all'apertura:
liberava subito la catena di cluster e azzerava la dirent su disco (size 0,
first_cluster 0), POI scriveva il nuovo contenuto. Se una qualunque scrittura
successiva sul volume USB falliva — e `handle_close` IGNORAVA silenziosamente
l'esito di `fd_flush_write` e `dir_update_entry` — la dirent restava a 0 e il file
risultava vuoto. Il file nuovo (drag-and-drop) non passa dalla TRUNCATE distruttiva,
da qui la differenza.

Fix: O_TRUNC reso NON distruttivo / atomico, sullo stesso percorso del file nuovo
(che funziona):
- **`handle_open`** (O_TRUNC): NON libera piu' la catena ne' tocca la dirent. Salva
  la vecchia catena in `trunc_old_chain` e mette `first_cluster=0`, cosi' le scritture
  allocano una catena FRESCA (esattamente come creare un file nuovo). La dirent
  continua a puntare al contenuto ORIGINALE per tutta la modifica.
- **`handle_close`**: cattura l'esito del flush (`flush_ok`). Solo se il nuovo
  contenuto e' su disco ripunta la dirent alla nuova catena e POI libera la vecchia.
  Se qualcosa fallisce: lascia la dirent sulla vecchia catena (contenuto originale
  INTATTO) e orfana la nuova catena parziale. Un salvataggio vuoto (nessun dato
  scritto) ha `flush_ok=true` e `first_cluster=0`, quindi la dirent viene
  correttamente ripuntata a {0,0}. Il trim dei cluster pre-allocati ora avviene
  solo se il flush e' riuscito.

Risultato: in caso di fallimento di scrittura, al massimo "la modifica non si e'
salvata" (contenuto originale conservato), MAI piu' "file svuotato".

Fix collaterale: **`fat_alloc_batch`** ora invalida la cluster cache su OGNI cluster
allocato (numero riusato), non solo per i cluster azzerati/directory; il percorso
file (zero=false) lasciava copie stantie in cache su un numero di cluster riusato.

Nota: questo NON rende infallibili le scritture USB (se il device resta "busy"
oltre il budget di retry, o se la chiavetta ha una FAT corrotta dai vecchi build, la
modifica puo' non salvarsi); ma ora il contenuto originale non si perde. Per una
chiavetta corrotta resta consigliata la riformattazione (DobDisk o PC).

---

# MainDOB — Scrittura USB sulla versione pura: il NAK del device trattato come "busy" (attesa+retry), niente reset

La versione pura non wedge-a piu' (nessun reset-on-NAK), ma le scritture
FALLISCONO: una chiavetta flash NAK-a il CBW del comando successivo mentre
committa la scrittura precedente, e la versione pura fa UN solo `bot_xfer` senza
retry — quindi sul NAK la scrittura ritorna errore e basta. Il drag-and-drop e
il Salva non scrivono.

Fix minimale: riconoscere il NAK persistente come BUSY (non errore, non successo)
e ATTENDERE+RIPROVARE. Niente reset: un reset aborterebbe il commit in corso e
incaglierebbe il device nel NAK permanente (era il bug originale; la versione
pura giustamente non resetta, quindi non c'e' nulla da togliere, solo da
aggiungere l'attesa+retry). Il segnale "busy" attraversa i livelli:

- **usb_uhci `xfer_run`**: un TD che resta ACTIVE con `TD_STS_NAK` e nessun bit
  d'errore (un NAK non decrementa CERR) e' flow-control "busy" -> ritorna 2.
- **`uhci_bulk_xfer`**: rc==2 -> -4 (prima dell'accounting di successo).
- **handler `BULK_XFER`**: got==-4 -> reply `DOB_ERR_INTERNAL` con `arg1=1`
  (niente disconnect-probe, niente teardown).
- **usbms `hc_bulk`**: reply `DOB_ERR_INTERNAL && arg1==1` -> -4.
- **usbms `bot_xfer`** (incapsula tutto il busy, layer di trasporto giusto):
  - CBW NAK (-4): attende 100 ms e RI-INVIA lo stesso CBW, fino a
    `CBW_BUSY_RETRIES` (20 = ~2 s). Mai reset.
  - CSW NAK (-4): RI-LEGGE la CSW (mai un nuovo CBW: violerebbe BOT e
    incaglierebbe il device), fino a `CSW_BUSY_RETRIES`.
  - phase dati: invariata; un -4 qui (raro: 4 KB stanno nel buffer del device)
    cade nel ramo errore -> fallisce pulito, niente wedge.
- **`scsi_write10`**: torna banale; `bot_xfer` gestisce gia' il busy. Anche le
  LETTURE beneficiano del retry-su-busy.

Reply azzerato per richiesta (memset nel loop), quindi `arg1==1` distingue il
busy dal `DOB_ERR_INTERNAL` generico senza falsi positivi. Timeout bulk invariato
(3000 ms): un commit normale completa il TD ben prima, il path busy scatta solo
per commit lunghi.

Nota separata (non e' questo fix): salvare SOPRA un file preesistente fa una
TRUNCATE che libera la vecchia catena; se la chiavetta ha catene corrotte da
scritture interrotte dai vecchi build, quella free puo' essere lenta. Si risolve
riformattando la chiavetta (con DobDisk o un PC). Il percorso di scrittura in se'
— file nuovi, drag-and-drop — adesso funziona.

---

# MainDOB — Ghost-icon allo stacco: chiuso l'unico buco del modello event-driven (stacco nella finestra sveglia)

Sintomo: stacchi la pendrive e l'icona NON sparisce; cliccandola apre l'esplora-
file con la directory servita dalla cache (vedi i file), ma aprire un file dà
vuoto perche' il device non c'e' piu'. Il dato e' davvero sulla chiavetta — e
infatti il trascinamento di un file sulla chiavetta funziona: il percorso di
scrittura USB e' sano. Il problema e' solo il RILEVAMENTO dello stacco.

Il modello e' (giustamente) event-driven: in Global Suspend, plug/unplug e' un
evento Resume-Detect che alza un IRQ. Niente polling di PORTSC. Su questo
hardware funziona davvero (USBPIRQDEN/0x2000 e' impostato dall'handoff LEGSUP;
la diag mostra `LEGSUP 0x0030 -> 0x2000`), quindi uno stacco a controller
SOSPESO viene visto da RD.

Il buco: tra l'ultima attivita' e la sospensione il controller resta sveglio per
la finestra di idle (`DEVICE_IDLE_MS`, ~2x = ~4 s dopo l'ultimo transfer). Uno
stacco IN quella finestra e' SILENZIOSO: un controller running non alza IRQ sul
cambio porta, latcha solo CSC. Quando poi `uhci_idle_tick` sospende, arma RD su
un device GIA' assente — e RD non scattera' mai (il cambio e' gia' avvenuto):
icona fantasma per sempre, e un click serve la directory dalla cache. Con i 60 s
di un giro precedente la finestra era enorme (~2 min) e il problema era certo;
anche a 2 s puo' capitare staccando subito dopo il drag-and-drop.

Fix (event-driven, NON polling), in `uhci_idle_tick`:
- Prima di entrare in Global Suspend, una SINGOLA lettura del CCS della porta
  attiva. Se il device e' sparito (staccato nella finestra sveglia), si chiama
  `uhci_device_gone()` (teardown + ritiro dell'icona dall'hotplug) invece di
  sospendere un fantasma. E' un controllo di sanita' alla transizione di stato,
  non un poll periodico.
- Lo stacco a controller gia' sospeso resta gestito da RD, come prima.

Risultato: l'icona sparisce entro pochi secondi dallo stacco in tutti i casi.
`DEVICE_IDLE_MS` resta 2000.

Nota (non un bug di questo fix): durante la breve finestra prima del teardown,
la directory della chiavetta puo' ancora essere servita dalla cache a un click;
se serve, si puo' invalidare la cache su CSC. Separato.

---

# MainDOB — runner promosso a DRIVER: sys_make_driver esige caller->is_driver

Campo v1.5.35: "adesso non si apre proprio". Causa trovata nel kernel:
sys_make_driver rifiuta (-1) qualunque chiamante con is_driver=false.
Il runner era spawnato con spawn_file SEMPLICE -> non-driver -> il
PRIMO step dell'azione (spawn di usbms via spawn_file_driver, che fa
make_driver) moriva in silenzio. Runner vivo, azione morta al passo
uno, nessun popup.

- **hotplug**: il runner e' spawnato con spawn_file_DRIVER — hotplug e'
  driver, la catena di promozione regge: hotplug -> runner -> usbms.
  (La whitelist DAS era invece OK: e' per basename della home, e
  /SYSTEM/OperatingSystem/hotplug/ -> "hotplug" e' ammesso.)
- **Runner blindato**: debug_print a inizio/fine, popup espliciti se i
  DAS non si caricano (das_count()==0) o se l'indice e' fuori range —
  mai piu' fallimenti muti: o la finestra, o un popup che nomina il
  perche'.

Versioni -> 1.5.36.

---

# MainDOB — PLUG-AND-PLAY BY CONSTRUCTION: le azioni diventano bubble (action runner)

Riformulazione del bug dal progetto: non la singola tana, la CLASSE —
icone fisse/zombie, click che non producono nulla, "tutto meno il plug
and play". Causa sistemica identificata: IL MOTORE DELLE AZIONI GIRAVA
DENTRO HOTPLUG. Ogni doppio click parcheggiava l'intero processo per la
catena spawn -> WAIT_READY -> PREPARE_VOLUME -> wait_service ->
OPEN_VIEW; nel frattempo ATTACH/GONE si accodavano (icone bugiarde),
i click successivi morivano, e QUALUNQUE anello debole della catena
congelava il desktop. Ogni zombie visto sul campo, qualunque fosse la
foglia, passava da qui.

- **Action runner**: ogni attivazione (icona o menu) ora gira in un
  PROCESSO USA-E-GETTA — lo stesso binario hotplug rilanciato con
  --run-action <das_idx> <menu_idx> <ctx...> via spawn_file asincrona
  (argv supportato, contesto serializzato in 8 argomenti; gli indici
  DAS sono stabili entro il boot perche' das_load_all e' deterministico
  e das.c e' autocontenuto, zero extern). hotplug NON SI BLOCCA MAI
  PIU': icone veritiere per costruzione; un'azione incagliata costa un
  processo leakato + il suo popup, mai il desktop. Fallback inline se
  lo spawn del runner fallisce (OOM).
- E' la filosofia bubble del sistema applicata alle azioni: isolamento
  del dominio di guasto, esattamente come per i driver.

Versioni -> 1.5.35. Test: i soliti cicli sadici + click multipli
ravvicinati su icone diverse (ora possono correre in parallelo) +
strappo A META' azione: il desktop deve restare vivo e l'icona seguire
il filo in ogni scenario.

---

# MainDOB — il reinserimento ora arriva in fondo al bring-up (31 op); diagnostica decisiva su 66/mount

Foto v1.5.33 a "icona fantasma": LETTURA CORRETTA = PROGRESSO. Device
presente (0x0095), enum rifatta, FSM idle sano e 31 op servite: il
bring-up sul reinserimento ORA COMPLETA (devinfo+descriptor+INQUIRY+
TUR+READCAP ~ proprio quel conteggio). I fix del ciclo di vita
lavorano; il guasto si e' spostato a valle: op 66 (trova FAT32) o il
mount di dobfs_0, su un volume che montava. La riga di fase 4 contiene
gia' il verdetto (prepara-volume: ok / NESSUN FAT32 / lettura MBR
FALLITA) ma la foto la tagliava.

- **usbms**: contatore cnt_vread_fail (letture fallite post-bring-up)
  esposto nei bit 16..23 di arg2 dell'opcode 3.
- **usbdiag**: fase 4 ora chiude con ", letture KO: N".

Matrice di lettura per il prossimo giro (foto della riga fase 4 INTERA
+ testo esatto del popup):
- "prepara-volume: lettura MBR FALLITA" o letture KO > 0 -> le vread
  post-bring-up muoiono: trasporto, si torna in uhci.
- "prepara-volume: NESSUN volume FAT32" -> l'MBR si legge ma il
  contenuto non torna: sospetto dati corrotti/toggle.
- "prepara-volume: ok" + popup mount_failed -> dobfs_0 muore nel
  mount: si scava la' (console: "[DobFileSystem] secondary mount...").

Versioni -> 1.5.34.

---

# MainDOB — icona fantasma decodificata: strappo a I/O attivo = NESSUN evento; port-gate

Le tre foto a icona fantasma (v1.5.32) hanno chiuso il caso:
- foto 1: driver SANO, porte VUOTE (0x0080), FSM "Global Suspend",
  enum=No, 73 op servite -> c'era stata una sessione piena; l'icona
  pero' e' ancora viva: fantasma.
- foto 2-3: in certi istanti usb_uhci NON risponde nemmeno al GET_DIAG
  -> il driver si blocca per secondi e poi torna.

Fisica del problema: a controller ATTIVO (in transfer) lo strappo NON
genera alcun evento — l'RD esiste solo in suspend; resta solo un CSC
muto. La scoperta avviene per TIMEOUT dei TD: ogni chain consuma il suo
timeout pieno (uhci dentro xfer_run per secondi: foto 2-3), il BOT
riprova, usbms scala i retry per minuti, hotplug resta parcheggiato
sull'azione -> GONE in coda, icona orfana, click morti. (Verificato:
l'annuncio GONE era GIA' dob_ipc_post su entrambi i rami — nessun
triangolo di deadlock.)

- **usb_uhci, PORT GATE**: alle op 3/4, una lettura di PORTSC prima di
  lanciare TD: porta vuota (CCS=0) -> DOB_ERR_DEAD immediato, zero TD.
  La rilevazione dello strappo passa da secondi a una lettura I/O.
- **usbms, transport_dead**: i wrapper hc_ctrl/hc_bulk riconoscono
  DOB_ERR_DEAD (-3) e armano il flag; bot_xfer, la scala TUR e il
  retry di bring-up cortocircuitano: gone e' gone, niente martellate su
  un device che non c'e'. Il DETACH (gia' postato dal gone) chiude il
  processo come prima.

Effetto atteso sul campo: strappo durante I/O -> errori in millisecondi
-> usbms abortisce -> hotplug libero -> GONE processato -> icona via,
finestra chiusa, reinserimento pulito. Versioni -> 1.5.33.

---

# MainDOB — caccia allo zombie da reinserimento: due falle chiuse, FSM ora confessa

Campo v1.5.31: inserisci -> ok; togli -> icona sparisce (giusto);
reinserisci -> icona ricompare (giusto) ma e' ZOMBIE. Audit del ciclo
reinserimento, due falle strutturali chiuse in attesa della foto
diagnostica (gli stati FSM ora hanno nomi: la riga "Stato FSM" durante
lo zombie dira' il punto esatto):

- **usb_uhci**: l'RD differito (rd_deferred) che atterra DURANTE la
  dwell di wake o di debounce veniva ripreso solo al successivo punto
  di stabilizzazione — uno strappo a meta' sveglia poteva restare
  inghiottito. Ora uhci_process_deferred_rd() gira a fine
  WAKING_SETTLE (dopo il serve) e alla risoluzione del
  CONNECT_DEBOUNCE.
- **usbms, registrazione VERIFICATA e rumorosa**: dob_server_init
  inghiotte l'errore di registry; una collisione di nome silenziosa
  (istanza precedente mai morta) produce ESATTAMENTE il sintomo "icona
  che non fa nulla" — il das risolve la porta vecchia, il nuovo
  processo serve una porta che nessuno chiama. Ora: porta creata,
  registrazione manuale verificata, fallimento con log esplicito
  "[usbms] name X ALREADY TAKEN (stale instance alive?)" + exit 1 ->
  popup bounded del wait_service. Nuova primitiva libdob
  dob_server_adopt_port per il pattern.

Verifica dal registry kernel: kregistry_cleanup_pid alla morte del
processo ESISTE (process.c:227) — quindi se il nome risulta occupato,
il vecchio usbms e' VIVO da qualche parte (parcheggiato): il log nuovo
lo dira'. Versioni -> 1.5.32.

---

# MainDOB — costo per istanza: CPU zero by design, RAM lazy (8+4 KB), shared-text come ticket

Obiezione di progetto sul multi-processo (5 UHCI = boilerplate in RAM e
peso CPU). Risposta con numeri e tre mosse:

- **CPU: zero per le istanze inattive, ed e' il TUO design a garantirlo.**
  Un'istanza senza device dorme in dob_ipc_receive con il controller in
  EGSM: nessun timer armato, nessun polling, zero cicli a riposo. Il
  costo di scheduling esiste solo all'arrivo di un evento, ed e'
  per-evento, non per-processo.
- **RAM dinamica: resa lazy dove era eager.** Il pool TD+bounce
  (~16 KB) era GIA' lazy (primo transfer). Ora anche: pending_payload
  (8 KB, malloc al primo parcheggio — un controller senza device non
  parcheggia mai) e il frame list DMA (4 KB, ensure_frame_list alla
  prima enumerazione/transfer: un controller idle in EGSM non fetcha
  frame). Un'istanza senza device costa ora ~il testo del binario piu'
  l'overhead kernel (stack/page table), niente buffer.
- **Il testo duplicato (~35 KB x istanza) e' il boilerplate vero, e la
  cura giusta e' sistemica, non architetturale**: shared text nel
  loader ELF del kernel (stesso .mdl -> stesse pagine codice fisiche,
  dati per-processo). Beneficia TUTTO il sistema: dobfs secondari,
  DobFiles multipli, usbms multipli. Aperto come ticket kernel; il
  consolidamento in un processo unico comprerebbe gli stessi ~140 KB
  rinunciando all'isolamento delle bubble — esattamente cio' che questa
  sessione ha dimostrato prezioso (un PIIX4 incagliato non deve poter
  affondare altri quattro controller).

Versioni -> 1.5.31.

---

# MainDOB — EHCI gatekeeper multi-istanza: le porte 2.0 non bloccano nulla

Decisioni di architettura discusse e fissate:
- Le porte 2.0 restano "allo standard migliore" SENZA rinunce: il
  routing e' CONFIGFLAG, un bit reversibile. Oggi (niente stack EHCI) il
  gatekeeper fa handoff dal BIOS, reset e CONFIGFLAG=0: porte vive a
  full speed via companion UHCI. Quando lo stack EHCI arrivera',
  rialzera' il flag e riprendera' l'high-speed. Nel frattempo: non
  bloccano niente, per costruzione.
- Stratificazione chiarita: il controller driver NON e' solo watchdog —
  e' il TRASPORTO (possiede il filo: enumerazione e ogni transfer
  passano dal suo motore TD/QH); usbms e' il protocollo (BOT/SCSI),
  dobfs il contenuto. La scalabilita' multi-porta viene dal token
  opaco, non da un controller "leggero".
- "Un processo per tecnologia": UN DRIVER per tecnologia (gia' cosi'),
  ma N processi — uno per controller PCI. L'isolamento e' la filosofia
  bubble di hotplug: un controller incagliato non si porta via gli
  altri quattro. Il registry arbitra, usbdiag raggruppa.

Implementato:
- **usb_ehci (gia' esistente dalla fase 1 come gatekeeper: handoff
  USBLEGSUP via EECP bounded, reset, CONFIGFLAG=0)** portato agli
  standard di sessione: dob_server_init_unique (ICH8 ha DUE EHCI: da
  singleton il secondo moriva e le sue porte restavano inghiottite dal
  BIOS) + version stamp all'avvio.

Versioni -> 1.5.30.

---

# MainDOB — Extensa 5220: capitolo 1, UHCI multi-istanza (token opachi istanza|porta)

Piano compatibilita' Extensa 5220 (ICH8: CINQUE UHCI + due EHCI):
capitolo 1 implementato, 2 quasi gratis, 3 pianificato.

1. **UHCI multi-istanza**: nuova primitiva libdob
   dob_server_init_unique(base,...) — una porta, nomi base/base_1..7
   tentati in sequenza, IL KERNEL REGISTRY E' L'ARBITRO atomico (il
   vecchio dob_server_init inghiottiva l'errore di registrazione:
   scoperto e aggirato). Ogni controller vive come usb_uhci,
   usb_uhci_1, ... L'announce porta provider = nome reale e token =
   (instance_id << 4) | porta: ogni coppia (controller, porta) e'
   globalmente unica.
2. **Catena a valle**: il token e' OPACO — das ($provider:$token),
   usbms (usbms_<token>), dobfs (dobfs_<token>), DobDisk (--select
   usbms_<token>) lo usano gia' come suffisso senza interpretarlo:
   ZERO modifiche ai protocolli. Adeguati solo: il DETACH di uhci verso
   usbms (token codificato), lo scan di libdob/block (spazio token
   sparso 8x2 con salto dei buchi; etichetta "ctrl N porta M") e
   usbdiag (riga "Controller UHCI attivi: N (nomi)"; il dettaglio
   per-istanza e' la prossima iterazione dello strumento).
3. **Capitolo mancante — router EHCI (prossimo giro)**: su ICH8 il BIOS
   puo' lasciare CONFIGFLAG=1 (porte instradate all'EHCI): i companion
   UHCI vedono il vuoto. Serve un mini-driver usb_ehci di solo
   parcheggio: BAR MMIO, handoff legacy (USBLEGSUP via EECP), reset,
   CONFIGFLAG=0 -> tutte le porte ai companion. Senza, l'Extensa puo'
   mostrare 5 controller e nessun dispositivo.

Versioni -> 1.5.29.

---

# MainDOB — consolidamento robustezza ciclo di vita: FSM time-validated, zero busy-wait nel resume

Richiesta di progetto: robustezza a inserimenti/disinserimenti, race,
timing, pulizia degli stati, rimbalzi ed enumerazioni fantasma. Audit
sistematico e quattro interventi su usb_uhci:

1. **FSM time-validated**: i messaggi timer non hanno identita' — un
   one-shot stantio (es. l'idle tick da 2 s armato prima di un suspend)
   poteva sparare DENTRO uno stato di wake e accorciarne la permanenza
   (FGR tenuto 3 ms non sveglia nulla: fallimenti intermittenti da
   manuale). Ogni stato temporizzato registra il suo t0 (clock_ms) e il
   handler, se svegliato in anticipo, riarma il RESIDUO e ignora: i
   timer stantii diventano innocui per costruzione (timed_state /
   dwell_elapsed, slack 2 ms).
2. **Ispezione RD unificata nella FSM**: il ramo sincrono del resume in
   DEVICE_IDLE (leave_suspend busy + verdetti su bus intontito) e'
   sostituito dal wake a timer con wake_reason_rd: l'ispezione porte
   avviene a TRSMRCY completato, su bus sveglio. Esiti: CCS=0 -> gone;
   CSC con device presente -> ack + ST_DISCONNECT_CHECK (50 ms, timer:
   anche il DISTACCO rimbalza) -> ri-verifica; wiggle spurio -> ack,
   re-enable, ritorno in idle. ZERO busy-wait residui nel percorso
   resume.
3. **Nessun chiamante abbandonato**: uhci_device_gone fallisce
   (DOB_ERR_DEAD) qualunque op parcheggiata nel wake — un sync caller
   abbandonato e' il seme di ogni freeze cacciato finora.
4. Igiene: ST_DISCONNECT_CHECK come membro enum vero; nomi usbdiag per
   tutti gli stati nuovi; toggle map gia' azzerati a ogni device_ready
   (verificato: il replug ri-enumera e riparte pulito).

Versioni -> 1.5.28. Test sul campo: cicli inserisci/togli ripetuti,
anche rapidi e con finestra aperta; atteso: icona che segue il filo,
nessuno zombie, stati di passaggio visibili in usbdiag se fotografati
nell'attimo.

---

# MainDOB — LA VERBATIM VIVE SUL PIIX4 (fase 4: 7600 MB, 139 op bulk) + wake event-driven

DAL CAMPO: "usbms_0 attivo: 15564800 sett. virt. (7600 MB), blocco
nativo 512 B" — finestra DobFiles aperta col contenuto della chiavetta
su Armada E500. 139 op servite, ultimo opcode 4: BOT in produzione su
silicio. Il TRSMRCY era l'ultimo guardiano. Restano: (a) cicli
inserisci/togli ripetuti degradano a "zombie" (icona fissa); (b)
obiezione di progetto sulle attese fisse.

- **Wake event-driven (usb_uhci)**: le op 3/4 che trovano il controller
  in DEVICE_IDLE vengono PARCHEGGIATE (payload copiato: il buffer di
  ricezione viene riusato) e la sequenza di resume avanza a TIMER:
  ST_WAKING_FGR (25 ms) -> ST_WAKING_SETTLE (RS+TRSMRCY 15 ms) ->
  READY -> l'op parcheggiata viene servita e risposta. Stessa latenza
  per il chiamante (call sincrona comunque), ma il LOOP del driver
  resta libero: IRQ e diagnostica servite durante la sveglia. I tempi
  FGR/TRSMRCY restano (sono fisica del bus, non polling); e' il COME
  si aspetta che ora e' a eventi. Il leave_suspend sincrono resta solo
  nei rami di ispezione disconnect (rari e intrinsecamente sincroni).
- **Debounce del connect (ST_CONNECT_DEBOUNCE)**: i contatti veri
  RIMBALZANO — un inserimento fisico fa sfarfallare CCS per decine di
  ms e il driver bruciava enumerazioni su fantasmi, interlacciando
  enum/gone fino allo zombie. Ora il primo fronte di connect parcheggia
  100 ms (timer) e ri-verifica CCS prima di enum_begin.
- usbdiag: nomi per i tre stati nuovi.

Versioni -> 1.5.27.

---

# MainDOB — TRSMRCY: parlavamo alla Verbatim 1 ms dopo il resume; la spec ne concede 10

Tabella post-click v1.5.24: porta 0x0095 (il re-enable LAVORA, porta
abilitata), FRNUM avanzato (schedule viva), eppure "Op servite: 2
(ultimo opcode 3)" — il primo control transfer muore su porta sana.
Ultimo indiziato rimasto: il TIMING del resume. Da spec, terminato il
resume signalling il DEVICE ha diritto a 10 ms di recovery (TRSMRCY),
con i SOF che scorrono, prima di dover accettare traffico. Il nostro
leave_suspend lanciava il primo TD ~1 ms dopo RS=1: QEMU e' senza
tempo, la chiavetta vera era ancora intontita -> no response -> 3
errori -> halt.

- **usb_uhci, leave_suspend**: (1) fast-path se il bus NON e' sospeso
  (EGSM clear + RS set): niente ri-dance — un FGR su bus attivo inietta
  resume signalling nel traffico vivo; (2) FGR tenuto 25 ms (floor 20);
  (3) dopo RS=1, attesa TRSMRCY di 15 ms a schedule attiva prima di
  restituire il controllo al chiamante.

Questa build cumula i tre strati mai testati insieme sull'Armada:
re-enable porta (v1.5.24, provato: porta resta 0x0095), retry one-shot
del bring-up (v1.5.25), e il TRSMRCY (v1.5.26). Versioni -> 1.5.26.

---

# MainDOB — retry one-shot del bring-up (transitorio di guarigione porta)

Foto Armada v1.5.24 PRE-click (porta 0x0095 sana, recorder a 0): per il
verdetto servono le tre righe POST-click (recorder, fase 4, Porta 1) —
lo stadio puo' essere avanzato anche se l'esperienza utente e'
identica. Nel frattempo, mossa giusta in ogni scenario:

- **usbms**: il bring-up ritenta UNA volta, 500 ms dopo, se il primo
  giro fallisce. Sul silicio il primo transfer puo' pagare il
  transitorio della guarigione porta (il re-enable nel wake puo'
  atterrare un attimo dopo che il primo TD e' gia' morto); il secondo
  tentativo parte da porta stabile. Bounded per costruzione; su QEMU
  il primo tentativo passa e basta. USBMS_VERSION -> 1.5.25.

---

# MainDOB — INCHIODATO: il PIIX4 auto-disabilita la porta (CCS=1, PED=0)

La v1.5.23 ha tenuto il sistema in piedi e la diagnostica ha consegnato
la verita' in tre righe: "Op servite al sub-driver: 2 (ultimo opcode
3)" (devinfo ok, PRIMO transfer sul filo morto), fase 4 "bring-up
FALLITO allo stadio: ricerca endpoint bulk", e soprattutto Porta 1 =
0x0091: dispositivo presente, PORT ENABLE SPENTO. Al boot era 0x0095.

Causa: da spec UHCI le porte si AUTO-DISABILITANO su disconnect
percepito — e il wiggle da EGSM del PIIX4 (lo stesso che latcha i CSC
spuri) viene percepito come tale: PE cade. Il fix v1.5.22 teneva
giustamente il device ("spurious CSC, kept") ma non riabilitava la
porta: CCS=1, PED=0, ogni TD a morire di timeout. La WRMASK e'
innocente (preserva PE): e' l'hardware che lo spegne.

- **usb_uhci, uhci_port_reenable()**: se CCS=1 e PE=0, scrive PE,
  attende 10 ms, verifica. Un port-disable NON resetta il device
  (indirizzo e configurazione persistono): riabilitare basta, niente
  ri-enumerazione. Invocata nel ramo spurious-CSC e in
  uhci_device_wake PRIMA di servire transfer: il primo TD del
  sub-driver trova sempre un filo vivo.

Versioni -> 1.5.24. Pronostico: con porta sana, l'opcode 3 del config
descriptor passa (maxpacket gia' corretto dalla v1.5.20), endpoint
trovati, e si va a giocare con BOT/INQUIRY sul silicio per la prima
volta.

---

# MainDOB — il mount del secondario non puo' piu' congelare hotplug; usbdiag driver-first

Foto Armada v1.5.22: PROGRESSO MASCHERATO DA REGRESSIONE. Il popup e'
avanzato a "montaggio fallito" = il bring-up ORA PASSA (i fix
falso-GONE/replug hanno lavorato) e si arriva fino a dobfs_0. Ma il
mount del secondario si incaglia sul ferro, e la combinazione
registrazione-pre-mount + main sequenziale trasformava il parcheggio
in paralisi: wait_service passava subito (registrato!), l'OPEN_VIEW
successivo parcheggiava nella coda di un loop mai partito, e HOTPLUG
restava congelato per sempre — la seconda foto: usbdiag morto alla
fase 1 su HOTPLUG_LIST.

- **DobFileSystem**: i mount SECONDARI si registrano DOPO il mount
  (il root resta pre-mount per il boot ordering). wait_service
  dobfs_<id> ora significa "mount concluso" ed e' bounded dal suo
  timeout: mount incagliato = popup mount_failed in 8 s e hotplug
  LIBERO, mai piu' freeze. Su mount fallito il secondario esce senza
  registrarsi (un registrato rotto colleziona solo chiamanti
  parcheggiati).
- **usbdiag driver-first**: l'ordine di interrogazione era hotplug ->
  driver -> usbms; con hotplug congelato lo strumento moriva al passo
  uno (foto 2: due righe). Ora: tabella subito, driver (risponde
  sempre: flight recorder visibile!), usbms, hotplug PER ULTIMO con
  sentinella dedicata.

Il prossimo dato decisivo e' il flight recorder dopo il click: "Op
servite al sub-driver: N (ultimo opcode X)" + fase 4 — insieme
nominano il punto esatto dove il mount si incaglia sul PIIX4.
Versioni -> 1.5.23.

---

# MainDOB — le sentinelle hanno parlato: falso-GONE da CSC spurio e replug sordo (PIIX4)

Foto Armada con v1.5.21 (binari finalmente certi): fase 3 risponde
(uhci VIVO post-click), sentinella di fase 4 inchiodata (usbms
registrato ma mai entrato nel loop: incastrato nel bring-up — e' lui
che incatatonisce hotplug). Seconda foto: porta 0x0091 (presente, non
abilitata), FSM "In attesa", enumerazione No — replug IGNORATO.
Completam. 6 = boot (3) + UNA RI-ENUMERAZIONE (3). Ricostruzione:

1. **Falso GONE (usb_uhci)**: sul PIIX4 l'ingresso/uscita da EGSM fa
   wiggle sulla porta e latcha CSC (QEMU mai). Il ramo DEVICE_IDLE
   dichiarava gone su CCS=1+CSC=1: DETACH a usbms in pieno bring-up,
   ri-enumerazione, device de-configurato. Ora: CSC con device
   presente = ack + debounce 50 ms + ri-lettura; gone SOLO se CCS=0.
2. **Replug sordo (usb_uhci)**: il ramo "FSM busy" deferiva chiamando
   uhci_service_port, che ACKA i change bit: CSC del replug inghiottito
   -> nessun change pendente -> RD mai piu' rilanciato -> driver sordo
   con porta presente (la foto 2 esatta). Ora il defer NON tocca
   PORTSC (flag rd_deferred) e il rescan parte quando l'FSM si
   stabilizza (device ready o ritorno in idle post-gone).
3. **Flight recorder (uhci diag + usbdiag fase 3)**: contatore e ultimo
   opcode servito al sub-driver. La sequenza del bring-up e'
   deterministica: se usbms tace, questo numero nomina lo stadio della
   morte — leggibile dalla fase 3, che risponde sempre. Niente piu'
   bisogno della seriale sul portatile.

Versioni -> 1.5.22.

---

# MainDOB — usbdiag a pubblicazione viva: il freeze diventa leggibile

Dal campo Armada: "le tabelle fase 3 e 4 non compaiono mai, solo 1 e
2". Spiegazione: per design le tabelle persistenti sono DUE (bubble
"fase 2/3" + la principale con le righe 1/3/4) — ma la principale
veniva creata PER ULTIMA, dopo tutte le query. Con il driver
incatatonito, la query di fase 3 blocca usbdiag e la tabella piu'
importante non nasce mai: lo strumento diagnostico mostrava NIENTE
esattamente quando il sistema era piu' malato.

- **usbdiag ristrutturato**: tabella principale creata SUBITO dopo la
  fase 1 e righe pubblicate IMMEDIATAMENTE (DobTable supporta AddRows
  a finestra visibile: SetRows+draw_all). Prima di ogni query
  bloccante, una riga-sentinella: "Fase 3: interrogo usb_uhci (se
  resta qui: driver bloccato)" / idem fase 4. Se il driver e' muto,
  l'ultima riga sullo schermo E' la diagnosi.
  Retry tardivo dello spawn se quello precoce fallisce (pressione
  finestre/VRAM su adapter piccoli: il Rage Mobility non e' la BGA).

Versione usbdiag -> 1.5.21.

---

# MainDOB — il maxpacket hardcoded: il vero scoglio della Verbatim sul PIIX4

Dal campo Armada: anche con RS ripristinato (v1.5.19), bring-up fallito
e catatonia. Ipotesi binari stale a parte (verificare la riga versione:
deve dire 1.5.20!), il re-audit ha trovato un secondo difetto da
silicio, perfettamente mascherato da QEMU:

- **usb_uhci, ctrl engine**: i data TD del control transfer erano
  spezzati a 64 byte FISSI. L'enumerazione conosce bMaxPacketSize0
  (lo legge con la danza standard — per questo il boot funziona) ma il
  motore xfer lo ignorava. Su un EP0 da 8/16/32 byte (legale a full
  speed; QEMU usa 64 e maschera) il primo pacchetto corto ferma la
  catena a mps0 byte: il config descriptor torna <9 byte e il bring-up
  muore a "no bulk endpoint pair found" — lo stadio dell'Armada.
  Ora il chunking usa l'mps0 enumerato (floor 8 pre-descriptor).
- **Bulk, stesso principio**: l'op 4 ora porta in arg2 il max packet
  dell'endpoint (il flag finestra ritirato liberava il posto); usbms lo
  parsa dai descriptor EP (wMaxPacketSize, byte 4-5) e lo passa.
  uhci_bulk_xfer spezza su quello (clamp 8..64).

Versioni -> 1.5.20. Se sull'Armada il popup ricompare, ora servono DUE
foto: la riga "Versione usbdiag" (caccia ai binari stale) e la fase 4
DOPO il click — con l'abort rapido v1.5.19 le tabelle rispondono in
secondi e nominano lo stadio.

---

# MainDOB — ARMADA: fase 1 e hotplug OK su silicio; la "catatonia" era RS mai ripristinato

Prime foto dal PIIX4 vero (8086:7112): enumerazione della Verbatim
(18A5:0302) riuscita, FSM in idle/RD, contatori puliti (resume 1,
completam. 3, vicini 0 — linea IRQ dedicata), inserimento e
disinserimento funzionanti. Poi il primo click: bring-up fallito e
sistema apparentemente morto (icona inchiodata, usbdiag muto).

Causa, inchiodata dalle foto (CMD=0x0088: RS=0 + EGSM):

- **uhci_leave_suspend dimenticava l'ultimo passo della sequenza di
  resume: Run/Stop a 1.** enter_suspend azzera RS e nessuno lo
  rimetteva. QEMU esegue i TD comunque (permissivo); il PIIX4 fa cio'
  che dice la spec con RS=0: NIENTE. Boot ok (init_hw setta RS,
  l'enumerazione gira prima del primo suspend), primo click dopo
  l'idle -> schedule spenta -> ogni transfer scade il timeout.
- **La "catatonia" non era un hang**: era una muraglia di attese
  bounded accumulate — 1 s per xfer, TUR con 20 retry: ~45 s in cui
  uhci sta nei suoi busy-wait e non serve nessuno (usbdiag bloccato
  sulla query: tabelle vuote; hotplug bloccato nell'ipc_call: icona
  che resta dopo lo strappo). Si riavvia prima che finisca.

Fix:
- **usb_uhci**: leave_suspend ora chiude la sequenza da spec:
  FGR -> attesa -> clear FGR+EGSM -> **RS=1**.
- **usbms**: il retry TUR distingue st>0 (device in spin-up: insisti,
  e' il suo scopo) da st<0 (TRASPORTO morto: 2 fallimenti consecutivi
  -> abort). Mai piu' muri di 45 s; il peggio ora e' ~5 s con lo
  stadio nominato in fase 4.
- Version bump coerente ovunque (uhci/usbms/usbdiag -> 1.5.19; la riga
  di usbdiag era rimasta a 1.5.13).

---

# MainDOB — HOTPLUG COMPLETO SU QEMU (tutti i test passati) + sync cache pre-Armada

Verdetto dal campo, log alla mano: inserimento a runtime (resume IRQ ->
enumerazione -> icona -> mount -> editor) E rimozione (disconnected ->
GONE -> "[DobFileSystem] SHUTDOWN: flushing and exiting" -> "[usbms]
detach: exiting" -> finestra DobFiles chiusa da sola). La fase 2 in
emulazione e' CHIUSA. Due rifiniture prima del silicio:

- **usbms, INQUIRY print**: la sprintf di MainDOB non supporta %.Ns
  (il log mostrava "%.8s %.16s" letterali) — copie bounded e %s.
  Sull'Armada quella riga nominera' la Verbatim per esteso.
- **usbms, SYNCHRONIZE CACHE(10)**: emesso dopo ogni op WRITE. QEMU e'
  write-through e non se ne accorge; le chiavette vere bufferizzano, e
  senza sync uno strappo dopo un salvataggio puo' perdere dati gia'
  dichiarati salvati all'utente. Latch sync_cache_unsupported per i
  device che rispondono CHECK CONDITION: mai piu' richiesto.
  Deterministico, zero timer. USBMS_VERSION -> 1.5.18.

Prossima tappa dichiarata: masterizzare la ISO e portare questo stack
sull'Armada E500 — fase 1 gia' provata li'; fase 2 (transfer, BOT,
suspend/resume reali) al suo primo silicio. A taccuino restano: naming
multi-UHCI (Extensa 5220), ticket KHEAP double-free (kernel), API
page-grant per il futuro zero-copy.

---

# MainDOB — target `make run-hotplug`: collaudo inserimento/rimozione a runtime

Pipeline QEMU del progetto: make run (ISO+installazione), make run-usb
(disco installato + pendrive presente al boot), make run-disk (solo
disco). Mancava il caso che prova l'hotplug VERO: sistema su, porta
vuota, inserimento a runtime. Nuovo target:

- **run-hotplug**: boot dal disco installato con il controller UHCI e il
  BACKEND del drive definiti ma NESSUN usb-storage collegato. Dal
  monitor QEMU (Ctrl+Alt+2; su macOS Ctrl+Option+2):
      device_add usb-storage,id=pen,bus=uhci.0,drive=usbstick   inserisce
      device_del pen                                            rimuove
  Stessi id/bus di QEMU_USB (usbstick, uhci.0), senza -usb (che
  aggiungerebbe il secondo controller di macchina). Atteso: icona al
  connect (RD -> enumerazione -> announce); alla rimozione icona via
  (GONE), usbms esce (DETACH), mount flushato e chiuso (DOBFS_SHUTDOWN),
  finestre DobFiles chiuse (UNMOUNT_NOTIFY). device_add di nuovo:
  pipeline da zero.

---

# MainDOB — TRAGUARDO FASE 2 ("ADESSO funziona davvero") + unmount pulito alla rimozione

Confermato dal campo: Formatta -> DobDisk preselezionato -> partizione
FAT32 -> doppio click -> finestra DobFiles -> file creato, "hello
world!" salvato E riletto. Lettura e scrittura USB in produzione su
QEMU. Restavano le due domande giuste: hotplug completo (inserimento E
rimozione a runtime)? hardware vero? Questo rilascio chiude il buco
della rimozione (il dobfs orfano):

- **libdob/server**: nuova API dob_server_request_exit() — il handler
  marca l'uscita, il loop esce DOPO aver spedito la reply al chiamante
  corrente. Per server la cui ragione d'essere puo' sparire (media
  rimovibili).
- **DobFileSystem (mount secondari)**: implementati DOBFS_SUBSCRIBE_
  UNMOUNT (16: i satelliti DobFiles si abbonano, lato server prima
  esisteva solo nel cdrom) e il nuovo DOBFS_SHUTDOWN (22, rifiutato sul
  root mount): flush di tutti gli fd aperti (dati+dirent) e della FAT,
  UNMOUNT_NOTIFY (17) postato ai satelliti — DobFiles chiude la
  finestra — poi uscita pulita via request_exit. A medium gia' strappato
  i flush falliscono RUMOROSAMENTE (strumentazione v1.5.15): il log
  nomina i dati persi.
- **usb_mass_storage**: sul HOTPLUG_DETACH posta DOBFS_SHUTDOWN al
  proprio dobfs_<porta> prima di uscire — POSTATO, mai chiamato (il
  dobfs potrebbe essere bloccato in lettura proprio su di noi: la
  regola anti-ciclo v1.5.2 vale anche in discesa).

Catena di rimozione completa: strappo fisico -> errore transfer o RD ->
check PORTSC deferito -> GONE a hotplug (icona via) + DETACH a usbms ->
usbms posta SHUTDOWN a dobfs e esce -> dobfs flusha, avvisa DobFiles
(finestra chiusa), esce. Reinserimento: pipeline da zero, pulita.

---

# MainDOB — PRIMA SCRITTURA SU USB MANCATA PER UN SOLO ANELLO: trovato (doppio bug incastrato)

Pietra miliare dal campo: Formatta -> DobDisk preselezionato ->
partizione FAT32 creata -> doppio click -> FINESTRA DOBFILES APERTA.
L'intero read path USB (BOT, vista 512, MBR, mount, FAT32) e' in
produzione. Il neo: file creato dall'editor ma contenuto perso ("hello
world!" mai arrivato su disco). Caccia chiusa in un giro, ed era un
DOPPIO bug a incastro perfetto:

1. **DobFileSystem, disk_write_sectors**: controllava solo l'esito del
   TRASPORTO IPC; reply.code — il verdetto del handler — non veniva mai
   letto. Qualunque errore del provider diventava un successo
   silenzioso. (Il read path lo faceva giusto: validava il payload.)
2. **usb_mass_storage**: rifiutava richieste oltre 32 settori con
   DOB_ERR_INVALID — e fat_flush di dobfs scrive FAT_CACHE_SECTORS=128
   settori in UNA chiamata (e il read batcher arriva a 64 KB). La FAT
   della chiavetta veniva rifiutata dal provider e il rifiuto mascherato
   dal bug 1: file creato (dirent ok, scrittura piccola), catena FAT mai
   persistita, contenuto perso senza una riga di log.

Fix: dobfs valida reply.code (con print diagnostico lba/n in caso
d'errore); usbms accetta fino a 128 settori per richiesta (64 KB, il
tetto payload IPC — il contratto block deve essere uniforme con
ata/ahci) spezzando internamente sui chunk BOT da 4 KB; client_buf a
64 KB. In piu', strumentata TUTTA la catena di scrittura di dobfs che
falliva in silenzio: fd_flush_write (seek/write_cluster), la close
(flush dati e update dirent), fat_flush (entrambe le copie),
fd_ensure_clusters (allocazione). Mai piu' un "vuoto" senza un nome
sulla console. USBMS_VERSION -> 1.5.15.

NOTA per il test: la chiavetta attuale ha una FAT incoerente (dirent
scritte, catene mai persistite) — RIFORMATTARE con DobDisk prima di
riprovare il salvataggio.

---

# MainDOB — un byte: il GET_DESCRIPTOR(CONFIG) chiedeva wLength=0

Coi binari finalmente correnti, il log nomina lo stadio: devinfo passa
("[usbms] device 46f4:0001 ...") e il bring-up muore a STAGE_ENDPOINTS
("no bulk endpoint pair found"). Causa, da manuale: il setup packet di
discover_endpoints costruiva wLength con (uint8_t)sizeof(cfg) dove cfg
e' 256 byte — 256 troncato a uint8 = 0. La richiesta usciva come
wLength=0, il device rispondeva diligentemente con ZERO byte, il parser
non trovava endpoint. Fix: wLength=256 codificato nei due byte (low
0x00, high 0x01); il resto della catena (short packet a 32 byte, status
stage post-SPD della v1.5.12) e' gia' pronto a riceverlo.

Nota di igiene dal log: il run era pre-v1.5.13 (manca lo stamp di
versione e c'e' ancora il MMAP Denied del mapping rimosso) — i fix
v1.5.13 + questo viaggiano insieme da qui in poi.

---

# MainDOB — il log svela binari driver STALE (~v1.5.7) con DAS correnti; version stamp ovunque

I tabulati di console del campo contengono tre verita':

1. "[usbms] no device ready on controller" e il rifiuto del GET_DEVINFO
   sono testi/comportamenti che NON esistono piu' dal v1.5.8: i driver
   in esecuzione (usbms E usb_uhci) sono binari vecchi ~v1.5.7, mentre
   i popup arrivano dai DAS — file di testo, sempre ricopiati freschi.
   Driver stale + config nuova = sintomi incoerenti che hanno bruciato
   un giro intero di diagnosi. Mai piu' alla cieca:
   **version stamp**: usb_uhci e usbms stampano la versione all'avvio
   ("[uhci] v1.5.13 driver starting", "[usbms] v1.5.13 starting"),
   usbdiag la mostra in cima alla tabella. Ogni log e ogni screenshot
   dichiarano d'ora in poi cosa sta girando. CONSIGLIO OPERATIVO:
   rm -rf di BUILD_DIR (o make clean) prima della prossima build, per
   spazzare ogni .o/.mdl fossile.
2. "[MMAP] Denied: tried to map allocated RAM": il kernel rifiuta PER
   POLICY il mapping di RAM allocata di un altro processo — e fa bene.
   La finestra zero-copy via mmap_phys e' quindi architetturalmente
   vietata: rimosso il tentativo all'avvio di usbms (niente piu' deny
   a ogni spawn); resta il fallback a payload (la via supportata) e il
   plumbing per il giorno di una vera API di page-grant nel kernel.
3. A taccuino, non nostro: "[KHEAP] DOUBLE FREE caught ... caller=
   0xc010d2de -- ignored" ricorrente nel kernel heap — merita un
   ticket separato.

---

# MainDOB — il bring-up fallisce davvero (rendezvous funzionante); due difetti reali nel transport

Il popup bringup_failed dal menu Formatta conferma che l'op 67 fa il suo
mestiere E che il bring-up muore sul serio — il che rilegge anche il
giro precedente: la "preparazione volume" falliva per !bringup_ok, non
per immagine vergine. In attesa dello stadio dalla fase 4, il re-audit
del transport con questo dato ha trovato due difetti concreti,
ciascuno capace di uccidere il bring-up:

- **Finestra zero-copy non allineata (usb_uhci)**: mmap_phys mappa
  PAGINE; se il bounce DMA non e' page-aligned il sub-driver riceve un
  puntatore sfasato: CBW corrotti sul filo, ogni scambio BOT muore in
  modo misterioso (INQUIRY/TUR). Ora il bounce e' sovra-allocato di una
  pagina e allineato; GET_WINDOW riporta il phys allineato.
- **Status stage saltato dopo short packet (usb_uhci, ctrl engine)**:
  con dati IN piu' corti del richiesto (il config descriptor: 32 byte
  contro 256 — esattamente STAGE_ENDPOINTS), l'SPD ferma la coda PRIMA
  dello status TD: il control transfer resta senza handshake. QEMU puo'
  tollerare una volta, il silicio vero no. Se lo status TD non e' mai
  girato, viene rilanciato da solo (500 ms bounded).

Prossimo dato necessario dal campo: usbdiag fase 4 -> "bring-up FALLITO
allo stadio: <nome>". Con questi due fix lo stadio puo' anche
spostarsi o sparire; in entrambi i casi e' informazione.

---

# MainDOB — via il retry: rendezvous IPC deterministico (WAIT_READY, op 67)

Obiezione di progetto accolta: il retry di DobDisk (v1.5.10) era polling
camuffato. La soluzione event-driven era gia' nelle proprieta' del
sistema: il main di usbms e' SEQUENZIALE (registrazione -> bring-up ->
loop), quindi qualunque ipc_call verso di lui parcheggia nella coda e
viene servita esattamente quando il bring-up e' concluso. Il reply IPC
E' l'evento "dispositivo pronto" — deterministico per costruzione,
bounded dal bring-up stesso (retry TUR e timeout di trasferimento
finiti).

- **usbms**: opcode 67 WAIT_READY — puro rendezvous: DOB_OK se
  bringup_ok, errore altrimenti. Zero stato, zero attese interne.
- **DAS menu "Formatta..."**: passo `ipc_call usbms_$token 67` PRIMA
  dello spawn di DobDisk; tutte le attese restano nel motore action di
  hotplug, come per il click. Nuovo errore bringup_failed con rimando
  a usbdiag fase 4 (che nomina lo stadio).
- **DobDisk**: selezione --select tornata single-pass; rimossi i
  busy_wait. L'ordinamento e' garantito a monte by design.

Stessa garanzia, implicita, per il flusso click: l'ipc_call 66 viene
dopo wait_service e si sincronizza allo stesso modo col bring-up.

---

# MainDOB — "Formatta..." apriva DobDisk generico: era la gara col bring-up

Dal campo: la voce di menu spawna DobDisk ma la finestra e' generica
(nessuna preselezione USB). Verificato il motore DAS: gli argv extra
ARRIVANO al figlio (argv[1]="--select", argv[2]="usbms_N" — crt0 li
espone da argv[1], argv[0]=basename dal kernel). Il colpevole e' la
GARA: dal v1.5.8 usbms si registra PRIMA del bring-up, quindi il
wait_service del menu passa subito, DobDisk parte ed enumera mentre la
chiavetta sta ancora in INQUIRY/TUR — opcode 3 risponde capacita' 0 e
l'adapter la salta correttamente.

- **DobDisk**: quando --select punta a una usbms_N assente dalla prima
  enumerazione, ritenta load_disks() ogni 500 ms fino a ~6 s prima di
  ripiegare sulla finestra generica. Copre l'intera finestra di
  bring-up (TUR worst case 4 s) senza toccare il flusso senza flag.

---

# MainDOB — DobDisk vede le pendrive; "Formatta..." nel menu dell'icona (via DAS)

Dal campo QEMU: popup dell'op 66 ("Impossibile preparare il volume") =
usbms registrato e funzionante; l'immagine usb-storage di QEMU e'
verosimilmente vergine (zeri: niente MBR ne' BPB), quindi "nessun
volume" e' la risposta GIUSTA. Il blocco reale era a valle: il popup
manda a DobDisk per formattare, ma DobDisk non elencava i provider
usbms_* — l'unita' esisteva e nessuno strumento la vedeva.

- **libdob/block**: nuova driver class USB nel registro table-driven
  (il commento "BLOCK_BUS_USB will be 3" aspettava noi): enumerate
  interroga usbms_0/1 (opcode 3, salta bring-up incompleti), read/write
  instradati al servizio per-porta con chunking a 32 settori, rescan
  no-op (i volumi USB emergono al click, modello cdrom). DobDisk e
  qualunque futuro client ereditano le pendrive senza una riga.
- **DobDisk**: bus "USB" nel nome, match provider usbms_* per il
  boot-disk, e flag argv --select <provider> che preseleziona il disco.
- **DAS pendrive — menu "Formatta..."** (il suggerimento di progetto:
  il menu della barra e' programmabile dai DAS, stesso motore action):
  spawn guarded del sub-driver + wait + spawn di DobDisk con
  --select usbms_$token: si apre gia' puntato sulla chiavetta. Nota:
  lo spawn da DAS promuove a driver anche DobDisk (privilegio in
  eccesso ma innocuo); seconda istanza con DobDisk gia' aperto = caso
  limite non gestito.
- **usbms**: esito dell'ultima PREPARE_VOLUME latchato ed esposto in
  opcode 3 (arg2 bits 8..15): distingue "nessun FAT32" (formatta e
  riprova) da "lettura MBR fallita" (problema di trasporto, tutt'altra
  caccia). usbdiag fase 4 lo decodifica.

Flusso utente atteso ora: icona pendrive -> menu -> Formatta... ->
DobDisk preselezionato -> crea partizione FAT32 -> click sull'icona ->
finestra DobFiles. Resta il giallo "fase 4 vede usbms assente dopo il
popup": con il latch, la prossima tabella lo scioglie (se al prossimo
giro usbms risulta vivo con prepara-volume=NESSUN FAT32, era solo una
screenshot di un boot diverso).

---

# MainDOB — trovato il killer di usbms: GET_DEVINFO rifiutava lo stato DEVICE_IDLE

Il popup "Impossibile avviare il driver mass-storage" con .mdl presente
su disco aveva una sola causa possibile: morte PRIMA della
registrazione. L'unico passo in quella zona era GET_DEVINFO — e il suo
handler accettava solo ST_DEVICE_READY. Ma dal redesign event-driven il
controller parcheggia in ST_DEVICE_IDLE subito dopo l'enumerazione:
prima chiamata di usbms -> DOB_ERR_INVALID -> "no device ready" ->
exit 1 -> mai registrato -> timeout -> popup. Il wake era stato dato
alle op 3/4 e negato (giustamente: niente filo) alla 8, dimenticando
pero' di ACCETTARE lo stato idle.

- **usb_uhci**: GET_DEVINFO valido in entrambi gli stati post-enum
  (READY e IDLE), senza wake: la risposta viene dagli statics
  dell'enumerazione.
- **usb_mass_storage**: anche la chiamata GET_DEVINFO spostata DOPO la
  registrazione (stadio DEVINFO tracciato): qualunque fallimento ora
  lascia un processo interrogabile, mai un cadavere. Unica uscita
  pre-registrazione rimasta: host controller assente dal registry.
- **DAS**: popup riformulati in mono-riga — il parser rende "\n"
  letterale (visibile nello screenshot del campo); cosmetico ma
  sciatto.

---

# MainDOB — usbms vivo-ma-degradato: il bring-up fallito ora confessa lo stadio

Dal campo QEMU: .mdl presente su disco (27624 B: build e staging ok), lo
spawn parte, ma il click produce il popup "driver non parte" e la fase 4
vede usbms NON registrato: il bring-up muore e il processo usciva PRIMA
di registrarsi — un cadavere che non risponde a domande. Battesimo del
fuoco del codice BOT: serve sapere QUALE dei sei stadi cade.

- **usb_mass_storage**: registrazione del servizio PRIMA del bring-up;
  su fallimento il processo resta vivo in modalita' degradata (opcode
  dati 1/2/66 rifiutati con garbo, opcode 3 sempre servito). Stadi
  tracciati: GET_DEVINFO, endpoint bulk, SET_CONFIGURATION, INQUIRY,
  TEST UNIT READY, READ CAPACITY, online — esposti in opcode 3 (arg2;
  arg0=0 segnala bring-up incompleto).
- **usbdiag fase 4**: se arg0=0 mostra "vivo ma bring-up FALLITO allo
  stadio: <nome>" — la prossima screenshot identifica lo stadio senza
  pescare la seriale.
- **DAS**: popup del passo 66 riformulato (copre sia "nessun FAT32:
  formatta con DobDisk" sia "bring-up fallito: usbdiag fase 4").

Effetto collaterale benefico: con la registrazione anticipata, il
wait_service del DAS ora riesce subito e l'ipc_call 66 attende il
bring-up (bounded ~6 s worst case) invece di mangiarsi il timeout.

---

# MainDOB — usbdiag: verdetto bubble aggiornato e verifica del .mdl su disco

Dal giro QEMU v1.5.5: split IRQ confermato (5 nostri, 1397 del vicino di
linea), bubble #7 cl=01:06 = subdevice pendrive creato correttamente
(pid=0 atteso: il sub-driver nasce al click, non si attacca alla
bubble). Restava aperto: cosa accade al click, e il binario e' sul
supporto? Due rifiniture per rispondere da dentro:

- **usbdiag fase 2**: il verdetto "attach OK: il driver si incaglia
  DOPO" era il testo dell'epoca del primo debugging e oggi e' falso e
  fuorviante; ora distingue LIVE+registrato ("driver sano, dettagli in
  fase 3") da LIVE senza servizio (incagliato davvero).
- **usbdiag fase 4**: nuova riga "usbms.mdl su disco" — List (che salta
  la sandbox) di /SYSTEM/DRIVERS/usb_mass_storage con presenza e size
  del binario: distingue "build/staging falliti in silenzio" (la regola
  Makefile e' nuova; lo staging `if [ -f ]` salta zitto cio' che non
  esiste) da "binario presente, spawn da indagare".

---

# MainDOB — contatore IRQ sdoppiato: i ~1380 "IRQ fantasma" erano il vicino di linea

Dalla tabella QEMU (v1.5.3+): 1386 IRQ con 3 completamenti, 0 resume e
FRNUM fermo a ~1 s di running — quegli interrupt non potevano essere
nostri. Sono i wakeup della LINEA CONDIVISA (IRQ 10 in QEMU: il vicino
— verosimilmente il controller del disco di boot — interrompe, il
kernel sveglia anche noi, USBSTS=0). Benigni, ma il contatore unico
faceva sembrare una tempesta cio' che era traffico altrui.

- **usb_uhci**: il handler legge USBSTS per primo; se nessun bit e'
  nostro -> cnt_shared++, irq_done, continue (niente ack STS inutile,
  niente routing). cnt_irq ora conta solo interrupt NOSTRI.
- **Protocollo/usbdiag**: campo cnt_shared in coda a uhci_diag_t; riga
  "IRQ ricevuti: N (resume R, completam. C, vicini S)".

Stato del giro QEMU: FSM "Dispositivo pronto (idle, RD armato)" — il
ciclo di vita post-enumerazione lavora come da progetto. Fase 4:
"usbms non registrato". Ipotesi primaria: build v1.5.3, dove
usb_mass_storage.mdl NON era ancora nelle liste di staging (aggiunto
in v1.5.4): lo spawn dell'action fallisce con popup usbms_failed.
Da ritestare su build >= v1.5.4.

---

# MainDOB — flusso volume al pattern cdrom: un'icona, un click, una finestra (+ superfloppy)

Adottato fino in fondo il pattern dei driver floppy/cdrom indicato come
riferimento: niente icona-volume intermedia. Il click sull'icona
pendrive apre direttamente la finestra DobFiles.

- **usb_mass_storage**: rimossi announce/retract dei volumi. Nuova
  find_volume_lba(): prima partizione FAT32 dell'MBR, oppure euristica
  SUPERFLOPPY (jump EB/E9 + BPB plausibile + 0xAA55 a LBA 0 — copre le
  chiavette formattate senza tabella partizioni, gap dichiarato della
  v1.5.0). Nuovo opcode 66 PREPARE_VOLUME: individua il volume e spawna
  il DobFileSystem secondario legato a se' (--mount provider=usbms_N,
  lba=L,selector=0,id=N,fs=fat32) FIRE-AND-FORGET, rispondendo subito:
  attendere il mount dentro usbms sarebbe deadlock, perche' il mount
  legge i settori DA usbms stesso. Guard anti-duplicato all'avvio
  (ogni click esegue lo spawn; le istanze extra escono in silenzio,
  come cdrom). L'op 20 (rescan volumi) decade con gli announce.
- **DAS pendrive — catena completa in stile cdrom**, con TUTTE le
  attese in hotplug: spawn usbms -> wait usbms_$token -> ipc_call 66 ->
  wait dobfs_$token -> ipc_call 21 (OPEN_VIEW -> dobfiles_OpenMount,
  identico a partition_fat32.das). Errori distinti: usbms_failed
  (driver non parte), novolume (niente FAT32: suggerisce DobDisk),
  mount_failed (volume trovato ma mount fallito: rimanda a usbdiag
  fase 4).
- **Build/staging/install**: regola Makefile aggiornata (stub
  DobFileSystem per spawn_file, include eps); usb_mass_storage aggiunto
  alle liste di staging di mkbootdisk E mklive (mancava: il .mdl non
  finiva sul supporto); MainDOB_Setup lo copia INCONDIZIONATAMENTE
  (nessun DAS lo nomina con driver=, e' spawnato da un'action — stessa
  classe di bug della sottodirectory DAS/USB, stavolta prevenuta).

Audit anti-deadlock della catena: click -> hotplug spawn/wait (kernel)
-> usbms 66 (risponde senza attese; la lettura dell'MBR e lo spawn del
.mdl viaggiano solo VERSO IL BASSO: dobfs root -> ata) -> hotplug wait
dobfs (il mount legge da usbms, che e' libero) -> OPEN_VIEW (percorso
di produzione AHCI). Follow-up noti: unmount pulito del dobfs orfano
alla rimozione (pattern SUBSCRIBE_UNMOUNT del cdrom), DobDisk che
elenca i provider usbms_*, SYNCHRONIZE CACHE.

---

# MainDOB — QEMU promuove la pipeline driver; tre rifiniture dal primo giro del nuovo stack

Primo test del nuovo stack su QEMU (PIIX3 8086:7020, device 46F4:0001
usb-storage): Match DAS = USB Mass Storage, Annuncio = Si, Diagnosi
"Pipeline driver OK". La tabella stessa ha pero' denunciato due difetti:

- **Tempesta di IRQ nei trasferimenti pollati** (1374 IRQ per 3
  completamenti reali): il motore transport polla il completamento ma
  lasciava attiva la maschera USBINT/SHORT — ogni short packet dei bulk
  IN accodava un messaggio inutile. xfer_run ora azzera USBINTR per la
  durata del trasferimento e la ripristina alla fine: linea quieta,
  coda del loop pulita.
- **"Stato FSM: Sconosciuto"**: i nuovi stati non erano nella mappa.
  Protocollo: UHCI_ST_DEVICE_READY=6, UHCI_ST_DEVICE_IDLE=7 (ENUM_ERROR
  scala a 8); usbdiag li mostra come "Dispositivo pronto (attivo)" /
  "(idle, RD armato)".
- **usbdiag FASE 4**: interroga usbms_0/1 (opcode 3) e mostra capacita'
  in settori virtuali/MB e blocco nativo, o l'assenza del sub-driver
  (icona non cliccata / spawn fallito). La catena di diagnosi ora copre
  controller -> enumerazione -> DAS -> announce -> sub-driver.

---

# MainDOB — rotto il ciclo di deadlock hotplug<->catena USB (GONE postato, teardown deferito)

Dal campo: usbdiag congelato a FASE 1/3 — hotplug non risponde, e con
lui tutta la catena. Analisi: il flusso nuovo chiude un CERCHIO di
chiamate sincrone. Il motore action di hotplug (click sul volume) fa
ipc_call verso il dobfs secondario (OPEN_VIEW); il dobfs monta LEGGENDO
settori da usbms; usbms chiama usb_uhci; e se in quel momento un
transfer falliva, il nuovo uhci_check_disconnect_after_error eseguiva
uhci_device_gone INLINE — cioe' una dob_ipc_call sincrona VERSO hotplug
dall'interno del servizio di una richiesta: uhci aspetta hotplug,
hotplug aspetta dobfs, dobfs aspetta usbms, usbms aspetta uhci.
Permanente. Basta un singolo errore bulk al primo mount (probabile al
debutto del BOT) per innescarlo.

Regola architetturale che ne esce, da rispettare ovunque: UN PROVIDER
NON CHIAMA MAI SINCRONO VERSO L'ALTO mentre qualcuno sotto la stessa
catena puo' essere in attesa di lui. Le notifiche verso l'alto sono
POST (fire-and-forget) — e il loop di hotplug gia' dispatcha i messaggi
postati come canale di prima classe (ICON_ACTIVATED arriva cosi').

- **usb_uhci**: HOTPLUG_SUBDEVICE_GONE ora POSTATO (struct statica per
  sopravvivere al post), mai chiamato. Il rilevamento disconnect dopo
  un errore di transfer e' DEFERITO: flag + one-shot da 10 ms, il
  teardown gira dal loop eventi DOPO aver risposto l'errore al client,
  mai dallo stack frame di handle_request.
- **usb_uhci**: nel resume spurio in DEVICE_IDLE vengono ackati i
  change bit di TUTTE le porte: un CSC latchato sulla porta VUOTA
  avrebbe rigenerato RD all'infinito (storm di leave/enter-suspend).
- **usb_mass_storage**: anche i GONE di retract_volumes sono postati
  (stesso identico rischio di ciclo via dobfs).

Nota: il deadlock mascherava il guasto sottostante (il transfer
fallito che lo innescava). Col cerchio rotto, quel guasto tornera'
visibile e diagnosticabile: popup usbms_failed dopo il timeout, trail
[usbms] sul debug, e usbdiag di nuovo operativo.

---

# MainDOB — la lezione di cdrom/floppy: via il polling, provider-agnostico, zero-copy

Tre direttive di progetto, tre risposte.

**1. Polling eliminato (la lezione dei maestri).** cdrom e floppy non
pollano mai: probe-on-demand al click, disk-change verificato
all'accesso. Tradotto in USB: il suspend UHCI genera Resume-Detect
ANCHE sul disconnect, quindi il controller ora parcheggia il
dispositivo ENUMERATO in global suspend (nuovo stato DEVICE_IDLE):
rimozione a icona ferma = RD, puro evento, zero CPU. Il primo transfer
del sub-driver lo sveglia (DEVICE_READY, ~50 ms una tantum); durante
l'uso attivo la rimozione emerge come errore di trasferimento (evento),
verificato sul PORTSC dopo ogni op fallita; dopo 2 s senza transfer un
idle timer ONE-SHOT (attivo solo nello stato attivo, mai periodico)
riporta in suspend riarmando la copertura RD. La watch a 1 Hz della
v1.5.0 e' rimossa: non resta alcun polling, su nessuno stato.

**2. Un solo sub-driver per 1.x/2.x/3.x (la tabella provider di
cdrom).** Gli opcode 3/4/7/8/9 sono promossi a USB HOST TRANSPORT
CONTRACT, documentato nel protocollo: usb_ehci e usb_xhci dovranno
implementare le stesse semantiche, e usb_mass_storage — come il cdrom
con ata/ahci — riceve "$provider:$token" dal DAS e parla con QUALUNQUE
controller senza una riga di codice specifica. Quando arriveranno gli
stack 2.0/3.0, le pendrive ad alta velocita' useranno lo stesso .mdl.

**3. "Qualcosa di simile al DMA" — fatto dove serve davvero.** Il filo
e' GIA' bus-master DMA su ogni host controller; il collo erano le DUE
copie IPC controller<->sub-driver sul data path. Nuovo op GET_WINDOW(9):
il controller espone l'indirizzo FISICO del suo bounce DMA, il
sub-driver (e' un driver: mmap_phys) lo mappa una volta e passa arg2=1
sui BULK: dati OUT presi direttamente dalla finestra, dati IN lasciati
li'. Zero copie tra controller e sub-driver; fallback automatico ai
payload se il controller non implementa l'op. Su UHCI (tetto 1.1,
~1 MB/s) e' igiene architetturale; su EHCI/xHCI sara' la differenza
tra memcpy-bound e wire-bound.

---

# MainDOB — FASE 2 USB: sub-driver mass storage (BOT/SCSI), transport UHCI, hot-despawn, vista 512B su settori nativi variabili

La fase 1 e' chiusa dal campo: popup "Pendrive USB rilevata su usb_uhci,
porta 0" sull'Armada E500. La fase 2 costruisce la meta: icona volume in
tempo reale (inserimento E rimozione), click -> finestra DobFiles via
dobFS, compatibilita' con l'utility di formattazione, e supporto ai
tagli di settore nativi.

- **usb_uhci: politica DEVICE_READY.** Dopo l'enumerazione il controller
  resta RUNNING (niente suspend: fermerebbe lo schedule e ogni wake
  costa ~50 ms). Nuovo stato ST_DEVICE_READY con port-watch one-shot
  auto-riarmato a 1 Hz: l'UHCI non ha IRQ di port-change fuori dal
  global suspend (limite hardware), quindi la watch e' la concessione
  minima al polling, attiva SOLO a dispositivo presente; a porte vuote
  si torna al suspend+Resume-Detect puramente event-driven.
- **usb_uhci: hot-despawn.** La watch che vede CSC/!CCS: ack, GONE a
  hotplug (stesso token dell'APPEARED -> icona pendrive rimossa), post
  di HOTPLUG_DETACH al sub-driver usbms_<porta> (post, non call: se
  fosse a meta' trasferimento verso di noi una call sincrona sarebbe
  deadlock), reset stato e rientro in suspend.
- **usb_uhci: transport per sub-driver.** Ops nuove nel protocollo:
  CTRL_XFER(3) (SETUP+dati, direzione da bmRequestType), BULK_XFER(4)
  (fino a 8 KB, toggle per-endpoint mantenuti nel controller, STALL
  segnalato come DENIED), RESET_TOGGLE(7) (post CLEAR_FEATURE HALT),
  GET_DEVINFO(8). Motore sincrono su pool TD dedicato + bounce DMA,
  QH agganciato a tutte le 1024 frame entry per la durata del
  trasferimento, SPD per gli short packet (normali nel BOT).
- **NUOVO drivers/usb_mass_storage** (~550 righe): client del
  controller e server block-device. Bring-up: SET_CONFIGURATION,
  scoperta endpoint bulk dal config descriptor, INQUIRY, TEST UNIT
  READY con retry (spin-up), READ CAPACITY(10). **Tagli di settore**:
  accetta blocchi nativi 512/1024/2048/4096 ed espone SEMPRE una vista
  virtuale a settori di 512 byte (lettura: blocchi nativi coprenti +
  slice; scrittura: read-modify-write, saltato quando allineata) — il
  protocollo block esistente (code 1 read / code 2 write, identico a
  ata/ahci) resta invariato e DobFileSystem + DobDisk funzionano su
  qualunque chiavetta senza saperne nulla. Scan MBR via
  libdob/dob/partition: i volumi FAT32 vengono annunciati a hotplug e
  matchano il partition_fat32.das ESISTENTE -> icona volume, mount
  (DobFileSystem --mount provider=usbms_N) e vista DobFiles a riuso
  totale della pipeline AHCI. Su DETACH ritratta i volumi (GONE con i
  token dello scanner) ed esce.
- **DAS pendrive**: l'action del click ora spawna il sub-driver
  (--port $token) e attende usbms_$token; in caso di errore popup con
  rimando a usbdiag. Flusso utente: icona pendrive -> click -> icona
  volume -> click -> finestra file.
- **Makefile**: regola usb_mass_storage.mdl (linka partition.o come
  ata/ahci); staged in /SYSTEM/DRIVERS dal loop esistente.

Limiti dichiarati di questa tappa, da verificare/estendere sul ferro:
superfloppy (FAT senza MBR) non ancora annunciate; DobDisk non elenca
ancora i provider usbms_* nel suo selettore (il formato funziona gia'
puntandolo al servizio; l'enumerazione UI e' il prossimo ritocco);
write-cache/SYNCHRONIZE CACHE non emesso (le chiavette FAT su BOT ne
fanno generalmente a meno, ma va aggiunto prima di dichiarare solida
la scrittura).

---

# MainDOB — DAS_TEXT_MAX 2048 troncava mass_storage.das: direttive a byte 2699, lette zero regole

Dal campo: File DAS letti=1, Open fd=0, Read len=2047 — l'intera catena
FS finalmente verde, ma Match=Nessuno. Causa, misurata sul repo: i
USB-DAS sono per stile documentazione-prima (giustamente), e in
mass_storage.das (3704 B) la prima direttiva (usb_class = 0x08) sta a
byte 2699; il buffer del matcher (DAS_TEXT_MAX=2048) leggeva 2047 byte
di SOLI COMMENTI. Parser senza regole, match impossibile.

- **usb_das.c**: DAS_TEXT_MAX 2048 -> 8192, con il racconto del sintomo
  nel commento; warning su debug_print se un .das dovesse mai riempire
  di nuovo il buffer (troncamento = direttive potenzialmente perse,
  meglio una traccia che il silenzio).

Catena attesa ora completa fino all'announce: Read intera -> parse ->
match 08:06:50 (match_on=interface, esattamente la classe della
Verbatim) -> SUBDEVICE_APPEARED con subdev 01:06 -> hotplug -> icona.

---

# MainDOB — usb_das apriva senza FS_READ: Open ok, ogni Read negata. (E ora, l'icona?)

Verdetto dal live (file SANO, 3704 B scritti da mcopy): "Open fd=0,
Read len=0" — il driver apre il file ma legge zero byte. Il read path di
DobFileSystem e' innocente: handle_open accetta flags=0 e crea l'fd, ma
handle_read controlla (flags & O_READ) e NEGA ogni lettura su un fd
aperto cosi'. read_file_all del matcher USB apriva con dobfs_Open(path,
0); hotplug (das.c) ha sempre aperto con FS_READ — per questo i .das
top-level si leggono e quelli USB no. Stesso footgun nella Open di test
di usbdiag.

- **usb_das.c / usbdiag**: aperture con FS_READ. Commento nel codice con
  la firma del sintomo per i posteri.
- Nota API per il futuro (non cambiata ora): handle_open che accetta
  flags=0 e' una trappola silenziosa — l'fd nasce ma e' inerte. Da
  valutare: default a FS_READ o rifiuto esplicito di flags=0.

Con questa, la catena letta dal campo e' verde fino al parse: porta ->
reset -> enumerazione -> sandbox -> Open -> Read. Restano parse del .das
(testo mai letto finora, quindi mai esercitato sul ferro) -> match
(classe 08:06) -> announce -> hotplug subdevice -> icona.

---

# MainDOB — sandbox risolta (Open fd=0); il file su disco e' a 0 byte: il fossile della copia manuale

Verdetto dal campo: "Open/Read (driver): Open fd=0, Read len=0". La
correzione della sandbox FUNZIONA (il driver apre il file). La Read
restituisce 0 perche' la dirent ha file_size=0: la Read si ferma a
file_size, EOF immediato. Il file a 0 byte e' il residuo della copia
manuale fatta PRIMA della v1.4.17, quando la sandbox negava a DobFiles
(programma) ogni accesso a CONFIG: la copia era destinata a fallire a
meta', e il loop chunked di DobFiles tratta una sorgente che legge 0
come EOF silenzioso, lasciando una destinazione vuota senza errori.

Nota di design confermata: anche dopo la v1.4.17 i programmi NON possono
scrivere in /SYSTEM/CONFIG (giusto cosi'); il canale legittimo per
aggiungere DAS a un sistema installato e' un pacchetto .dbp via
DobInstaller (whitelistato), oppure l'installazione stessa (Setup
v1.4.14+ copia DAS/USB). La via maestra per il test dell'icona e' il
LIVE della build corrente: il file e' scritto da mcopy con size
corretta e il driver ora puo' leggerlo.

- **usbdiag**: la riga "List (usbdiag, ORA)" mostra la SIZE di ogni
  file; il verdetto distingue "file a 0 byte su disco" (rigenera il
  supporto / usa il live) da "file non vuoto ma Read vuota" (sarebbe un
  bug nel read path, da scavare separatamente).

---

# MainDOB — ROOT CAUSE FINALE della pipeline icona: la sandbox di DobFileSystem negava i DAS ai driver

Il doppio Open/Read dal campo (Armada): Open fd=-1 sia nel driver sia in
usbdiag, con List rc=0 che vede il file. Il file (copiato a mano) era
SANO: era la POLICY. In sandbox_check, /SYSTEM/CONFIG/ e' area riservata
controllata PRIMA del bypass driver, con whitelist per nome (config,
init, DobFileSystem, hotplug, dobinterface, DobInstaller, MainDOB_Setup).
hotplug legge i DAS top-level perche' whitelistato; usb_uhci — pur con
PRIV_DRIVER (make_driver da hotplug) — veniva respinto perche' il check
CONFIG precede il bypass; usbdiag idem (programma). La List "vede" il
file perche' il listing salta la sandbox per design: da qui l'asimmetria
che ha guidato la diagnosi.

- **Fix (minimo per design)**: in sandbox_check, eccezione SOLA LETTURA
  per i processi driver sul sottoalbero /SYSTEM/CONFIG/DAS/ — i driver
  host USB sono i consumatori designati dei DAS device-level
  all'enumerazione, lo stesso dato che hotplug legge per PCI. Le
  scritture su DAS e tutto il resto di CONFIG restano whitelisted-only.
- **usbdiag**: la propria Open su CONFIG continua correttamente a essere
  negata; la riga ora lo annota come atteso per non sviare la diagnosi.

Con questo cade l'ultimo blocco noto della catena: rilevamento ->
enumerazione (gia' verde sul ferro) -> lettura DAS (ora permessa) ->
match -> announce -> icona. Atteso al prossimo test: File DAS USB letti
>= 1, Match = mass_storage, Annuncio = Si, icona sul desktop.

---

# MainDOB — il cerchio si stringe: List OK ovunque, e' la LETTURA del .das a fallire nel driver

Verdetto dal campo (Armada, build v1.4.15):

    List (driver):       rc=0, entry nella dir: 1     <- dobfs FUNZIONA dal driver
    List (usbdiag, ORA): 1 entry, 1 file: mass_storage.das  <- file presente
    File DAS USB letti:  0                            <- ma il file viene scartato

Le teorie "file assenti" e "dobfs rotto nel contesto driver" sono morte
entrambe: la stessa List riesce dal processo driver. L'unica entry viene
scartata dai filtri del loop, e siccome il suffisso .das c'e' e usbdiag
(stesso filtro FS_TYPE_FILE) conta 1 file, il sospetto primario e'
read_file_all: il contatore das_files incrementa solo DOPO una lettura
riuscita — quindi e' la Open o la Read di quel file a fallire. Nota: la
Open di quel file non e' mai stata provata da NESSUN processo finora
(usbdiag listava soltanto); se il file e' stato copiato a mano con
DobFiles e' il percorso di SCRITTURA FAT32 ad averlo creato (LFN/8.3,
case), e potrebbe essere il file stesso il problema.

Strumentazione per l'ultimo split:

- **usb_das**: latch di Open (fd) e Read (len) dell'ultimo file tentato,
  esposti via GET_DIAG (das_open_fd / das_read_len; -100 = mai tentato).
- **usbdiag**: oltre alla List, ora APRE e LEGGE il primo .das dal
  proprio processo e mostra i primi 40 byte di contenuto (o quale delle
  due chiamate fallisce).
- **Diagnosi**: distingue "Open fallisce nel driver", "Open ok ma Read
  vuota", "scartato dai filtri tipo/nome", ciascuno incrociato con
  l'esito di usbdiag.

Esiti possibili e relative cure: Open fallisce per ENTRAMBI -> il file
creato dalla copia manuale e' difettoso (ricrearlo da script/installer,
e c'e' un bug nel write-path o nel lookup LFN di DobFileSystem da
aprire come filone separato); Open fallisce SOLO nel driver -> bug di
contesto da scavare nello stub; Read vuota -> file a lunghezza zero
(copia incompleta).

---

# MainDOB — usbdiag: doppia verita' sulla directory DAS/USB (driver vs adesso)

Il campo riporta ancora "File DAS USB letti: 0" dopo il fix
dell'installer. Quel contatore da solo non distingue TRE guasti diversi:
(a) file davvero assenti dal volume di boot, (b) dobfs_List che fallisce
dal contesto del processo DRIVER (usb_uhci e' il primo driver del sistema
a fare I/O su file: per AHCI il match dei subdevice lo esegue hotplug nel
proprio processo), (c) latch vecchio — i campi DAS si aggiornano solo a
un nuovo ENUM_DONE, quindi file copiati DOPO l'enumerazione di boot
risultano "assenti" finche' la pendrive non viene reinserita.

Strumentazione per separarli con una sola foto:

- **usb_das**: latch dell'esito grezzo della List (rc e numero di entry
  della directory, -99 = matcher mai eseguito) esposto via GET_DIAG
  (campi das_list_rc / das_dir_entries in coda a uhci_diag_t).
- **usbdiag**: nuova riga "List (driver)" con quei latch, e soprattutto
  "List (usbdiag, ORA)": la STESSA directory listata dal processo di
  usbdiag al momento dell'apertura, con conteggio e primi nomi file.
- **Diagnosi raffinata** dal confronto: assente ORA -> copiare i file;
  presente ORA ma rc driver < 0 -> bug dobfs lato driver; presente ORA
  ma latch a zero/mai eseguito -> reinserire la pendrive per
  ri-enumerare.

---

# MainDOB — PRIMA ENUMERAZIONE USB SU HARDWARE REALE; ultimo anello: l'installer non copiava DAS/USB

Verdetto dal Compaq Armada E500 (build con fix frame-list + ELCR):

    IRQ ricevuti: 3 (completamenti 2)      <- linea VIVA: era l'ELCR
    ELCR: 0x0800 -> IRQ 11 LEVEL           <- promozione driver-side ok
    Enumerazione completata: SI
    Dispositivo: 18A5:0302, classe if 08:06:50 (mass storage / BOT)

La pendrive viene enumerata da MainDOB su silicio PIIX4. Entrambe le
root cause del giro scorso confermate dal campo.

Anello residuo, auto-diagnosticato dalla tabella: "File DAS USB letti: 0"
-> match nullo -> niente announce -> niente icona. Causa: la macchina gira
da sistema INSTALLATO, e MainDOB_Setup non ha mai copiato la
sottodirectory /SYSTEM/CONFIG/DAS/USB — il passo DAS lista il livello
superiore e scarta le entry non-FILE (la directory USB e' invisibile), la
fase install crea solo DAS/ e copia i .das selezionati flat. I supporti
live/boot hanno i file (mklive/mkbootdisk li copiano), ogni sistema
installato no.

- **MainDOB_Setup, install_phase_config**: dopo la copia dei DAS
  selezionati, mkdir di /SYSTEM/CONFIG/DAS/USB sul target e copia
  INCONDIZIONATA di tutti i .das della sottodirectory (sono definizioni
  di classe device-level lette dai driver host USB all'enumerazione, non
  driver selezionabili). Avviso nel log d'installazione se il supporto
  sorgente ne e' privo.

Rimedi per i sistemi gia' installati (senza reinstallare): copiare a mano
i .das da DAS/USB del supporto live a /SYSTEM/CONFIG/DAS/USB del disco
con DobFiles, oppure testare direttamente dal live dove i file esistono.

Atteso al prossimo test: File DAS USB letti >= 1, Match DAS =
mass_storage, Annuncio a hotplug = Si, ICONA sul desktop.

---

# MainDOB — UHCI: il QH era agganciato a UN SOLO frame su 1024 (root cause del "transfer timeout"), ELCR promosso a level, forensics al fallimento

La prima tabella diagnostica completa dal Compaq Armada E500 (porta OK e
ABILITATA dopo il reset robusto, LEGSUP 0x0030->0x2000, FRNUM avanzato,
ma: IRQ=0, timeout=1, avanzati-senza-IRQ=0, "transfer timeout") ha
permesso di chiudere il cerchio su DUE root cause indipendenti.

- **ROOT CAUSE A — schedule: frame_list[0] soltanto.** ctrl_submit
  agganciava il QH di controllo alla sola entry 0. Il controller esegue
  frame_list[FRNUM & 0x3FF], una entry al millisecondo: il QH veniva
  visitato UNA volta ogni 1024 ms, mentre il timeout di trasferimento
  scatta a 100 ms. Esito deterministico sul silicio: TD mai eseguito
  (ACTIVE, CERR intatto, ActLen vuoto) -> "transfer timeout" sempre.
  QEMU mascherava il difetto: il suo UHCI fa caching delle queue
  scoperte e le serve attraverso i frame. Fix: QH agganciato a TUTTE le
  1024 entry (e sgancio totale in ctrl_unlink); con LP_DEPTH gia'
  presente sui link TD->TD la catena completa in una visita, ~1 ms.
- **ROOT CAUSE B (probabile, per IRQ=0) — ELCR mai toccato per le linee
  preassegnate dal BIOS.** elcr_set_level esiste nel kernel ma viene
  chiamato solo da pirq_wire_device, cioe' solo nel RICABLAGGIO. La
  linea che il BIOS ha gia' instradato (qui IRQ 11) viene usata as-is e
  se il BIOS l'ha lasciata edge il PIC non vede mai un fronte da un
  segnale level: cnt_irq=0 con PIRQDEN corretto. Fix driver-side: dopo
  irq_register, promozione idempotente della linea a level nell'ELCR
  (mai per le linee di sistema 0,1,2,8,13).
- **Forensics al fallimento.** enter_suspend acka USBSTS distruggendo le
  prove: ora enum_fail fotografa USBSTS (HSE/HCPE!), FRNUM, PORTSC e lo
  status raw del TD atteso PRIMA del teardown; il protocollo GET_DIAG li
  espone (+ ELCR) e usbdiag li decodifica con una Diagnosi raffinata
  (mai eseguito vs dispositivo che risponde male vs HCPE/HSE).

Aspettativa sul prossimo test Armada: enumerazione completata in pochi
ms anche a IRQ muto (fallback timeout), icona se DAS/announce ok; con
l'ELCR promosso, cnt_irq>0 e hot-plug event-driven funzionante.

---

# MainDOB — usbdiag a fasi: chi blocca chi?

Anche con la porta server unificata (v1.4.11) il campo riporta lo stesso
sintomo: usbdiag congelato su "Interrogazione in corso". Significa che il
driver si incaglia PRIMA del loop eventi. Analisi esaustiva del percorso
pre-loop: dob_registry_wait("hotplug", 5000) e' bounded, ogni chiamata in
uhci_init_hw e' una syscall bounded — l'UNICA chiamata che puo' bloccare
per sempre e' dob_driver_attach, sincrona verso hotplug. I sospetti sono
quindi due: hotplug incagliato (e con lui ogni driver che gli parla), o
qualcosa che la sola analisi statica non vede.

usbdiag diventa percio' un'indagine a fasi con breadcrumb a video, che
non richiede alcuna collaborazione del driver:

- FASE 1/3: HOTPLUG_LIST verso hotplug. Se la finestra resta su questa
  fase -> e' HOTPLUG il processo bloccato (ed e' lui che blocca il
  driver in attach).
- FASE 2/3: tabella di TUTTE le bubble (vendor:device, classe, pid,
  stato) piu' una riga VERDETTO sulla bubble USB (classe 0C:03):
  nessuna bubble -> hotplug non l'ha mai matchata; ATTACHING -> driver
  fermo nell'handshake READY; LIVE -> attach completato, l'incaglio e'
  dopo (init_hw o loop). Questa tabella RESTA a schermo qualunque cosa
  accada dopo.
- FASE 3/3: GET_DIAG come prima. Se congela qui, il verdetto della fase
  2 e' gia' visibile sopra.

Qualunque fotografia del prossimo test sull'Armada identifica
univocamente il processo e la fase colpevoli.

---

# MainDOB — usb_uhci: la porta server non veniva mai letta (usbdiag congelato ovunque, DETACH perso)

Il verdetto dal campo (Armada E500): usbdiag si avvia, mostra la finestra
di stato e resta su "Interrogazione in corso" — driver registrato ma muto.
La caccia al punto d'incaglio ha trovato un difetto architetturale, non un
difetto da silicio: NON sarebbe mai potuto funzionare nemmeno su QEMU.

- **Due porte, una sola letta.** dob_server_init("usb_uhci") crea e
  registra la porta SERVER: e' li' che atterrano le chiamate dei client
  (GET_DIAG di usbdiag, il DETACH di hotplug, qualunque sub-driver
  futuro). Ma uhci_init_hw creava una SECONDA porta privata
  (irq_port = port_create()) per IRQ e timer, e il loop eventi riceveva
  SOLO da quella. handle_request() era irraggiungibile: ogni
  dob_ipc_call di un client si bloccava per sempre. La pipeline pendrive
  su QEMU funzionava perche' e' guidata interamente da IRQ/timer, che
  arrivavano sulla porta giusta — per questo il difetto e' rimasto
  invisibile. Il driver ATA, al contrario, serve la sua porta server nel
  loop principale.
- **Fix: una porta per tutto.** Il driver UHCI e' event-driven puro,
  quindi irq_port = dob_server_get_port(): IRQ (type 3), timer (code 70)
  e richieste sincrone (type 1) condividono la porta registrata; il
  dispatch del loop li distingueva gia' correttamente.
- **DETACH gestito.** Conseguenza della stessa unificazione: il DETACH di
  hotplug ora arriva davvero al driver. Il loop lo gestisce
  esplicitamente: quiesce del controller (RS=0, USBINTR=0, niente DMA/IRQ
  verso un processo morto), reply, _exit(0). Prima veniva perso in
  silenzio sulla porta non letta.

Con questo fix usbdiag deve riempire la tabella su QUALSIASI macchina.
Sull'Armada il prossimo test torna a essere decisivo: la riga "Diagnosi"
e i contatori (IRQ ricevuti, LEGSUP, stato FSM, ultimo errore) diranno
quale dei sospetti silicio rimasti e' quello vero.

---

# MainDOB — UHCI su silicio: igiene PORTSC (bit RD), resume rientrante, usbdiag a prova di driver muto

Continuazione della caccia al "silenzio totale" sull'Armada E500 (UHCI
puro): tre difetti che QEMU non puo' mostrare, piu' un cambio di forma a
usbdiag motivato dal suo stesso sintomo.

- **usb_uhci: il bit Resume Detect veniva riscritto a 1 in ogni RMW di
  PORTSC.** Bit 6 e' R/W: l'hardware lo setta quando rileva resume in
  suspend, ma il SOFTWARE che lo scrive a 1 PILOTA resume signaling sulla
  porta. Dopo un vero resume il bit si legge 1, e ogni pattern
  `io_inw | ...` lo riscriveva: K-state forzato su una porta in funzione,
  transfer di enumerazione corrotti, Resume Detect rigenerato a catena.
  QEMU non setta mai quel bit, percio' il difetto era invisibile li'. Ogni
  scrittura PORTSC ora parte da una lettura mascherata con
  UHCI_PORT_WRMASK (via CSC/PEC, che sono R/WC, e via RD).
- **usb_uhci: Resume Detect rientrante troncava l'enumerazione.** Sul
  silicio un secondo RD arriva di routine mentre la FSM e' a meta'
  enumerazione (si ri-latcha attorno alla sequenza FGR). Il vecchio
  percorso faceva leave_suspend + "niente di nuovo" + enter_suspend MENTRE
  un reset/transfer era in volo: controller fermato, FSM forzata a IDLE,
  porta abbandonata con RESET asserito — porta congelata, driver
  apparentemente in perfetto idle. Ora un RD con FSM occupata acka i
  change bit e lascia finire la macchina a stati. Limite noto: un connect
  sull'ALTRA porta avvenuto durante una FSM occupata viene ackato e perso
  fino all'evento successivo.
- **usbdiag: finestra di stato PRIMA della GET_DIAG.** L'IPC sincrona non
  ha timeout: se il driver e' registrato ma incagliato fuori dalla sua
  receive (sub-chiamata bloccata, tempesta di IRQ, ...), usbdiag restava
  appeso senza mostrare NULLA — ne' tabella ne' popup. Che e' esattamente
  il sintomo riportato dal campo, e che da solo gia' distingue i casi:
  driver assente/morto -> popup d'errore (il registry viene ripulito alla
  morte del processo); driver vivo ma muto -> niente. Ora usbdiag apre
  subito una DobTable di stato ("Interrogazione in corso... / Se resta
  qui: il driver e' registrato ma NON risponde"), poi chiama; a risposta
  arrivata la chiude e mostra la tabella vera. Un driver appeso produce
  cosi' un verdetto leggibile a video invece del nulla.

---

# MainDOB — USB su ferro vero, atto secondo: EHCI gatekeeper (CONFIGFLAG), maschera IRQ hot-plug, debounce del connect al boot

Tre difetti che spiegano il silenzio totale della pendrive sulle macchine
fisiche; nessuno è visibile su QEMU.

- **usb_ehci: da scheletro pericoloso a gatekeeper.** Sull'Acer Extensa
  5220 (ICH8M: EHCI 2836/283A + companion UHCI 2830..2835) il driver EHCI
  scriveva `CONFIGFLAG=1`, instradando OGNI porta a un controller privo di
  stack di trasferimento: la pendrive arrivava all'EHCI e moriva lì, gli
  UHCI non vedevano mai il connect. In più programmava PERIODICLISTBASE/
  ASYNCLISTADDR con indirizzi *virtuali* (DMA su memoria fisica casuale,
  catastrofico su ferro) e non faceva alcun handoff BIOS. Riscritto come
  gatekeeper: handoff USBLEGSUP via catena EECP (semaforo OS, attesa fino
  a 1 s del rilascio BIOS, force-clear su timeout, SMI legacy azzerati su
  USBLEGCTLSTS — stessa sequenza di ehci_bios_handoff di Linux), halt +
  HCRESET, `USBINTR=0`, `CONFIGFLAG=0`: tutte le porte passano ai
  companion UHCI, dove vive l'intera pipeline rilevamento → enumerazione
  → DAS → annuncio. Niente schedule, niente bus master, niente IRQ. I
  dispositivi lavorano a full speed finché non esisterà un vero stack
  EHCI; per icona + mass storage è pienamente funzionale. Anche il BIOS
  da solo lascia spesso CONFIGFLAG=1 (legacy boot), quindi il gatekeeper
  serve pure dove il vecchio driver non girava.
- **usb_uhci: completion IRQ mascherati nel percorso hot-plug.**
  `uhci_enter_suspend` restringe USBINTR al solo Resume; `leave_suspend`
  non lo riallargava mai. Risultato: ogni enumerazione a caldo avanzava
  SOLO col fallback dei timeout (100 ms a passo), e su usbdiag il quadro
  (cnt_complete=0, noirq_adv>0) si travestiva da linea IRQ morta anche a
  linea viva. Ora `leave_suspend` programma RESUME|IOC|SHORT|TIMEOUT e il
  percorso boot-present include anche RESUME.
- **usb_uhci: debounce del connect al boot.** Il seed di port_present
  campionava PORTSC una sola volta, subito dopo GRESET+HCRESET: su QEMU
  CCS è istantaneo, sul silicio si ri-latcha con ritardo. Una pendrive
  presente dall'accensione poteva risultare "assente" — e quel miss era
  irrecuperabile sulle macchine il cui resume IRQ non arriva. Ora il
  campionamento dura fino a 100 ms (10×10 ms, uscita anticipata al primo
  CCS): dwell una-tantum di bring-up, non polling. Nel percorso senza
  dispositivo i change bit alzati dai reset vengono ackati prima di
  entrare in suspend.

Limite noto (invariato, ora documentato): hotplug spawna un usb_uhci per
OGNI funzione UHCI ma il registry è singleton — sull'Extensa (5 companion)
vive solo il primo registrato e la porta giusta è una lotteria. Serve il
naming per-istanza (usb_uhci_N); prossimo passo dopo la verifica su ferro.

---

# MainDOB — USB: occhi sul ferro (usbdiag + GET_DIAG) e reset di porta a prova di silicio vero

Test sul Compaq Armada E500 (live CD, pendrive sia al boot sia a caldo):
nessuna icona, nessuna informazione — il trail debug_print() del driver
finisce su VGA testo, invisibile appena dobinterface prende lo schermo.
Stessa lezione già imparata con ATA: i fatti vanno latchati nel driver e
letti a video. Tre interventi:

- **usb_uhci: snapshot diagnostico (opcode 6, GET_DIAG).** Nuovo header
  condiviso `libdob/include/dob/usb_uhci_protocol.h` con `uhci_diag_t`:
  esito init, io_base/IRQ, vendor:device, LEGSUP prima del handoff e
  letto live, registri controller (CMD/STS/INTR/FRNUM/PORTSC1-2),
  contatori eventi (IRQ totali, resume, completamenti, timeout,
  avanzamenti senza IRQ), stato FSM, ultimo errore di enumerazione
  (latchato in enum_fail), esito ENUM DONE (VID:PID + tripla classe),
  numero di file DAS USB letti dal matcher (0 = staging rotto), esito
  match DAS + label, esito announce a hotplug. `usb_das.c` espone
  `usb_das_last_file_count()` per il check di staging.
- **programs/usbdiag.** Clone di atadiag: interroga "usb_uhci", mostra lo
  snapshot in una DobTable e chiude con una riga "Diagnosi" che indica il
  punto esatto della pipeline dove si è fermato tutto (servizio assente →
  match hotplug; CCS=0 → elettrico; IRQ=0 → handoff/PIRQ; errore enum →
  reset/trasferimento; DAS=0 → staging; announce ok ma niente icona → il
  bug è a valle, in hotplug/dobinterface). Rilanciarlo dopo un re-insert
  dà uno snapshot fresco.
- **usb_uhci: reset di porta irrobustito per hardware reale.** Tre punti
  dove QEMU perdona e il silicio no: (1) il primo write di Port Enable
  dopo il reset viene spesso ignorato dal PIIX finché CSC/PEC sono
  latched — ora PE viene scritto con retry (fino a 5, ackando i change
  bit tra i tentativi, come uhci-hub di Linux), e un PE che non si
  aggancia è un errore esplicito ("port refused enable after reset")
  invece di un SET_ADDRESS che muore in timeout; (2) recovery TRSTRCY di
  10 ms dopo il reset prima del primo SETUP (USB 2.0 §7.1.7.3) — i flash
  drive veri ignorano il traffico nella finestra di recovery; (3)
  recovery di 2 ms dopo SET_ADDRESS prima del primo GET_DESCRIPTOR al
  nuovo indirizzo (§9.2.6.3).

---

# MainDOB — USB UHCI on real hardware (BIOS handoff, IRQ-less enumeration fallback)

Hot-inserting a pendrive on physical UHCI machines did nothing, while the
same build detected + enumerated + DAS-matched on QEMU. Two changes, both
in `drivers/usb_uhci/main.c`. Detection stays purely event-driven (Global
Suspend + Resume Detect IRQ — hotplug is intrinsic to USB); no PORTSC
polling was added.

- **BIOS owned the controller (SMM legacy keyboard emulation).** The driver
  never touched PCI LEGSUP (0xC0): port 60h/64h traps kept raising SMIs and
  the BIOS drove the controller behind our back; our IRQs were stolen. QEMU
  has no SMM BIOS, hence the works-on-QEMU/dead-on-metal split. Now, before
  the controller reset, LEGSUP is written with 0x8F00 (ack all R/WC trap
  status, clear every SMI/trap enable — SMM lets go); after HCRESET it is
  written with 0x2000 (USBPIRQDEN only). The PIRQDEN write matters on its
  own: on PIIX, without it the controller's interrupt is never routed to
  PIRQ at all — no resume IRQ, no completion IRQ, driver sleeps forever.
  Same sequence as Linux `uhci_check_and_reset_hc`. LEGSUP is 16-bit but
  SYS_PCI_WRITE is dword-only, so both writes RMW 32 bits preserving the
  reserved upper half. The pre-handoff LEGSUP value is logged.
- **Enumeration relied solely on completion IRQs.** If a completion IRQ is
  lost, the FSM stalled in *_SENT and the timeout killed the attempt. The
  (pre-existing, one-shot) timeout handler now inspects the TD chain before
  failing: if the transfer in fact completed on the wire, it advances the
  FSM exactly as the IRQ handler would (one ENUM_TIMEOUT_MS per step). With
  working IRQs nothing changes; lost IRQs cost latency instead of the
  device.

---

# MainDOB — bugfix pass (textbox selection, UTF-8 labels, tray clicks, ACPI S5 shutdown, .DBP .setting/.mem, reboot menu, module-manager uninstall)

Maintenance pass on top of v1.0.0.420.60 (build tag glyphclip/ringfix).
Four user-reported defects fixed; no version bump, no feature changes.

- **Single-line textbox ate the first character.** A plain click left a
  selection anchor at the click position (`dobtb_OnClick`); the first
  keystroke moved the cursor while the anchor stayed put, producing a
  1-char selection that the next key deleted — typing "apple" gave
  "pple". The anchor is now established lazily on the first drag move, so
  a plain click never leaves a dangling anchor, independent of whether
  its release is delivered. Same fix on the multi-line box.
  (`libdobui/textbox.c`)
- **Accents blank in labels and system UI (textboxes were fine).** The
  font and keyboard layouts are Latin-1, but C source string literals are
  saved UTF-8, so an accented byte reached the rasterizer as a multibyte
  sequence and indexed empty glyph slots. Added `dob_font_decode()`
  (UTF-8 → codepoint → Latin-1 glyph index, with a Latin-1 fallback that
  keeps typed/Latin-1 textbox bytes rendering as before) and routed all
  three compositor text rasterizers through it.
  (`libdob/include/dob/font.h`, `boot/dobinterface/main.c`)
- **Tray flyout clicks leaked to the window below.** The widget-panel
  flyout (`<`) is drawn over the window area; the focused-window
  mouse-forward guards checked `!mc_active` but not `!widget_panel_open`,
  so a click on a tray tool was also delivered to the program underneath.
  Added `!widget_panel_open` to the press/release/drag forward guards.
  (`boot/dobinterface/main.c`)
- **Shutdown froze instead of powering off (QEMU q35 and real hardware).**
  The old port-poke poweroff never triggered S5 on ICH9/q35 or on real
  machines, so the kernel fell through to an HLT park. `SYS_SHUTDOWN` now
  performs a real ACPI S5: the ACPI reader parses the FADT (PM1a/PM1b_CNT,
  SMI_CMD, ACPI_ENABLE) and scans the DSDT `\_S5` object for the
  sleep-type values; shutdown enters ACPI mode (bounded SCI_EN wait), then
  writes `SLP_TYP|SLP_EN` to PM1a/PM1b, keeping the legacy emulator ports
  as a fallback and the HLT park as the last resort.
  (`kernel/acpi/acpi.c`, `kernel/acpi/acpi.h`, `kernel/syscall/syscall.c`)
- **.DBP installer/uninstaller ignored `.mem` and `.setting` files.**
  These formats post-date the package scheme, so they fell outside
  `classify_path` and were neither installed nor recorded for uninstall.
  They are now first-class categories matched by extension wherever they
  appear: a `.setting` is relocated into the central
  `/SYSTEM/PROGRAMS/DobSettings/` directory (so settingsd registers it on
  the next boot), a `.mem` stays beside its binary, and both are written
  to the ModuleFiles receipt so uninstall removes them.
  (`programs/DobInstaller/main.c`)
- **Added a "Riavvia" (reboot) entry to the MainDOB menu.** Sits beside
  "Spegni" and calls the new `reboot()` libc wrapper → `SYS_REBOOT`.
  `sys_reboot` now accepts trusted primaries (e.g. dobinterface), not just
  drivers, mirroring how `SYS_SHUTDOWN` is already reachable from the
  desktop. (`boot/dobinterface/main.c`, `libc/include/unistd.h`,
  `kernel/syscall/syscall.c`)
- **Restored the "Disinstalla" entry in the redesigned module manager.**
  The rewrite dropped the uninstall command the old manager had. It is
  back as an "All programs" tab action: it derives the package bubble
  (dirname of the module's mdl_path) and hands it to DobInstaller via
  `--uninstall`, exactly as before. Gated to manifest-backed modules so
  base system modules aren't offered for removal; DobInstaller still owns
  the confirmation and error messaging. (`programs/modules/main.c`)

---

# MainDOB v1.0.0.420.60 — three-branch merge: Armada E500 + dobVideo introspection + Settings

Single consolidated build merging three parallel branches (mach64,
vram-p1p2, settings) that diverged from v59.  These are not three
independent features: they form a three-layer stack — driver →
introspection → user-facing configuration — and were split for
parallel development.

- **mach64 branch** contributed: ATI Mach64 video driver (Rage
  Mobility-P / Rage LT Pro AGP) and ESS Maestro-2E audio driver,
  i.e. the onboard video+audio of the Compaq Armada E500.
- **vram (p1+p2) branch** contributed: (p1) the dobVideo
  introspection cold-path that powers the videotest diagnostic tool,
  which was broken on v59 — a provisional data-plane-over-IPC
  scaffolding had been removed leaving no working query transport;
  (p2) Latin-1/-9 font and the keyboard-layout subsystem (US, IT)
  with the keymap tray applet and AltGr support in inputd.
- **settings branch** contributed: the settings subsystem
  (settingsd daemon + DobSettings editor + dobsettings_protocol +
  the demosettings worked example) and an ATA cold-boot BSY race
  fix in kernel/boot/disk.c.

Merge order applied: vram → mach64 → settings.  All file-level
conflicts were either zero-overlap (auto-merged) or list-union
(`OS_SKIP` in mkbootdisk, `PROGRAMS` in programs/Makefile, EPS
copies in the top Makefile).  One semantic conflict required
manual work: vram extends the dobvc wire protocol with five
introspection opcodes (CAP_QUERY, CAP_QUERY_LIMIT, CAP_QUERY_FORMAT,
VCORE_COUNT, VCORE_INFO) on the bga driver; the mach64 driver
copied dobvc_protocol.h *before* this extension and had to be
brought forward in the merge.  Backport scope: 12 lines added to
`drivers/mach64/dobvc_protocol.h`, 48 lines added to the IPC
dispatcher in `drivers/mach64/mach64_transport_ipc.c`, all five
`dv_*` callee functions already existed in `drivers/mach64/main.c`
with identical signatures to bga's, so the case bodies copy 1:1.

---

## Mach64 video driver (from mach64 branch)

New userspace video driver `drivers/mach64`, an alternative
provider of the `dobVideo` service alongside `bga`.

- **Target.** ATI Mach64 family — specifically the Rage Mobility-P
  (PCI 1002:4C4D) and Rage LT Pro AGP (PCI 1002:4C42) variants
  fitted to the Compaq Armada E500.  Single binary, runtime variant
  detection from PCI device_id.  Two DAS files (`mach64_mobility.das`,
  `mach64_ltpro.das`) match each by exact vendor:device.
- **Architecture.** Same module layout as the bga driver but the
  hot path differs: hardware 2D engine (FIFO/BitBlt/solid fill),
  SCALE_3D engine for alpha+stretched blits, hardware overlay
  plane (YUV→RGB with chroma-key), hardware cursor 64×64×2bpp,
  vblank IRQ.  See `drivers/mach64/README.md`.
- **Memory model.** Single framebuffer, no page flip (a key
  difference from bga, called out in `mach64_state.h`): the
  Mobility-P / LT Pro carry 8 MB / 4 MB of SGRAM respectively,
  not enough to double-buffer at 1024×768×32 while leaving room
  for HW cursor sprites and overlay scratch.
- **Status.** Hot-path code verified against the chip's register
  reference; not yet tested on real E500 hardware.

## ESS Maestro-2E audio driver (from mach64 branch)

New PCI audio driver `drivers/maestro2e`, an alternative provider
of the `audio` service alongside `ac97`.

- **Target.** ESS Maestro-2E / ES1978 (PCI 0x125D:0x1978) — the
  onboard audio of the Compaq Armada E500.  Hardware layer (Wave
  Processor / APU / wavecache, "Bob" timer, AC97 codec access)
  transliterated from the Linux `sound/pci/es1968.c` driver; the
  file is therefore GPL-2.0-or-later.  The DobAudio IPC layer,
  software mixer, ring, and Volume Mixer widget are reused from
  the ac97 driver.
- **DAS integration.** `config/DAS/maestro2e.das` matches by exact
  vendor:device, scoring ~1000 against ac97.das's ~20 class match.
  On an E500 the Maestro DAS wins and `ac97.mdl` is never spawned
  for it; on a generic AC97 box ac97 still wins.  Both DAS files
  coexist — the hotplug matcher picks the right driver per machine,
  no config switch.
- **Status.** Audio hot path verified against es1968.c.  Two spots
  marked `CHECK` in `main.c` (PCI chip-init sequence, APU stereo
  routing reg 11) are not yet pinned to a known-good value — not
  yet tested on real E500 hardware.

## dobVideo introspection cold path (from vram branch, p1)

Five new control-plane opcodes restore the diagnostic-query path
the videotest tool needs.  Pre-v60 videotest was issuing data-plane
`DV_*` opcodes over IPC against a `dispatch_data()` shim inside
bga that had been removed during cleanup; the result was every
query returning `DV_ERR_NOSUPPORT (-3)` and the tool effectively
non-functional.  The fix moves introspection to its proper home
on the control plane:

- **New opcodes** (`drivers/{bga,mach64}/dobvc_protocol.h`):
  `DOBVC_OP_CAP_QUERY` (0x10080), `DOBVC_OP_CAP_QUERY_LIMIT`
  (0x10081), `DOBVC_OP_CAP_QUERY_FORMAT` (0x10082),
  `DOBVC_OP_VCORE_COUNT` (0x10090), `DOBVC_OP_VCORE_INFO` (0x10091).
  All cold-path, queried once at attach time — the fast-path
  boomerang deliberately does not wire them.
- **Stub** `libdob/src/DobVideoControl_stub.c` (new file, 398
  lines): the client-side marshalling for every DOBVC_* opcode,
  callable by any process including one without a vprocess —
  which is exactly what videotest is.
- **videotest** rewritten (`programs/videotest/main.c`) to use the
  two correct transports: the control-plane stub above for
  identity/displays/mode/caps/vcores, and the data-plane boomerang
  (`dv_call_pl`) for `DV_VRAM_INFO`, the one query that lives
  only on the hot path.  The DobTable title and error messages
  are driver-agnostic: the active driver name comes from the
  `DRIVER_INFO` opcode response, so the same tool works against
  bga, mach64, or any future dobVideo provider.

## Multilingual input/output (from vram branch, p2)

- **Font extension** (`libdob/src/font.c`, `libdob/include/dob/font.h`):
  the shared 8×16 bitmap font is rebuilt as a 256-glyph table indexed
  directly by the raw character byte (0..255), branch-free.  Covers
  printable ASCII plus the Latin-1/-9 codepoints needed by the new
  keyboard layouts — accented letters, section/degree/pound/euro signs.
- **Keyboard layout subsystem.**  New `libdob/include/dob/input_layout.h`
  defines a portable scancode→codepoint table with an AltGr level.
  `boot/inputd` is extended: tracks the AltGr modifier (scancode
  0x38/0xB8 make/break), starts with a built-in US layout so the
  earliest keystrokes work, and accepts an `INPUT_SETLAYOUT` message
  to swap layouts at runtime.
- **keymap tray applet** (`programs/keymap`): on-demand layout
  switcher.  Loads `.kbl` files (US and IT shipped; the format is
  declarative, more layouts are just data) and pushes the active
  one to inputd via INPUT_SETLAYOUT.  Skipped from the generic
  Programs copy in mkbootdisk.sh so its `.kbl` siblings ship with it
  under `/SYSTEM/PROGRAMS/keymap/`; hotplug loads it after dobinterface.

## Settings subsystem (from settings branch)

- **settingsd** (`boot/settingsd/`): new boot service, the sole
  process that touches `.setting` files on disk.  Listed as
  `primary driver needs:DobFileSystem` in Startup_modules — the
  `driver` flag grants DobFileSystem sandbox bypass so the daemon
  can write `.setting` files inside other programs' home folders
  (a program's `.setting` lives next to its `.mdl`).  Storage
  model: light registry of known files, one file materialised at
  a time into a single working model, atomic temp-file+rename
  writes.
- **dobsettings_protocol** (`libdob/include/dob/dobsettings_protocol.h`):
  the wire contract.  Identity model is PID-based, mirroring
  DobFileSystem: the daemon resolves `<name>` from the caller's
  PID via SYS_GET_HOME_DIR and operates on that `.setting` file
  alone — a program literally cannot name another program's
  settings file.  One identity is privileged: the editor
  (`SETTINGS_EDITOR_NAME`), the only caller permitted to write
  values.  Three caller classes: owning program (DECLARE_ENTRY,
  GET_VALUE), editor (LIST_FILES, READ_SCHEMA, SET_VALUE),
  anyone (PING).
- **DobSettings** (`programs/DobSettings/`): the GUI editor.
  On-demand program — not a service — that lists known `.setting`
  files, reads their schemas, and lets the user change values.
  Only client permitted to write.
- **demosettings** (`programs/demosettings/`): worked example
  showing the full round trip — `declareSetting` /
  `declareSettingMulti` to register a schema with the daemon at
  startup, `getSetting` / `settingField` to read values back.
  Covers every control type: string (textbox), bool (checkbox),
  enum (dropdown), and a composite "display.resolution" entry
  with two textbox fields (width, height).

## ATA cold-boot race fix (from settings branch)

`kernel/boot/disk.c` — fixes the intermittent `[BOOTFS] Cannot
read sector 0` failure on cold boot.  Root cause: the previous
poll order tested ERR and DRQ before waiting out BSY, but per
the ATA spec ERR/DRQ are undefined while BSY is asserted, so on
a cold boot (drive holds BSY a touch longer) the routine could
read a garbage ERR bit during the BSY window and report a false
failure that then cleared on the next, warmer boot.  Fixed
sequence: 400ns settle (four alt-status reads) so the drive has
asserted BSY, then poll until BSY clears, *then* test ERR/DRQ.
Bounded poll because `load_all_modules` does `cli` before the
boot disk reads — a genuine hang is unrecoverable here, so a
dead drive must return false rather than spin forever.

## Pending integration (deliberately out of scope)

The three-layer stack is now in place at the infrastructure
level but the top-to-bottom loop is not yet wired up:

- No video driver (bga, mach64) currently calls `declareSetting`
  against settingsd to register its configurable options
  (resolution, vsync, double-buffer mode).  The data is queryable
  via CAP_QUERY/CAP_QUERY_LIMIT, the editor is ready to display
  it, but the bridging code in the driver init does not exist
  yet.  This is the next natural piece of work.
- The fast-path boomerang in both video drivers deliberately does
  not wire the new introspection opcodes — they are cold-path only,
  queried at attach time, never per-frame.  This is correct as
  designed and intentional.

---

# MainDOB v1.0.0.420.59 — APIC + scheduler-fix merge

Single consolidated build merging two parallel branches (no separate
55/56/57/58 are released; the version jumps directly from 54 to 59 to
signal "contains everything from both lines"):

- **build 54** branch contributed: B1–B12 hardening, strict-priority
  scheduler fix, wake-time preemption, benchmark realtime exemption.
- **build 58** branch contributed: APIC/tickless time rewrite,
  per-thread `sleep_timer`, performance optimizations, B1–B7 hardening.

Both branches diverged from build 52. Merge target = union of fixes and
optimizations, less the dead code each had carried separately.

---

## Time / interrupt subsystem (from b58)

- **Tickless scheduling.** PIT periodic tick removed. Per-CPU LAPIC
  one-shot timer drives both the scheduler slice and arbitrary
  deadlines. Slice timer is rearmed in `do_switch` based on the new
  thread's priority alone.
- **TSC nanosecond clock.** `clock_now_ns()` reads TSC directly; mul-
  shift conversion calibrated once at boot. No PIT polling.
  `clock_now_ms()` and `clock_now_us()` are inlines on top.
- **Per-thread `sleep_timer`** in `thread_t`. Used by both
  `scheduler_sleep_ns` and the `wait_*_timeout` family. No global
  sleep queue; no global wake scan.
- **Min-heap timer subsystem** (`time/timer.c`), 4096-entry cap, O(log
  n) arm/cancel via per-timer `heap_idx` back-pointer. `timer_cancel`
  short-circuits via lockless fast-path when `heap_idx < 0`.
- **Lazy LAPIC re-arm.** `s_armed_deadline_ns` cache skips MMIO write
  when `heap[0]` hasn't moved.
- **TSC seqlock** (`s_seq`) protects `tsc_now_ns` against re-anchor
  races during long suspend/resume.
- **Stable-TSC advisory.** Soft warning at boot if neither
  `INVARIANT_TSC` nor `HYPERVISOR` CPUID bit is set. Non-fatal:
  re-anchor is pure arithmetic, ns clock stays coherent even on
  drifting hardware. Pre-Nehalem real hardware (~2008 and earlier)
  is out of scope; any VM is fine — `HYPERVISOR` covers it.
- **Civil calendar consolidated** into `time/civil.c`. RTC parses on
  boot only; subsequent reads compute from `clock_now_ns()`.
- Files removed: `arch/x86/rtc.{c,h}`, `arch/x86/timer_mgr.h`.

## Scheduler

- **Strict-priority filter** applied in three places (carries over the
  build 53 fix that was lost in the b58 rewrite):
  - `slice_callback` (replaces `scheduler_tick`): only switches if the
    next ready peer is at equal-or-higher priority. A priority-2
    benchmark no longer alternates with the priority-3 idle thread on
    every quantum boundary.
  - `scheduler_yield`: when caller is `RUNNING`, only switches if a
    peer of equal-or-higher priority is ready. POSIX-ish semantics.
    `BLOCKED`/`SLEEPING` callers always switch (the priority filter
    doesn't apply — the caller has already given up its slot).
  - Helper: `peek_next_priority()` — single `__builtin_ctz(ready_mask)`
    read, no list pop.
- **Wake-time preemption** (`scheduler_unblock`): when waking a thread
  strictly higher-priority than the running one, direct-switches to
  it instead of just queuing. Eliminates up-to-quantum (~10–20 ms)
  latency for IRQ-driven services. Safe from IRQ context (LAPIC ISR
  uses early-EOI specifically so callbacks may context-switch).
- **No-runnable hlt-loop fix** (`scheduler_yield`): when `pick_next`
  returns NULL and caller is `BLOCKED`/`SLEEPING`, calls
  `halt_until_state_change` instead of single-`hlt`-and-return. The
  b58 version returned with the caller still BLOCKED — guaranteed
  hang on the first thread that actually blocked with an empty run
  queue.
- **Quantum donation removed** from `scheduler_yield_to`. The slice
  timer is armed in `do_switch` from the target's priority alone;
  writing `target->quantum` was dead code in the tickless model. The
  legacy "give the receiver only the sender's remaining slice"
  fairness semantics is replaced by strict-priority + wake-time
  preempt.
- **Boot+idle threads unified** (F6). The boot thread (TID 0,
  priority 3, lowest) is itself the idle thread by virtue of strict
  priority. After kmain finishes init, control falls into
  `idle_entry()` (workqueue → pre-zero pages → hlt) instead of a
  bare `cpu_halt` loop. Saves one PCB, one 8 KB kernel stack, and
  removes the `idle_thread` global. Function renamed:
  `thread_create_idle()` → `thread_bootstrap_idle()`.
- **`thread_t::quantum` field removed** (F8). Never read in the
  tickless model — the slice timer always uses
  `priority_quantum_ns[t->priority]`. Field, the writes in
  `thread_create`/`thread_bootstrap_idle`/`scheduler_unblock`,
  the `quantum_for(thread_t*)` helper, the `SCHED_IPC_MIN_QUANTUM`
  constant, and the public `scheduler_quantum_for_priority()` API
  are all gone.
- **`priority_quantum[]` table is now in nanoseconds**, not
  milliseconds. `do_switch` arms the slice with a single load — no
  per-dispatch `MS_TO_NS` multiply on the hot path.

## Wait / sync consolidation (F3)

- Renamed for clarity and migration:
  - `wait_queue_prepare_sleep_timeout` → `wait_prepare`
  - `wait_queue_finish_sleep` → `wait_finish`
- The 9-line `enqueue + state=BLOCKED + sleep_timer arm + … + cancel`
  pattern that mutex/sem/event_group/barrier each inlined separately
  is now centralized. Migrated:
  `kernel/sync/mutex.c`, `kernel/sync/semaphore.c`,
  `kernel/sync/event_group.c`, `kernel/sync/barrier.c`,
  `kernel/registry.c`. Net ~36 lines saved + single source of truth
  for the "block-with-timeout" idiom.

## Hardening (B1–B12)

B1–B7 carried in unchanged from b58 (process accounting limits,
overflow-safe MLK header validation, `wait_queue_remove_thread` for
timeout/death cleanup, `strnlen` in lib/string for ELF interp parsing,
etc.).

- **B8 — `mlk_lock`.** Single global spinlock around mlk load /
  unload / cleanup. Prevents two concurrent loaders from claiming the
  same free slot or installing duplicate entries for the same name;
  prevents two concurrent unloaders from both seeing `refcount == 1`
  and double-destroying. Held across the slow alloc/map/destroy path
  — mlk operations are infrequent (plugin lifecycle), contention is
  irrelevant. `mlk_cleanup_process` snapshots its target list under
  the lock then unloads outside it (mlk_unload re-takes the lock —
  holding across the call would self-deadlock).
- **B9 — drop redundant `pmm_free_contiguous` in mlk load error
  path.** When the trampoline-missing branch fires after the code
  pages have been mapped, `pd_destroy` already walks the PT and
  `pmm_free_frame`'s every mapped page including all of `code_phys`.
  The explicit `pmm_free_contiguous(code_phys, code_pages)` was a
  double-free that corrupted the pmm bitmap.
- **B10 — `kernel_pd_lock` in `ensure_page_table`.** Serializes the
  read-modify-write window for kernel-range PDEs. Without it, two
  CPUs (or two threads on different processes) faulting into the
  same not-yet-existing kernel-range PDE both `pmm_alloc_frame` a
  page table, both install their own entry into their local PD, both
  propagate to `kernel_pd[]`. One frame leaks irrecoverably and the
  two PDs disagree about which PT backs that range. Fast path
  (PRESENT in local PD) takes no lock — the PDE write is 32-bit
  aligned and atomic on x86. Slow path also re-checks
  `kernel_pd[index]` under the lock and copies from the master if
  another CPU got there first, avoiding redundant frame allocation.
- **B11 — `sys_brk` rollback on mid-loop OOM.** The original wrote
  `hr->pages` BEFORE the alloc loop; on OOM mid-loop it returned -1
  with `hr->pages` advanced and frames already mapped but unreachable
  from any tracker (the process leaked memory until exit). New code
  defers `hr->pages`/`hr->committed` writes until the entire growth
  succeeds; on OOM, unmaps and `pmm_free_frame`'s every page just
  mapped in the failed call.
- **B12 — port generation counter.** ABA defense for sync IPC
  cleanup. Per-id monotonic counter (`port_generation[MAX_PORTS]`)
  outside the port struct, bumped on every `ipc_port_create` that
  takes a slot. Senders snapshot it at send time
  (`pending_reply_t::target_port_gen`), check it at sender-side
  cleanup. The b58 ipc_port_t recycle pool returns the same struct
  address for new ports, so the cached `port` pointer alone is no
  evidence the slot still holds OUR port. With the check: if the
  destination was destroyed AFTER our reply was received but BEFORE
  we got to cleanup, `port_dead` was never set (the wake-senders
  walk skipped us — `reply_received` was already true), but the
  generation mismatch tells us the port is gone and our stale
  `port_node` must NOT be `list_remove`'d from the new port's freshly
  `list_init`'d list.

## Benchmark realtime

- `sys_set_priority`: priority 0 (realtime) is normally driver-only;
  exempt by name `strcmp(proc->name, "benchmark") == 0`. Without this,
  benchmarks on stock build can't measure scheduler dispatch cost
  separately from idle-share noise.
- `programs/benchmark/main.c`: calls `set_priority(0)` as first
  instruction in `main()`. With strict-priority guaranteed at prio 0
  the test program holds the CPU for the entire run; results no
  longer jitter ±30 % depending on mouse/window activity.

## Performance optimizations preserved (from b58)

All present unchanged:

- `config.mk` `BUILD_MODE=release` default (-O2 + NDEBUG).
- Kernel built with `-ffunction-sections -fdata-sections` +
  `--gc-sections` link.
- `isr128` calls `syscall_entry` directly — bypasses
  `isr_handler` + `syscall_dispatcher` dual lookup.
- `user_check` unified userspace-pointer validation (~30 sites);
  legacy `is_user_ptr`/`copy_from_user`/`copy_to_user` retained as
  thin inline wrappers around `user_check` (no duplicated logic).
- `thread_get_current` / `process_get_current` inlined in `thread.h`.
- `ipc_port_t` recycle pool LIFO 64-deep.
- `copy_string_from_user` single-pass.
- LFENCE before WRMSR `IA32_TSC_DEADLINE` (was MFENCE — LFENCE is
  enough for ordering-only).
- `tsc_snapshot()` single RDTSC per LAPIC arm.
- `timer_cancel` lockless fast-path when `heap_idx < 0`.
- `userspace free()`: O(n) duplicate scan removed.
- `likely()` / `unlikely()` macros in `lib/types.h`.

## Known semantic changes vs build 54

- `sys_sleep_us` cap lowered 1 s → 100 µs. Longer sleeps return -1
  (use `sys_sleep_ms` for those).
- `scheduler_yield` is a no-op when caller is already the highest
  runnable. Previously yielded to a strictly lower-priority peer
  (which is why the benchmark phenomenon existed in the first place).
- TSC strict floor — see Time subsystem above.
- Quantum donation removed from `yield_to` — see Scheduler above.

## File / artifact churn

- 6 separate changelog files → 1 (`CHANGELOG.md`).
- Removed: `arch/x86/rtc.c`, `arch/x86/rtc.h`, `arch/x86/timer_mgr.h`.
- Renamed: `thread_create_idle` → `thread_bootstrap_idle`;
  `wait_queue_prepare_sleep_timeout` → `wait_prepare`;
  `wait_queue_finish_sleep` → `wait_finish`.
- Removed from public API: `SCHED_IPC_MIN_QUANTUM`,
  `scheduler_quantum_for_priority`.

## Post-merge cleanup pass

A second sweep over the merge result, looking for surface-level
duplications and small pessimizations the first pass left in place:

- **`RESTORE_EFLAGS(saved)` macro** in scheduler.c. Eight identical
  inline-asm blocks `__asm__ volatile ("push %0; popf" …)` collapsed
  to one definition + 8 use sites. The asm is already a single
  instruction so codegen is unchanged; the maintenance cost was the
  problem, not the runtime cost.
- **`sync_sender_count` field removed** from `ipc_port_t`. It was a
  parallel counter to `pending_senders` (intrusive list); `list_empty()`
  is O(1) on a doubly-linked list, exactly the same cost as the old
  counter compare. Fewer state fields = fewer state-sync bug surfaces.
  Net effect on hot path: −2 memory writes per sync IPC round-trip
  (the `++` at send and `--` at cleanup).
- **`mutex_take_locked(m, t)` helper** factored out the 7-line "stamp
  owner + track ownership" sequence shared by `mutex_trylock` and
  `mutex_lock_internal`'s fast path. The duplication had bit-rotted:
  `mutex_trylock` was missing the `owned_mutexes[]` tracking, so a
  thread dying while holding a trylock'd mutex would leave it orphaned
  forever. Single helper = bug fixed by construction.
- **`held_locks` LIFO fast path** in `thread_untrack_lock`. Lock
  acquire/release is LIFO in ~all kernel paths, but the old code
  always entered the scan-and-shift loop. New version: O(1) check on
  top-of-stack; falls back to scan only on out-of-order release. Hot
  path is now one compare + one decrement.
- **`MAX_PENDING` macro removed.** It was hardcoded `4096` in
  channel.c with the explicit invariant "= MAX_THREADS". Two
  redefinitions of the same number are a desync waiting to happen;
  channel.c now uses `MAX_THREADS` directly with a comment explaining
  why they're the same.
- **Branch-free `enqueue` in wait.c.** The two-branch shape
  (`if tail … else head=tail=t`) collapsed to a single shared tail
  write. Marginal — one mov saved on the hit branch — but cleaner.
- **`priority_quantum_ns[]` uses `MS_TO_NS()`** rather than literal
  `*1000000ULL`. Identical machine code (constant-folded), more
  consistent with the rest of the time subsystem.

## Lost optimizations restored / kernel speedups

After the post-merge cleanup, a third audit caught several items the
CHANGELOG had described as in place but which were either missing
from the actual codebase or applied only halfway. These are real
hot-path wins, not cosmetics.

- **Single-CPU spinlock fast path.** `spinlock_acquire_irqsave` was
  doing `cli; LOCK XCHG; (loop)`. After `cli` on a single-CPU kernel,
  no other code can run on this CPU — IRQ handlers can't fire and the
  scheduler can't preempt. The atomic xchg therefore can never fail
  (only writer is only thread) and serves no synchronization purpose;
  its LOCK prefix is a full memory barrier (~10–30 cycles on modern
  x86). Replaced with a plain store gated on `MAINDOB_SMP` (default
  off). The kernel makes 110+ calls to `spinlock_acquire_irqsave`,
  almost all of them on hot paths (every IPC, every scheduler dispatch,
  every mutex/sem guard, every port lock). On an IPC-heavy benchmark
  this is the single biggest visible win.
- **`-ffunction-sections -fdata-sections` + `--gc-sections`** added
  to `KERNEL_CFLAGS` and `LDFLAGS_KERNEL`. The CHANGELOG had claimed
  these were on; they weren't. The kernel binary now ships only the
  symbols actually referenced — smaller text section, smaller working
  set, less i-cache pollution.
- **Removed redundant `cli/sti` pair in `ipc_send_sync_staged`.** The
  old code did `spinlock_release_irqrestore(&port->lock, flags)`
  (which restores IF=1) immediately followed by `pushf; pop; cli`
  (sets IF=0 again) just to enter the receiver-extract critical
  region. Net effect was a sti+cli round-trip per sync IPC, with no
  window of usefulness. Now releases the port lock without IF
  restore; IF stays 0 straight through the extract+yield_to. Saves
  one `pushf`+`pop`+`cli` per sync IPC.
- **`likely()` / `unlikely()` applied** on the actually-hot branches
  in scheduler.c (`slice_callback` priority gate, `scheduler_unblock`
  state guard and preempt branch) and channel.c (port-full check,
  receiver-present check). The macros existed in `lib/types.h` but
  were used twice in the entire kernel. Tells the compiler which
  branch to keep inline and which to push out of the cache line —
  measurable on tight loops, free everywhere else.

## Pure event-driven idle (zero load) — variant B (synchronous reclaim)

**This is build 59-B**, forked from build 59 immediately before the
`idle_entry` cleanup, taking the alternative design path. The fork
exists for direct comparison against build 59-A (which kept the
deferred-zero infrastructure and just stopped calling
`idle_prezero_pages` from idle).

The diagnosis was the same: the idle loop performed structural CPU
work on every iteration. `idle_prezero_pages()` ran on every wake
(acquired `zero_lock`, optionally pulled a frame from `pmm_alloc_frame`,
mapped it to a temp vaddr, memset 512 bytes per call, and on completion
published the page into `zero_cache[]`), so the system was never truly
idle.

### Design — synchronous SHM reclaim

The whole deferred zero pipeline is gone. The fix is:

```
old:  unmap → shm_cache_reclaim → shm_dirty[]
                                  └─ idle: chunked memset (polling)
                                            → zero_cache[]

new:  unmap → shm_cache_reclaim ─[memset on caller's CPU]─→ zero_cache[]
```

The unmapping thread pays the 4 KB memset on its own time. Idle is
never involved in zeroing; it only drains the workqueue and HLTs.

### Code changes vs build 59 (pre-fork)

- **`workqueue.c`** rewritten: removed `shm_dirty[]`, `shm_dirty_count`,
  `wip_phys`, `wip_offset`, `idle_prezero_pages()`, `shm_cache_alloc_dirty()`.
  `shm_cache_reclaim(phys)` now maps the frame to `ZERO_TEMP_VADDR`
  under `zero_lock`+IRQs off, memsets 4 KB, unmaps, and pushes the
  frame directly into `zero_cache[]`. ~50 lines net removed.
- **`workqueue.h`**: drops `idle_prezero_pages` and
  `shm_cache_alloc_dirty` prototypes.
- **`thread.c::idle_entry`**: same shape as build 59-A (drain workqueue
  + HLT). No call to `idle_prezero_pages` (it doesn't exist anymore).
- **`syscall.c::sys_shm_create`**: dropped the level-2 fallback
  (`shm_cache_alloc_dirty`). Allocation is now level-1 (clean cache,
  pre-zeroed from previous reclaim) → level-2 (fresh PMM, post-map
  memset). One indirection less per SHM create.

### Trade-off vs A

- **A**: SHM create always pays the 4 KB memset. Reclaim is essentially
  free (just stash phys in dirty list). System was idle-clean once
  the loop iteration finished.
- **B**: SHM unmap pays the 4 KB memset. Create is free when cache hits
  (`already_zeroed=true`). System is idle-clean immediately after the
  unmap returns.

For the steady-state SHM create+unmap loop in the benchmark, both
designs perform the same single 4 KB memset per cycle — just on
opposite ends of the pair. B has the small additional cost of a
map+unmap of the temp vaddr per reclaim (negligible) but saves one
spinlock acquire+release per create (level 2 went away).

### Lock-held window in shm_cache_reclaim

The map+memset+unmap sequence runs entirely under `zero_lock` with
IRQs off. The window is ~1–2 µs (4 KB memset at 0.5–1 GB/s memory
bandwidth plus two paging ops). On single-CPU this serializes
concurrent reclaims and adds a small IRQ latency floor when reclaims
overlap a hardware IRQ. SHM unmap is rare (programs release SHM at
shutdown, not in tight loops in real workloads), so the latency
impact is below the noise floor for any driver. If a future workload
demonstrates contention, the obvious next step is to drop the lock
across the memset and re-acquire to publish — the lock isn't doing
real synchronization across the memset, only protecting `ZERO_TEMP_VADDR`
ownership and `zero_cache[]` integrity.

---

## Cleanup pass — fossil code removal

Vestigial code from previous architecture iterations was identified
and removed:

- **Old structured-IPC API**. `dob_call_header_t`, `DOB_CALL_MAGIC`,
  `dob_param_type_t`, `dob_fileref_t` and the string-keyed routing
  in `libdob/src/server.c` were the original dispatch mechanism for
  servers — superseded by the simpler opcode-based dispatch
  (every server now does `switch(msg->code)` on its own opcodes).
  All 13 servers had migrated to `dob_server_register("*", ...)`
  (catch-all). The `function_name` parameter has been dropped from
  `dob_server_register`; callers now pass the handler directly.
  Removed: `handlers[]` table, `find_handler`, `catch_all_handler`,
  `DOB_SERVER_MAX_HANDLERS`.
- **HOTPLUG_DEVICE_APPEARED / HOTPLUG_DEVICE_GONE** broadcast codes
  (241 / 242) and their payload typedefs `hotplug_appeared_t` /
  `hotplug_gone_t` were defined but never sent — hotplug publishes
  via `GUI_DEVICE_ATTACH` / `GUI_DEVICE_DETACH` directly.
  `hotplug_events.h` banner corrected accordingly (it claimed the
  removed broadcasts were the contract).
- **`SYS_SPAWN`** macro removed from `kernel/syscall/syscall.h` and
  `libc/include/sys/syscall.h`. ABI slot 3 stays bucked permanently
  (formerly the in-kernel ELF spawn path; superseded by
  `SYS_SPAWN_DATA` via `spawn_file()`).
- **`IPC_ERR_TIMEOUT`** removed — the IPC layer no longer exposes
  timeouts.
- **`VREG_DATA`** flag removed — type tag was defined but never
  applied to any allocation.
- **`FS_SEEK_CUR` / `FS_SEEK_END`** removed from `DobFileSystem.h`
  (only `FS_SEEK_SET` is used; seek-from-current/end is handled via
  `DOBFS_SEEK_*` in the protocol header).
- **Round-robin scheduler banner** in `scheduler.c` corrected to
  describe the actual strict-priority + wake-time preemption model.
- **Orphan code**: `make_unique_name()` in DobFiles (planned
  paste-conflict auto-rename, never wired); `tool_names[]` in
  DobPicture (replaced by icon bitmaps); `atapi_probe_signature()`
  in the ATA driver (body had been inlined into `atapi_scan_all`,
  the original function was left behind).
- **Unused typedefs** removed: `off_t`, `dob_handle_t`,
  `dob_param_type_t`, `dob_fileref_t`.
- **Stale memo** removed from `DobRT.h` (`dob_sleep_us` location
  pointer no longer needed).
- **Hyper-comment cleanup pass** across kernel, libdob, libdobui,
  libc, boot, drivers and programs: ~35 over-long comments
  (>1000 chars each) reduced to factual descriptions of current
  behaviour. Removed cost-model paragraphs, "why we chose X" tracts,
  trade-off discussions, and refactor history breadcrumbs ("v1.0.1.0
  introduced…", "418.5 win", "previously did Y", etc.).

## Typography: new proportional system font (font branch)

A feature pass replacing the old fixed VGA face and introducing
adaptive (proportional) spacing across the UI. No version bump.

- **New 8×16 system font** (`libdob/src/font.c`, regenerated): a humanist
  sans (rendered from DejaVu Sans Condensed) replacing the previous
  fixedsys-style VGA glyphs. Far more legible; same coverage as before —
  printable ASCII plus the Latin-1/-9 codepoints the keyboard layouts emit
  (accents à è é ì ò ù and À È É Ì Ò Ù, plus € £ § °). The lowercase `m`,
  which does not resolve cleanly at 8 px from the TTF, is a small
  hand-tuned override. The runtime glyph atlas (built from `dob_font_data`)
  picks the new face up automatically.
- **Adaptive (proportional) spacing.** Each glyph now carries its inked
  extent — `dob_glyph_l[]` (first inked column) and `dob_glyph_w[]` (inked
  width, 0 = blank) — generated alongside the bitmaps. `font.h` adds
  `dob_font_left()`, `dob_font_advance()` (inked width + 1 px tracking, or a
  fixed advance for blanks) and `dob_text_width()` (UTF-8-aware run width).
  Renderers advance by the real glyph width and shift each glyph left by
  its ink-left so the ink lands at the pen — i.e. spacing derived from the
  first/last inked column of every glyph. (`libdob/include/dob/font.h`,
  `libdob/src/font.c`)
- **Proportional by default, fixed-pitch where it matters.**
  `dobui_DrawText` is now proportional, so labels, buttons, menus, list
  items, window titles, toasts and the system panel all inherit adaptive
  spacing with no caller changes. A new `DOBUI_OP_DRAW_TEXT_FIXED` opcode
  and `dobui_DrawTextFixed()` keep a monospace path for editable text
  fields: the textbox uses it so its cell-based cursor/selection geometry
  is untouched (zero change to the textbox's `DOBTB_FONT_W` arithmetic).
  Window-content thumbnails in Mission Control stay monospace (they are
  downscaled to a few pixels). (`libdob/include/dob/dobui_cmdbuf.h`,
  `boot/dobinterface/DobInterface_stub.c`, `boot/dobinterface/DobInterface.h`,
  `boot/dobinterface/main.c`)
- **Centering follows the proportional metric.** The compositor's shared
  `font_string_width()` and the `doblbl`/`dobbtn` width helpers now measure
  with `dob_text_width()`, so centered/anchored text lands correctly and
  accented labels no longer over-measure by counting UTF-8 bytes.
  (`boot/dobinterface/main.c`, `libdobui/label.c`, `libdobui/button.c`)

## Height-adaptive popups (popups branch)

- **System popups now size their height to the text.** `boot/popups`
  used a fixed 350×140 window (and 350×170 for the input box), so short
  messages wasted space and long ones overflowed. A `count_message_lines()`
  helper replays `draw_message()`'s exact wrapping (char-count + explicit
  `\n` + trailing partial line); `show_popup()` and `show_input_popup()`
  derive the window height from the line count — text block (clearing the
  32 px icon for short messages) plus the button strip below it. The
  buttons keep sitting at the bottom because `setup_buttons()` already
  anchors to `area_h`. For the input box the text field is now placed
  just under the (possibly multi-line) prompt rather than at a fixed
  y=60. Clamped to a minimum (so a one-liner still looks like a dialog)
  and a `POPUP_H_MAX` ceiling (so a pathologically long message can't push
  its buttons off-screen). Covers every popup type — info, warning, error,
  yes/no and input. (`boot/popups/main.c`)
