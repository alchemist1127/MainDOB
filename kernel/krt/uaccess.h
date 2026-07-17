#ifndef MAINDOB_KRT_UACCESS_H
#define MAINDOB_KRT_UACCESS_H

#include "lib/types.h"

/* Accesso a memoria userspace, punto UNICO di validazione — riusato da
 * ogni syscall (nel 1.0 la stessa logica era sparsa e incoerente).
 *
 * Due presidi complementari:
 *   - uaccess_check: il CONFINE (estremi del buffer entro lo spazio
 *     utente, permessi) senza faultare. I buffer utente sono contigui e
 *     i mapping stabili (niente demand paging in 1.1);
 *   - copie fault-safe via extable: chiudono la finestra TOCTOU in cui un
 *     altro thread dello stesso processo smappa il range fra il check e
 *     la copia. Un fault durante la copia viene recuperato
 *     (uaccess_fault_recover), non trasformato in un fault ring-0. */

#define USER_SPACE_LIMIT  0xC0000000u

bool uaccess_check(const void *uptr, uint32_t size, bool need_write);
bool copy_from_user(void *kdst, const void *usrc, uint32_t size);
bool copy_to_user(void *udst, const void *ksrc, uint32_t size);
bool uaccess_get_u32(uint32_t uaddr, uint32_t *out);
bool uaccess_put_u32(uint32_t uaddr, uint32_t value);

/* Copia una stringa utente (max size-1 char) NUL-terminandola.
 * Ritorna la lunghezza copiata, o -1 se il puntatore e' invalido. */
int32_t copy_string_from_user(char *kdst, const char *usrc, uint32_t size);

/* Recupero fixup: chiamato dal page-fault handler. Se il #PF ring-0 e'
 * su un'istruzione di copia registrata in .ex_table, ritorna l'EIP del
 * fixup a cui saltare (la copia fallira' pulita); 0 se non recuperabile.
 * Firma scalare (per valore) per non prendere l'indirizzo di un membro
 * packed di isr_regs_t. */
uint32_t uaccess_fault_recover(uint32_t vector, uint32_t cs, uint32_t eip);

#endif
