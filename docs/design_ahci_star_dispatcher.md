# Design — AHCI "a stella": dispatcher IRQ unico e media-change nativo

Stato: **proposta di design. Nessun codice del driver è stato modificato.**
Target: `drivers/ahci/main.c` (build attuale 120).
Obiettivo: far comparire/sparire l'icona di un CD su unità ottica **SATA** quando
il disco viene inserito/rimosso a runtime, usando il meccanismo **nativo** di
notifica asincrona ATAPI (niente polling), senza rompere l'I/O del disco che
oggi funziona.

---

## 1. Perché serve una ristrutturazione (il vincolo hardware)

L'AHCI ha **un solo interrupt fisico per l'intero controller**. Nel driver:

- `irq_num` / `irq_port` sono **unici** (registrati una volta in `init_hardware`,
  riga ~533: `irq_register(irq_num, irq_port)`).
- Quando l'IRQ scatta, la causa va cercata in `HBA_IS` (quale porta) e poi in
  `PxIS` (quale evento su quella porta).

Tutte le porte — disco di sistema e unità ottica — condividono quindi lo stesso
IRQ e la **stessa coda** `irq_port`.

Oggi `irq_port` viene letta **solo** dentro `ahci_wait_irq`, che è invocata
**solo durante un comando** (read/write/identify). Quando il driver è fermo nel
`dob_server_loop` ad aspettare richieste da DobFileSystem, **nessuno** legge
`irq_port`: un interrupt asincrono (media-change) arriverebbe e resterebbe non
processato.

**Conseguenza:** non possiamo semplicemente aggiungere "un secondo lettore" di
`irq_port` per gli eventi async, perché durante una lettura del disco
`ahci_wait_irq` sta già leggendo quella coda → due lettori sulla stessa coda →
si rubano i messaggi (l'IRQ di completamento potrebbe finire al lettore async, e
la lettura del disco andrebbe in timeout). E **non** possiamo dare all'ottico un
IRQ separato: l'AHCI ne ha uno solo, è hardware.

Da qui la struttura **a stella**: un **unico** consumatore di `irq_port`.

---

## 2. La stella

```
                         +------------------------+
   IRQ del controller    |   IRQ DISPATCHER       |
   (irq_port) ---------> |   (thread unico)       |
                         |  - unico lettore di    |
                         |    irq_port            |
                         |  - legge HBA_IS/PxIS   |
                         |  - smista per causa    |
                         +-----------+------------+
                                     |
              +----------------------+----------------------+
              |                                             |
   completamento comando                          evento async (media)
   su porta P                                     su porta ottica P
              |                                             |
              v                                             v
   notify -> il contesto che                     GESN(P) -> media presente?
   ha emesso il comando su P                       sì -> SUBDEVICE_APPEARED
   (ahci_wait_irq, riscritta come                  no -> SUBDEVICE_GONE
   attesa di notify, NON legge irq_port)          (stessa identica chiamata
                                                   che main() fa già al boot)
```

Punto chiave: **un solo lettore** di `irq_port` (il dispatcher). Tutti gli altri
contesti non toccano `irq_port`; vengono **svegliati** dal dispatcher. Così lo
scenario "due lettori che si contendono la coda" non esiste per costruzione.

---

## 3. Primitive disponibili (verificate nel codice)

- `dob_thread_spawn(fn, arg)` — avvia il thread dispatcher. Già usato nel driver
  per i `*_partition_pusher_thread`, quindi pattern noto e funzionante.
- `dob_ipc_notify(port_id, bits)` / `dob_ipc_wait_notify(port_id)`
  (in `libdob/include/dob/ipc.h`) — il canale di risveglio leggero tra
  dispatcher e contesto-comando. Il contesto comando aspetta una notify; il
  dispatcher la manda quando il comando su quella porta è completo.
- `HOTPLUG_SUBDEVICE_APPEARED` (244) / `HOTPLUG_SUBDEVICE_GONE` (245) — già
  esistenti; l'invio APPEARED è **identico** a quello che `main()` fa oggi al
  boot per gli ottici (righe ~960-981). GONE è il simmetrico.

Niente di tutto ciò è nuovo da inventare: sono mattoni già presenti.

---

