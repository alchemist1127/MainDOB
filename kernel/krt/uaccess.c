#include "krt/uaccess.h"
#include "mm/paging.h"
#include "mm/pmm.h"      /* PAGE_SIZE */
#include "lib/string.h"

/* === Verbo di validazione (usato da tutti i copy_*) ===================== */

bool uaccess_check(const void *uptr, uint32_t size, bool need_write)
{
    if (size == 0)
    {
        return true;
    }
    if (uptr == NULL)
    {
        return false;
    }

    uint32_t addr = (uint32_t)uptr;
    if (addr >= USER_SPACE_LIMIT)
    {
        return false;
    }
    if (addr + size < addr || addr + size > USER_SPACE_LIMIT)
    {
        return false;                   /* overflow / sconfina nel kernel */
    }

    /* Prima e ultima pagina: presenti + user-accessibili (+scrivibili). */
    uint32_t first = addr & ~0xFFFu;
    uint32_t last  = (addr + size - 1) & ~0xFFFu;

    for (uint32_t page = first; ; page += PAGE_SIZE)
    {
        uint32_t flags = 0;
        if (paging_get_physical(page, &flags) == 0)
        {
            return false;
        }
        if ((flags & PTE_USER) == 0)
        {
            return false;
        }
        if (need_write && (flags & PTE_WRITABLE) == 0)
        {
            return false;
        }
        if (page == last)
        {
            break;
        }
    }
    return true;
}

/* === Copia fault-safe (extable) ========================================= *
 * uaccess_check convalida il CONFINE (utente/kernel, permessi) senza
 * faultare, ma tra il check e la copia un altro thread dello stesso
 * processo puo' smappare il range (TOCTOU): il memcpy nudo faulterebbe a
 * ring 0. copy_user_asm rende la copia fault-safe — la sua istruzione e'
 * registrata in .ex_table; su fault il page-fault handler salta al fixup
 * (vedi uaccess_fault_recover) e la copia riporta i byte NON copiati. Le
 * due difese sono complementari: il check e' la SICUREZZA (mai leggere/
 * scrivere kernel per un puntatore ostile), l'extable e' la ROBUSTEZZA
 * (la pagina validata che sparisce a meta' copia). */

/* Blocco esecutivo: copia fault-safe a DUE stadi — il grosso a dword
 * (rep movsl, ~4x rispetto al movsb byte-a-byte su tutto il silicio
 * P6/Core di riferimento, che non ha ERMSB) e la coda 0..3 a byte.
 * Un solo compito, nessuna decisione. Ritorna i byte NON copiati.
 *
 * Contabilita' del fault, per stadio:
 *   - fault nel movsl (1:): ECX = dword residue -> il fixup 4: le
 *     riconverte in byte (shl 2) e somma la coda mai iniziata;
 *   - fault nel movsb (2:): ECX = byte residui della coda, gia' nella
 *     unita' giusta -> fixup diretto a 3:.
 * Il recover (#PF ring 0 su EIP in .ex_table) rimpiazza solo l'EIP:
 * ECX/ESI/EDI del momento del fault sopravvivono per costruzione
 * dell'ISR — e' cio' che rende possibile fare la matematica nel fixup. */
static uint32_t copy_user_asm(void *dst, const void *src, uint32_t len)
{
    uint32_t d  = (uint32_t)dst, s = (uint32_t)src;
    uint32_t n  = len >> 2;             /* dword del grosso             */
    uint32_t tr = len & 3u;             /* coda a byte                  */
    __asm__ volatile (
        "    cld\n"
        "1:  rep movsl\n"
        "    movl %[tr], %%ecx\n"
        "2:  rep movsb\n"
        "3:\n"
        "    jmp 5f\n"
        "4:  shll $2, %%ecx\n"          /* dword residue -> byte        */
        "    addl %[tr], %%ecx\n"       /* + coda mai iniziata          */
        "5:\n"
        "    .pushsection .ex_table,\"a\"\n"
        "    .align 4\n"
        "    .long 1b, 4b\n"
        "    .long 2b, 3b\n"
        "    .popsection\n"
        : "+c"(n), "+S"(s), "+D"(d)
        : [tr] "r"(tr)
        : "memory");
    return n;
}

