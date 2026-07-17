#ifndef MAINDOB_BOOT_BOOTFS_H
#define MAINDOB_BOOT_BOOTFS_H

#include "lib/types.h"

/* Lettore FAT32 minimale — SOLO FASE DI BOOT (impianto osservato dal
 * 1.0). Contratto critico ereditato: bootfs_read_file NON va chiamato
 * dopo che i moduli sono avviati — il driver ATA userspace condivide
 * gli stessi registri PIO. bootfs_shutdown() rende la violazione
 * impossibile per costruzione: ogni read successiva fallisce a voce
 * alta. Backend: ramdisk (modulo GRUB "live_fs") in questa build; il
 * backend ATA PIO da disco arriva col porting driver (osservare
 * boot/disk.c del 1.0 prima). */

bool        bootfs_init(void);      /* backend disco ATA (boot_disk_*) */
bool        bootfs_init_ramdisk(uint32_t phys_base, uint32_t size);
const void *bootfs_live_blob_ptr(void);
uint32_t    bootfs_live_blob_size_bytes(void);

/* Legge un file per path assoluto. Ritorna un buffer statico interno
 * (valido fino alla read successiva; NON kfree) e scrive *size. */
void *bootfs_read_file(const char *path, uint32_t *size);
void  bootfs_shutdown(void);

#endif