## 4. Per-porta: stato e completamento

Oggi il completamento di un comando è "fai partire il comando, poi resta in
`ahci_wait_irq` a leggere `irq_port` finché `PxCI` torna a 0". Nella stella
diventa:

1. il chiamante (es. handler READ) prepara il comando, scrive `PxCI`, poi chiama
   una nuova `ahci_wait_done(P)` che fa `dob_ipc_wait_notify(cmd_wait_port[P])`;
2. il dispatcher, vedendo `HBA_IS` bit P e `PxIS` "completamento", fa
   `dob_ipc_notify(cmd_wait_port[P], …)` e pulisce `PxIS`/`HBA_IS`/`irq_done`;
3. `ahci_wait_done(P)` si sveglia, controlla `PxCI`/`PxTFD` (errore) come fa già
   oggi `ahci_wait_irq`, e ritorna.

Serve quindi, per ogni porta che può avere un comando in volo, un piccolo stato:
- `uint32_t cmd_wait_port[MAX_PORTS]` — porta di notify dedicata su cui il
  contesto-comando di quella porta attende. (In pratica i comandi su una stessa
  porta sono serializzati, quindi una notify-port per porta basta.)

Nota: oggi i comandi sono già serializzati di fatto (un solo `dma_buf`/`cmd_table`
per porta). Il design **non** introduce concorrenza di comandi sulla stessa
porta; mantiene l'attuale "un comando per porta alla volta".

---

## 5. Media-change: cosa aggiunge (il vero scopo)

Sulla **porta ottica**:

1. **Abilitazione async** (in `port_setup`, solo se `type == DEV_OPTICAL`):
   - abilitare in `PxIE` il bit del **Set Device Bits FIS** (async notification)
     oltre all'attuale `0x01` (D2H);
   - abilitare a livello HBA il supporto SNTF se richiesto dal controller.
   - (I bit esatti vanno fissati con i numeri AHCI/SATA; qui restano descritti,
     non "indovinati" — è uno dei punti da validare al primo collaudo.)

2. **Nel dispatcher**, quando l'evento su una porta ottica è di tipo
   async-notify (non un completamento comando):
   - emetti `GET EVENT STATUS NOTIFICATION` (CDB 0x4A) via `issue_atapi`
     (già esistente) sulla porta;
   - interpreta l'evento:
     - media **inserito / presente** → manda `HOTPLUG_SUBDEVICE_APPEARED`
       (riusa pari pari il blocco di `main()` boot: provider_token = P,
       class 0x01 / subclass 0x05, provider "ahci");
     - media **rimosso** → manda `HOTPLUG_SUBDEVICE_GONE` con lo stesso token.
   - aggiorna uno stato per-porta `media_present[P]` per non rimandare eventi
     duplicati.

Il "far comparire l'icona" è quindi la **stessa** azione del boot, solo
innescata da un evento invece che dallo scan iniziale. Questo è esattamente il
punto che avevi sottolineato: la parte icona è già risolta.

---

## 6. Cosa NON cambia (per proteggere l'I/O che funziona)

- Il protocollo verso DobFileSystem (READ=1, WRITE=2, IDENTIFY=3, LIST_PORTS=10,
  …) e i formati di reply restano **identici**.
- `dob_server_loop` continua a servire le richieste di DobFileSystem sulla porta
  del server, invariato.
- La serializzazione "un comando per porta" resta.
- Il watchdog anti-doppione e il ruolo `bootdisk` (build 119) restano invariati.
- Su un controller **senza** unità ottiche, il comportamento è identico a oggi:
  il dispatcher gestisce solo completamenti comando; nessun percorso media-change
  si attiva.

---

## 7. Rischi e perché si procede a tappe

Questa è una modifica al **cuore della gestione IRQ**, su codice di basso livello
che non posso eseguire io. Rischio principale: regredire l'I/O del disco appena
stabilizzato (build 119/120). Mitigazione: **tappe collaudabili**.

