# MainDOB — Driver Mach64 (Rage Mobility-P / MACH64LM) — Fase 2

Implementazione del protocollo video **dv** sopra il bring-up della Fase 1
(già completa: pattern EBU corretto, CRTC/DSP/PLL programmati). Con questa
fase il driver implementa il data-plane e il control-plane che `dobinterface`
usa per disegnare il desktop, e **si auto-registra** come driver "video".

## File in questo pacchetto (tutti in `drivers/mach64/`)

Sostituire i file omonimi nell'albero sorgente:

| file | cosa contiene |
|---|---|
| `main.c` | stato `g_mach64`, allocatore VRAM, blocchi dv #1 (vproc/surface/texture), #2 (cmdlist+replay, glyph in HW), #3 (layer+compose), control-plane, `main()` bootstrap, modeset (Fase 1, invariato) |
| `mach64_2d.c` | motore 2D HW: solid_fill, blit, blit_overlap, line, host_blit, **mono_blit** (espansione monocromatica, PRG §6.3.3.1) |
| `mach64_hw.h` | registri; aggiunti `M64_PIX_WIDTH_1BPP/4BPP` per il mono source |
| `mach64_state.h` | architettura/contratto; aggiunta la dichiarazione di `mach64_hw_mono_blit` |
| `mach64_transport_fast.c` | dispatcher data-plane (boomerang int 0x85) |
| `mach64_transport_ipc.c` | dispatcher control-plane (DOBVC_OP_*) + event loop |
| `mach64_fast_entry.asm` | shim boomerang (invariato dalla Fase 1, già corretto) |

Il `drivers/Makefile` esistente **già** elenca tutti questi oggetti
(`mach64_2d.o`, `mach64_transport_fast.o`, `mach64_transport_ipc.o`,
`mach64_fast_entry.o`): non serve modificarlo.

## Build

Build standard del progetto (`-m32 -ffreestanding`, i686-elf). Nessuna nuova
dipendenza esterna. Gli stub vuoti `mach64_{modeset,3d,irq,cursor,overlay}.c`
restano nel Makefile e non confliggono (il modeset reale è `static` in `main.c`;
3D/IRQ/cursor/overlay non sono ancora usati — scelta deliberata, vedi sotto).

## Decisioni di progetto (coerenza hardware + MainDOB)

- **Single framebuffer**: si compone direttamente nell'unico primary a offset
  VRAM 0; `dv_page_flip` è un no-op (DV_OK). Tear-free via `dv_compose`
  allineato al vblank (polling di `M64_CRTC_VBLANK`). Risparmia ~3 MB di VRAM
  rispetto al double-buffer.
- **Glyph (testo) in HARDWARE** via espansione monocromatica del motore 2D
  (`mach64_hw_mono_blit`): scrittura mascherata binaria, niente lettura della
  VRAM, **aggira il difetto texture-alpha documentato del chip LM** (Rage Pro/
  Mobility — vedi vintage3d.org; Quake3 usava un path software per lo stesso
  motivo). La maschera 1bpp dell'atlante è costruita lazy in VRAM e invalidata
  su `texture_update_region`/`surface_destroy`.
- **Alpha**: opaco (alpha=255) → blitter HW; alpha costante/pixel (raro, livello
  finestra) → over software sulla mappatura VRAM. È l'unico caso che legge la
  destinazione; l'accelerazione 3D (SCALE_3D) è rinviata perché su LM il
  texture-alpha è difettoso.
- **Capabilities oneste** (`dv_cap_query`): PAGE_FLIP, ACCELERATED_BLIT,
  ALPHA_BLEND, VSYNC, HW_SCROLL. **Non** dichiarate: 3D/SHADER/COMPUTE/
  VRAM_MAP/HW_CURSOR/OVERLAY.
- **Profondità colore**: per ora fissa a 32bpp. `format_bpp()` è il singolo
  gancio predisposto per lo switch 256/16/24/32 (feature DobSettings EPS,
  pianificata — vedi "Prossimi passi").

## Stato di verifica (importante, da leggere)

Ogni blocco è stato compilato con gli header reali (libdob+libc) e **testato
funzionalmente in isolamento**:

- allocatore VRAM (alloc/align/free/coalesce/find/slot) — OK
- blocco #1 vproc/surface/texture: quota, contabilità, **nessun leak** — OK
- blocco #2 cmdlist: round-trip record↔replay con clip+offset; packing maschera
  mono ed espansione (pixel esatti, destinazione intatta dove bit=0) — OK
- blocco #3 compose: z-order, skip-clear, dispatch cmdlist/surface, page_flip — OK
- **end-to-end** attraverso `mach64_fast_dispatch` (boomerang), con payload
  impacchettati come libdob: ATTACH→VRAM_INFO→TEXTURE→UPDATE→CMDLIST→GLYPHS→
  LAYER→COMPOSE→FLIP; i glyph vengono espansi in hardware nel compose — OK

**NON ancora verificato** (fuori dalla portata di questo ambiente):
1. **Link del kernel completo** e boot reale: qui mancavano alcuni header di
   sistema per il link finale; i singoli file compilano con gli header di
   libdob/libc presenti.
2. **Esecuzione sul ferro** (Compaq Armada E500). È il prossimo passo
   consigliato prima di EPS.

I test sopra girano su host **64-bit**; gli unici warning (`pointer-to-int-cast`)
sono artefatti dell'ABI boomerang a 32-bit e **spariscono con `-m32`** — verificato
che il driver BGA, che builda pulito a `-m32`, produce gli stessi warning a 64-bit.

## Cosa controllare al primo boot sul ferro

1. Il driver registra "video" nei log dopo "int 0x85 fast path registered" —
   l'ordine è critico (registrare prima sveglierebbe `dobinterface` troppo presto
   → `DV_ERR_NOTREADY`).
2. Il compose: se compare tearing visibile, il `mach64_wait_for_vblank` (polling)
   va sostituito con l'IRQ vsync (blocco IRQ, rinviato).
3. I glyph: se il testo appare a blocchi anziché nitido, controllare il packing
   della maschera mono vs il `DP_BYTE_PIX_ORDER` effettivo del raster.
4. `mach64_modeset` assume 8 MB di VRAM e BAR0 valido (come da dump EVEREST).

## Prossimi passi pianificati

- Blocchi 6-7: **DobSettings EPS** — vsync (bool) e profondità colore
  (256/16/24/32, transazionale: riprogramma CRTC+DSP+engine+riallocazione).
- IRQ vsync reale (oggi: polling), cursore HW 64×64, overlay YUV.
- Eventuale path SCALE_3D per l'alpha costante, se un profilo lo giustifica.
