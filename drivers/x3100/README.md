# Intel GMA X3100 (GM965) driver — register-probe stage

## Cos'è adesso
Driver **read-only**. Si aggancia a hotplug via i DAS `x3100_2a02.das` /
`x3100_2a03.das`, mappa il BAR MMIO del device e fa il **dump dei
registri di display** come il firmware (VBIOS) li ha lasciati, sulla
console seriale. Poi resta in idle. **Non scrive nulla** sull'hardware:
niente modeset, niente PLL, niente power del pannello. Zero rischio sul
pannello LVDS reale.

Rispetta il modello MainDOB: kernel e boot **non toccati**. Il driver
parla all'hardware da userspace col proprio device, esattamente come
bga e mach64.

## A cosa serve
A leggere, sul vero Extensa 5220, i valori che servono per scrivere poi
il modeset nativo senza indovinare:
- quale BAR contiene i registri MMIO (tabella BAR stampata),
- quale pipe (A/B) pilota il pannello LVDS interno,
- i divisori DPLL funzionanti e il reference clock,
- i delay di power-sequencing del pannello (PP_*),
- la config LVDS (canali/bpp) e il panel fitter,
- la risoluzione/timing attivi.
È la stessa tecnica del mach64 (che citava LCD_GEN_CNTL = 0x407524DE dal
dump del BIOS Compaq).

## Come leggere l'output
Avvia con la seriale collegata e cerca le righe `[x3100]`. Le righe
`HINT:` in coda spiegano come interpretare i valori (pipe abilitata,
risoluzione da PIPExSRC, divisori PLL da riusare).

## Prossimo passo
Con i valori del dump, si riempiono i `TODO[PRM §x.y]` in `x3100_hw.h`
(bit di DPLL/PIPECONF/DSPCNTR/LVDS/PP_CONTROL/PFIT) e si scrive
`x3100_modeset()` seguendo la sequenza §2.2.2 del PRM Vol.3. Solo allora
il driver passa da "probe" a "video driver" vero (boomerang 0x85 +
protocollo DV + EBU bars native).

## Fonte registri
Intel 965/G35 PRM Volume 3, Display Registers, Jan 2008 Rev 1.0c
(Creative Commons). Mirror: x.org/docs/intel/VOL_3_display_registers.pdf