### Tappa A — dispatcher "trasparente" (nessuna nuova feature)
Introdurre il thread dispatcher come **unico** lettore di `irq_port`, e riscrivere
`ahci_wait_irq` → `ahci_wait_done` (attesa via notify). Nessun media-change
ancora. **Criterio di successo:** l'I/O del disco SATA continua a funzionare
esattamente come nella build 120 (boot, lettura/scrittura, installazione). Se
questo non regge, ci si ferma qui e si capisce perché, senza aver introdotto
complessità media-change.

### Tappa B — abilitazione async + GESN con SOLO log
Abilitare le async notification sulla porta ottica e, nel dispatcher, loggare
("evento async su porta P", risultato GESN) **senza** ancora mandare
SUBDEVICE_*. **Criterio:** inserendo/togliendo un CD nell'unità SATA, compaiono i
log attesi. Conferma che l'evento nativo arriva e che GESN lo interpreta, prima
di toccare le icone.

### Tappa C — invio SUBDEVICE_APPEARED / GONE
Collegare l'evento all'invio dei subdevice. **Criterio:** l'icona del CD SATA
compare/sparisce a runtime.

Ogni tappa è una build separata, con i suoi log, collaudata prima di passare
alla successiva. È lo stesso metodo che ha chiuso i bug precedenti: log reali →
causa → passo successivo, mai a indovinare.

---

## 8. Punti aperti da validare al collaudo (onestà tecnica)

1. I **bit esatti** di `PxIE`/SNTF per le async notification ATAPI su questo
   controller QEMU (ich9-ahci) e su hardware reale — da confermare col primo log
   della Tappa B.
2. Come **QEMU** modella l'inserimento del media su un'unità AHCI: genera davvero
   un async-notify, o solo un cambiamento rilevabile via GESN su richiesta? Se
   QEMU non emette l'async, la Tappa B lo rivela subito (nessun log evento) e si
   decide il da farsi — senza aver compromesso l'I/O.
3. Comportamento di `irq_done` con il dispatcher: oggi `irq_done(irq_num)` è
   chiamato dentro `ahci_wait_irq`; nel dispatcher va chiamato lì, una volta per
   IRQ ricevuto, dopo aver pulito `PxIS`/`HBA_IS`. Da verificare che il timing
   regga sotto carico I/O.

---

## 8-bis. Primitiva di sistema rilevante (scoperta verificando il kernel)

Il kernel **non** consegna gli IRQ come messaggi generici: in `sys_irq` /
`irq_forward_handler` (kernel/syscall/syscall.c ~riga 212) fa

```c
ipc_notify(irq_notify_port[irq], (1u << irq));
```

cioè una **notifica** con bitmask = `(1 << irq)` sulla porta registrata con
`irq_register(irq_num, notify_port)`. Quindi:

- Il kernel **già demultiplexa per linea IRQ**: IRQ diversi → bit diversi nella
  notifica. Il dispatcher può capire *quale* linea ha fatto scattare l'evento
  dal bitmask, prima ancora di leggere `HBA_IS`.
- La primitiva di attesa naturale è `dob_ipc_wait_notify(port)` /
  `dob_ipc_notify(port, bits)` — **già usata dal kernel** per consegnare gli IRQ.
  È esattamente il canale "centro → contesto-comando" della stella; non serve
  inventare un meccanismo di risveglio.

**E soprattutto:** il pattern "una porta, più sorgenti, smistamento per tipo"
**esiste già** in `ahci_wait_irq`. Oggi `irq_port` riceve insieme:
- le **notifiche IRQ** del kernel, e
- i messaggi del **timer** (`timer_set(irq_port, timeout_ms, 0)` per il timeout),
e il loop di `ahci_wait_irq` li distingue (controlla `msg.type == 3` per l'IRQ e
`msg.code == 70` per il timer).

Implicazione per il design: la stella **non introduce un paradigma nuovo**, ma
generalizza un loop di ricezione/smistamento che il driver ha già. Il dispatcher
è "`ahci_wait_irq` promosso a thread permanente che, oltre a timer e
completamenti, riconosce anche gli eventi async e li instrada". Questo riduce il
rischio: stiamo estendendo codice collaudato, non sostituendolo con un modello
estraneo.

Punto da validare in Tappa A: confermare che, spostando la ricezione di
`irq_port` nel dispatcher, `ahci_wait_done` riceva il via libera tramite la
stessa identica meccanica di notify che oggi sblocca `ahci_wait_irq` — così il
percorso di completamento comando resta, nei fatti, quello attuale.

