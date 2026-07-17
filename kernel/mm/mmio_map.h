#ifndef MAINDOB_MM_MMIO_MAP_H
#define MAINDOB_MM_MMIO_MAP_H

#include "lib/types.h"

/* Mappatura di memoria fisica fuori dal direct-map (registri MMIO di un
 * device, o una tabella firmware oltre i 16MB) in una finestra virtuale
 * kernel dedicata.
 *
 * Danza standard (D9 — un solo posto testato, mai duplicata):
 *   1. riserva `pages` pagine di VA kernel via kpages_alloc, che le
 *      aggancia gia' a frame RAM casuali;
 *   2. smonta ogni pagina e libera quei frame — non si vuole RAM li',
 *      la fisica finale e' quella del chiamante, non quella assegnata
 *      dall'allocatore;
 *   3. rimappa ogni pagina sulla fisica voluta, coi flag richiesti
 *      (PTE_CACHE_DISABLE per registri MMIO veri; niente per una
 *      tabella firmware in RAM, dove cache disabilitata non serve e
 *      costerebbe in lettura).
 *
 * Prima di questa consegna, lapic.c/ioapic.c/acpi.c avevano ciascuno la
 * propria copia di questa identica danza (gia' cosi' nel kernel 1.0:
 * map_lapic_mmio, map_ioapic_mmio, scratch_map). Consolidata qui. */

/* Mappa `len` byte fisici a partire da `phys`. Se `cache_disable` e'
 * true la finestra e' PTE_CACHE_DISABLE (registri MMIO); altrimenti e'
 * una mappatura RAM ordinaria (tabelle firmware, lette non scritte ma
 * mappate scrivibili per uniformita', come da convenzione 1.0).
 * Ritorna il puntatore al byte a `phys` (l'offset dentro la prima
 * pagina e' gestito internamente), o NULL su fallimento di
 * allocazione. `*out_virt`/`*out_pages` sono la base della finestra e
 * il conteggio pagine, da passare a mmio_unmap. */
void *mmio_map(uint64_t phys, uint32_t len, bool cache_disable,
              uint32_t *out_virt, uint32_t *out_pages);

/* Smonta una finestra ottenuta da mmio_map e ricicla la VA. Le pagine
 * sono MMIO/riservate: niente frame fisico da liberare, kpage_free
 * ricicla solo l'indirizzo virtuale. */
void mmio_unmap(uint32_t virt, uint32_t pages);

#endif
