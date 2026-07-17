#include "krt/entropy.h"
#include "arch/x86/tsc.h"
#include "arch/x86/pit.h"
#include "arch/x86/cpu.h"
#include "console/console.h"

static uint32_t s_state[4];
static uint32_t s_mix_index;

/* === Verbi ============================================================== */

static uint32_t rotl(uint32_t v, int r)
{
    return (v << r) | (v >> (32 - r));
}

static uint32_t xorshift128(void)
{
    uint32_t t = s_state[3];
    uint32_t s = s_state[0];
    s_state[3] = s_state[2];
    s_state[2] = s_state[1];
    s_state[1] = s;

    t ^= t << 11;
    t ^= t >> 8;
    s_state[0] = t ^ s ^ (s >> 19);
    return s_state[0];
}

/* === API ================================================================ */

void entropy_init(void)
{
    uint64_t seed = tsc_read();
    s_state[0] = (uint32_t)seed ^ 0xD0B00001u;
    s_state[1] = (uint32_t)(seed >> 32) ^ 0x9E3779B9u;
    s_state[2] = (uint32_t)(tsc_read() >> 11) ^ 0x85EBCA6Bu;
    s_state[3] = 0xC2B2AE35u;

    if ((s_state[0] | s_state[1] | s_state[2] | s_state[3]) == 0)
    {
        s_state[0] = 1;                 /* xorshift: mai stato tutto-zero */
    }
    kprintf("[RNG ] Pool di entropia inizializzato (TSC%s).\n",
            tsc_hz() ? "" : " assente: solo tick");
}

void entropy_add(uint32_t sample)
{
    uint32_t idx = s_mix_index++ & 3u;
    s_state[idx] ^= rotl(sample, (int)(s_mix_index % 31u) + 1);
    if ((s_state[0] | s_state[1] | s_state[2] | s_state[3]) == 0)
    {
        s_state[0] = 1;
    }
}

void entropy_get_bytes(void *buf, uint32_t len)
{
    uint8_t *out = (uint8_t *)buf;

    entropy_add((uint32_t)tsc_read());  /* jitter fresco a ogni richiesta */

    uint32_t fl = irq_save();
    while (len >= 4)
    {
        uint32_t v = xorshift128();
        out[0] = (uint8_t)v;
        out[1] = (uint8_t)(v >> 8);
        out[2] = (uint8_t)(v >> 16);
        out[3] = (uint8_t)(v >> 24);
        out += 4;
        len -= 4;
    }
    if (len > 0)
    {
        uint32_t v = xorshift128();
        for (uint32_t i = 0; i < len; i++)
        {
            out[i] = (uint8_t)(v >> (8 * i));
        }
    }
    irq_restore(fl);
}
