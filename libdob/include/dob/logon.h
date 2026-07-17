/* MainDOB -- Logon password record (/SYSTEM/CONFIG/Logon_password.dat)
 *
 * Il contratto condiviso fra CHI CREA il record (MainDOB_Setup al
 * momento dell'installazione, dobinterface al cambio password) e CHI
 * LO VERIFICA (il foglio logon di dobinterface).  Header-only, come
 * rt_helpers.h: si include e si usa, niente oggetti in libdob.
 *
 * SEMANTICA -- sicurezza PASSIVA, dichiarata come tale.
 *
 *   La presenza del file accende la schermata di accesso; la sua
 *   assenza la spegne (la ISO live non lo contiene: si va dritti al
 *   desktop demo).  La password NON cifra nulla: chiunque abbia
 *   accesso fisico al disco legge tutto con un altro sistema.  E' una
 *   tenda contro l'impiccione occasionale, non una cassaforte -- e
 *   proprio per questo il file NON contiene mai la password in
 *   chiaro: contiene un hash salato, cosi' un backup copiato o un
 *   processo whitelistato curioso non la regalano comunque.
 *
 * FORMATO -- una riga di testo, versionata:
 *
 *   MDLOGON1 <salt:8 hex> <hash:16 hex>\n
 *
 *   Il magic versionato consente di cambiare schema in futuro senza
 *   ambiguita': un magic ignoto si tratta come "file assente"
 *   (fail-open, coerente con la natura passiva del meccanismo).
 *
 * HASH -- FNV-1a a 64 bit su (salt || password), poi
 *   DOB_LOGON_ROUNDS giri di rimescolamento avalanche (xorshift +
 *   moltiplicazione).  NON crittograficamente forte in senso stretto
 *   -- dichiarato come tale, nello stesso spirito del pool di
 *   entropia del kernel -- ma piu' che adeguato allo scopo passivo, e
 *   abbastanza economico da girare in un lampo anche sul ferro
 *   d'epoca che MainDOB governa.
 *
 * CONFRONTO -- dob_logon_check confronta a tempo costante (fold XOR
 *   dell'intero u64): il tempo di risposta non dipende da quanti
 *   caratteri combaciano. */

#ifndef MAINDOB_DOB_LOGON_H
#define MAINDOB_DOB_LOGON_H

#include <dob/types.h>
#include <string.h>

/* Percorso canonico del record. */
#define DOB_LOGON_PATH        "/SYSTEM/CONFIG/Logon_password.dat"

/* Lunghezza massima della password (byte, NUL escluso). Oltre si
 * tronca in input: 63 caratteri bastano a chiunque e tengono i buffer
 * su stack piccoli. */
#define DOB_LOGON_PW_MAX      63

/* Giri di rimescolamento post-FNV. */
#define DOB_LOGON_ROUNDS      4096u

/* Dimensione minima del buffer per dob_logon_format:
 * "MDLOGON1 " + 8 + " " + 16 + "\n" + NUL = 36. Arrotondata. */
#define DOB_LOGON_RECORD_MAX  40

/* ================= verbi esecutivi ==================================== */

/* Hash salato della password. Il salt entra per primo cosi' due
 * installazioni con la stessa password producono record diversi. */
static inline uint64_t dob_logon_hash(uint32_t salt, const char *password)
{
    /* FNV-1a 64 bit. */
    uint64_t h = 0xCBF29CE484222325ull;
    const uint64_t prime = 0x100000001B3ull;

    for (int i = 0; i < 4; i++)
    {
        h ^= (uint8_t)(salt >> (i * 8));
        h *= prime;
    }
    if (password)
    {
        for (const char *p = password; *p; p++)
        {
            h ^= (uint8_t)*p;
            h *= prime;
        }
    }

    /* Rimescolamento: xorshift + moltiplicazione, DOB_LOGON_ROUNDS
     * giri. Ogni giro dipende interamente dal precedente. */
    for (uint32_t r = 0; r < DOB_LOGON_ROUNDS; r++)
    {
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
        h += (uint64_t)salt | ((uint64_t)r << 32);
    }
    return h;
}

/* --- hex a mano: niente dipendenza da stdio nel percorso condiviso --- */

static inline void dob_logon_u32_hex(uint32_t v, char out[9])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        out[i] = hx[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = '\0';
}

static inline void dob_logon_u64_hex(uint64_t v, char out[17])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
        out[i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    out[16] = '\0';
}

/* -1 se il carattere non e' un hex digit. */
static inline int dob_logon_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ================= logica: record ===================================== */

/* Scrive in `out` (>= DOB_LOGON_RECORD_MAX byte) il record completo,
 * NUL-terminato. Ritorna la lunghezza in byte (senza NUL). */
static inline int dob_logon_format(char *out, uint32_t salt, uint64_t hash)
{
    char s[9], hh[17];
    dob_logon_u32_hex(salt, s);
    dob_logon_u64_hex(hash, hh);

    int n = 0;
    const char *magic = "MDLOGON1 ";
    while (magic[n]) { out[n] = magic[n]; n++; }
    memcpy(out + n, s, 8);  n += 8;
    out[n++] = ' ';
    memcpy(out + n, hh, 16); n += 16;
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

/* Comodo per chi crea il record da una password in chiaro: salt
 * fornito dal chiamante (random_u32() di libc, o l'entropia che
 * preferisce). */
static inline int dob_logon_make_record(char *out, uint32_t salt,
                                        const char *password)
{
    return dob_logon_format(out, salt, dob_logon_hash(salt, password));
}

/* Parse tollerante del record: accetta spazi multipli e ritorno a
 * capo finale. Ritorna true e riempie salt/hash solo su un record
 * MDLOGON1 ben formato; qualunque altra cosa (magic ignoto, hex
 * corto, spazzatura) e' false -- che il chiamante tratta come "nessuna
 * password" (fail-open dichiarato). */
static inline bool dob_logon_parse(const char *text, uint32_t *salt_out,
                                   uint64_t *hash_out)
{
    const char *p = text;
    const char *magic = "MDLOGON1";

    if (!p) return false;
    for (int i = 0; magic[i]; i++)
        if (*p++ != magic[i]) return false;
    if (*p != ' ') return false;
    while (*p == ' ') p++;

    uint32_t salt = 0;
    for (int i = 0; i < 8; i++)
    {
        int v = dob_logon_hexval(*p++);
        if (v < 0) return false;
        salt = (salt << 4) | (uint32_t)v;
    }
    if (*p != ' ') return false;
    while (*p == ' ') p++;

    uint64_t hash = 0;
    for (int i = 0; i < 16; i++)
    {
        int v = dob_logon_hexval(*p++);
        if (v < 0) return false;
        hash = (hash << 4) | (uint64_t)v;
    }
    /* Coda: solo whitespace ammesso. */
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p != '\0') return false;

    if (salt_out) *salt_out = salt;
    if (hash_out) *hash_out = hash;
    return true;
}

/* Verifica a tempo costante: hash del tentativo contro il record. */
static inline bool dob_logon_check(uint32_t salt, uint64_t stored_hash,
                                   const char *attempt)
{
    uint64_t h = dob_logon_hash(salt, attempt);
    uint64_t d = h ^ stored_hash;
    /* Fold: il compilatore non puo' cortocircuitare. */
    volatile uint32_t fold = (uint32_t)(d ^ (d >> 32));
    return fold == 0;
}

#endif /* MAINDOB_DOB_LOGON_H */
