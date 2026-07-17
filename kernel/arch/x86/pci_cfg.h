/* MainDOB — possessore UNICO del meccanismo PCI config 0xCF8/0xCFC.
 *
 * Il meccanismo legacy e' UNA coppia di porte globali: l'indirizzo va
 * su 0xCF8, il dato transita su 0xCFC. Due cicli concorrenti (una
 * seconda CPU, o kernel vs syscall driver) si interfogliano e leggono
 * o scrivono il registro del DEVICE SBAGLIATO — su SMP e' una race
 * viva, intermittente e senza firma: un pin INT# letto zero, un BAR
 * corrotto, un MSI armato sul device sbagliato. Su UP non scatta mai,
 * ed e' per questo che il ferro monoprocessore non l'ha mai mostrata.
 *
 * Regola D9 (un solo posto testato, mai duplicato): prima di questa
 * consegna il ciclo era copiato identico in syscall/driver.c (kpci_*)
 * e irq/pirq.c (pci_cfg_* privati). Ora vive qui, sotto un unico
 * spinlock irqsave che serializza OGNI accesso config del sistema:
 * kernel (pirq, MSI, risoluzione INTx) e userspace (SYS_PCI_READ /
 * SYS_PCI_WRITE passano da qui). ECAM/MCFG si aggancera' qui quando
 * arrivera' il suo milestone. */

#ifndef MAINDOB_ARCH_X86_PCI_CFG_H
#define MAINDOB_ARCH_X86_PCI_CFG_H

#include "lib/types.h"

/* Lettura/scrittura di un dword config allineato (offset & 0xFC). */
uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint32_t offset);
void     pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func,
                         uint32_t offset, uint32_t value);

/* Accessi a byte: read-modify-write del dword contenitore, ATOMICI
 * rispetto a ogni altro ciclo config (stesso lock). */
uint8_t  pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func,
                       uint8_t offset);
void     pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint8_t value);

#endif /* MAINDOB_ARCH_X86_PCI_CFG_H */
