#ifndef MAINDOB_CONSOLE_CONSOLE_H
#define MAINDOB_CONSOLE_CONSOLE_H

#include "lib/types.h"

/* Console del kernel 1.1: VGA text 80x25 + mirror seriale COM1
 * 115200 8N1 (canale di debug primario su Armada E500 / Extensa 5220,
 * dove i log a schermo scorrono via prima che si riesca a leggerli).
 *
 * Differenze rispetto all'1.0 (lib/printf.c):
 *  - il formatter e' fuori, in lib/format.h (componente condiviso);
 *  - la seriale va a 115200 (l'1.0 era a 38400) con self-test in
 *    loopback all'init: se l'UART non c'e' (o e' rotto) il mirror si
 *    disattiva da solo, invece di scrivere nel vuoto;
 *  - la TX seriale ha un'attesa LIMITATA: un UART incastrato non puo'
 *    piu' appendere un kprintf (l'1.0 attendeva THR-empty per sempre);
 *  - kpanic emette SENZA prendere il lock console: un panic dentro
 *    kprintf non si autoblocca. */

/* Colori testo (valori = attributi VGA standard 0-15). */
typedef enum
{
    CON_BLACK         = 0,
    CON_BLUE          = 1,
    CON_GREEN         = 2,
    CON_CYAN          = 3,
    CON_RED           = 4,
    CON_MAGENTA       = 5,
    CON_BROWN         = 6,
    CON_LIGHT_GREY    = 7,
    CON_DARK_GREY     = 8,
    CON_LIGHT_BLUE    = 9,
    CON_LIGHT_GREEN   = 10,
    CON_LIGHT_CYAN    = 11,
    CON_LIGHT_RED     = 12,
    CON_LIGHT_MAGENTA = 13,
    CON_YELLOW        = 14,
    CON_WHITE         = 15,
} con_color_t;

/* Porta su VGA pulito + seriale collaudata. Prima chiamata di kmain. */
void console_init(void);

/* Pulisce lo schermo (attributo corrente). */
void console_clear(void);

/* Colore testo/sfondo per le scritture successive. */
void console_set_color(con_color_t fg, con_color_t bg);

/* Scrittura diretta (gia' serializzata dal lock console). */
void console_putchar(char c);
void console_write(const char *s);

/* La seriale ha superato il loopback all'init? (diagnostica) */
bool console_serial_present(void);

/* printf del kernel: %d %i %u %x %X %p %s %c %%, flag '0', larghezza.
 * Thread-safe (spinlock irqsave), cursore aggiornato una volta a
 * chiamata. Il mirror seriale converte '\n' in "\r\n". */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));


/* Registra il framebuffer attivo (VA fisica + geometria) per il disegno del
 * panic. Chiamata dal driver video via SYS_SET_PANIC_FB. */
void console_register_panic_fb(uint32_t phys, uint32_t width, uint32_t height,
                               uint32_t pitch, uint32_t bpp);

#endif /* MAINDOB_CONSOLE_CONSOLE_H */
