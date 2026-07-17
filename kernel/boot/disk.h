#ifndef MAINDOB_BOOT_DISK_H
#define MAINDOB_BOOT_DISK_H

#include "lib/types.h"

/* Disco di boot — SOLO FASE DI BOOT, come bootfs. Legge settori mentre
 * il kernel ha accesso esclusivo al disco (no scheduler, no driver). Due
 * backend dietro un'unica API: ATA PIO (LBA28) sui canali IDE legacy e,
 * in fallback, AHCI/SATA (READ DMA EXT in polling) per i dischi dietro un
 * controller AHCI. I driver ATA/AHCI userspace prendono gli stessi
 * registri: dopo bootfs_shutdown nessuno deve piu' chiamare qui. */

bool boot_disk_init(void);
bool boot_disk_read(uint32_t lba, uint32_t count, void *buf);
uint32_t boot_disk_total_sectors(void);

#endif