## 8-ter. Protezione dalle race — primitiva nativa `enter_critical`/`exit_critical`

Nella stella più contesti girano in parallelo (il thread dispatcher; i thread
che emettono comandi su richiesta di DobFileSystem; il pusher delle partizioni).
Toccano stato condiviso del driver che va protetto. MainDOB offre la primitiva
adatta, in `libc/include/unistd.h`:

```c
int enter_critical(void);   /* non-preemption guard, solo driver, annidabile */
int exit_critical(void);
```

Caratteristiche (dalla doc dell'header):
- **Guardia di non-preemption**: mentre si è dentro, lo scheduler non passa a un
  altro thread che si risveglia — la sezione gira atomica rispetto agli altri
  thread del driver. È esattamente la mutua esclusione che serve qui.
- **Contata/annidabile**: enter/exit possono nidificarsi.
- **Solo per driver** (ata/ahci lo sono): ritorna -1 altrimenti.
- **NON disabilita gli interrupt**: gli IRQ handler continuano a girare, quindi
  la consegna degli IRQ (e quindi delle notify) non viene bloccata. Importante:
  non rischiamo di perdere eventi async mentre siamo in sezione critica.
- **Vincolo d'uso**: la regione protetta dev'essere **corta** — ritarda i thread
  a priorità più alta finché non si esce. Quindi: proteggere solo
  l'aggiornamento dello stato, mai un'attesa di I/O o una `wait_notify`.

### Stato condiviso da proteggere (e regioni corte)

1. **`media_present[P]`** (nuovo): letto/scritto dal dispatcher quando valuta
   l'esito GESN. Regione critica = solo il confronto-e-aggiornamento del flag
   ("era assente, ora presente?"), **non** il comando GESN né l'invio del
   subdevice (quelli stanno fuori dalla sezione critica).

2. **Proprietà del comando per porta** (`cmd_wait_port[P]` / "comando in volo su
   P"): la sequenza "marca P occupata + scrivi PxCI" e la sequenza di rilascio
   vanno serializzate rispetto al dispatcher che potrebbe segnalare il
   completamento. Regione critica = solo la transizione di stato, non l'attesa.

3. **`ports[P]` durante (ri)configurazione**: se in futuro una porta viene
   riconfigurata a runtime, le scritture ai puntatori DMA (`cmd_list`,
   `cmd_table`, `dma_buf`) vanno protette. Allo stato attuale `port_setup` gira
   solo all'init (prima che il dispatcher serva richieste), quindi qui non serve
   subito — annotato per completezza.

### Principio guida

`enter_critical` protegge **transizioni di stato brevi**, non attese. Tutto ciò
che blocca (I/O, `wait_notify`, comandi al device) sta **fuori** dalla sezione
critica. Questo rispetta il vincolo "regione corta" ed evita che il guard ritardi
i thread ad alta priorità o, peggio, che si tenti di attendere mentre si è dentro
(che congelerebbe lo scheduling).

Da validare in Tappa A: verificare che avvolgere le transizioni di stato comando
con enter/exit non alteri il timing dell'I/O del disco (le regioni sono di poche
istruzioni, quindi l'impatto atteso è nullo).

## 9. Riepilogo

- **Cosa**: un thread dispatcher unico lettore dell'IRQ AHCI condiviso, che
  smista completamenti-comando (→ notify a chi aspetta) ed eventi async media
  (→ GESN → SUBDEVICE_APPEARED/GONE).
- **Perché così**: l'AHCI ha un solo IRQ fisico; la stella evita la contesa sulla
  coda per costruzione (un solo lettore).
- **Come**: con primitive già presenti (`dob_thread_spawn`, `dob_ipc_notify` /
  `wait_notify`, subdevice 244/245, `issue_atapi`).
- **Race**: protette con `enter_critical`/`exit_critical` (guard di
  non-preemption nativo, solo-driver, annidabile) su **brevi transizioni di
  stato** — mai su attese o I/O.
- **Sicurezza**: il protocollo DobFileSystem e l'I/O non cambiano; tappe
  collaudabili; su sistemi senza ottici nulla si attiva.