/* === Copie ============================================================== */

bool copy_from_user(void *kdst, const void *usrc, uint32_t size)
{
    if (!uaccess_check(usrc, size, false))
    {
        return false;
    }
    return copy_user_asm(kdst, usrc, size) == 0;
}

bool copy_to_user(void *udst, const void *ksrc, uint32_t size)
{
    if (!uaccess_check(udst, size, true))
    {
        return false;
    }
    return copy_user_asm(udst, ksrc, size) == 0;
}

bool uaccess_get_u32(uint32_t uaddr, uint32_t *out)
{
    return copy_from_user(out, (const void *)uaddr, sizeof(uint32_t));
}

bool uaccess_put_u32(uint32_t uaddr, uint32_t value)
{
    return copy_to_user((void *)uaddr, &value, sizeof(uint32_t));
}

int32_t copy_string_from_user(char *kdst, const char *usrc, uint32_t size)
{
    if (size == 0 || !uaccess_check(usrc, 1, false))
    {
        return -1;
    }

    /* A BLOCCHI di pagina, non a byte (lezione della caccia b126->b130:
     * la versione per-byte pagava cld+setup dell'asm fault-safe PER
     * CARATTERE — su ogni path di ogni open/spawn/registry). Ogni giro:
     * un solo uaccess_check e una sola copy_user_asm per il tratto fino
     * al confine di pagina, poi la scansione del NUL avviene su memoria
     * KERNEL gia' al sicuro. Semantica identica: validazione a ogni
     * pagina attraversata, fallimento pulito se la pagina sparisce
     * nella race (la copy riporta il tratto monco: si tiene il valido).
     * Nota fedele all'originale: un fault a meta' tratto tronca la
     * stringa li' anche se il NUL fosse nel pezzo copiato prima del
     * fault — impossibile: copy_user_asm copia in avanti, i byte prima
     * del fault SONO arrivati e vengono scanditi comunque. */
    uint32_t i = 0;
    while (i < size - 1)
    {
        uint32_t upos    = (uint32_t)(usrc + i);
        uint32_t to_page = PAGE_SIZE - (upos & (PAGE_SIZE - 1u));
        uint32_t room    = (size - 1u) - i;
        uint32_t chunk   = (to_page < room) ? to_page : room;

        if (!uaccess_check(usrc + i, 1, false))
        {
            break;                      /* pagina successiva non valida */
        }
        uint32_t missed = copy_user_asm(kdst + i, usrc + i, chunk);
        uint32_t got    = chunk - missed;

        /* Scansione del NUL nel tratto arrivato: memoria kernel. */
        for (uint32_t k = 0; k < got; k++)
        {
            if (kdst[i + k] == '\0')
            {
                kdst[i + k] = '\0';
                return (int32_t)(i + k);
            }
        }
        i += got;
        if (missed != 0)
        {
            break;                      /* fault nella race: stop pulito */
        }
    }
    kdst[i] = '\0';
    return (int32_t)i;
}

/* === Recupero fault (fixup) ============================================= *
 * Consultato dal page-fault handler PRIMA di attribuire un fault ring-0
 * al processo: se l'EIP che ha faultato e' un'istruzione di copia
 * registrata in .ex_table, salta al suo fixup (che fa fallire la copia)
 * invece di uccidere. Firma scalare (vector/cs/&eip) per non accoppiare
 * uaccess a isr_regs_t. La tabella e' popolata dai .pushsection sopra. */
extern uint32_t _ex_table_start[];
extern uint32_t _ex_table_end[];

uint32_t uaccess_fault_recover(uint32_t vector, uint32_t cs, uint32_t eip)
{
    if (vector != 14u || (cs & 0x3u) != 0u)
    {
        return 0;                       /* solo #PF a ring 0              */
    }
    for (uint32_t *e = _ex_table_start; e < _ex_table_end; e += 2)
    {
        if (e[0] == eip)
        {
            return e[1];                /* EIP del fixup                  */
        }
    }
    return 0;                           /* non e' una copia registrata    */
}
