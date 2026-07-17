#include "console/console.h"
#include "kernel.h"
#include "lib/format.h"
#include "lib/string.h"
#include "arch/x86/ports.h"
#include "arch/x86/cpu.h"
#include "sync/spinlock.h"
#include "mm/mmio_map.h"

/* ======================================================================
 * Stato
 * ==================================================================== */

/* Buffer VGA all'indirizzo standard, visto dall'higher half:
 * KERNEL_VMA + 0xB8000 (dentro i BOOT_DIRECT_MAP_MB mappati dal boot). */
#define VGA_BUFFER  ((volatile uint16_t *)(KERNEL_VMA + 0xB8000u))
#define VGA_WIDTH   80u
#define VGA_HEIGHT  25u

#define COM1_BASE   0x3F8u
#define COM1_BAUD   115200u

static uint32_t   s_row;
static uint32_t   s_col;
static uint8_t    s_attr;
static bool       s_serial_ok;
static spinlock_t s_lock = SPINLOCK_INIT;

/* ======================================================================
 * Verbi VGA
 * ==================================================================== */

static inline uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

/* Cursore hardware: ~4 outb, microsecondi su ferro vero. Va chiamato
 * una volta a fine messaggio, non per carattere (ottimizzazione gia'
 * collaudata nell'1.0: ~320 µs risparmiati su una riga da 80). */
static void vga_sync_cursor(void)
{
    uint16_t pos = (uint16_t)(s_row * VGA_WIDTH + s_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_scroll_one_row(void)
{
    /* memmove obbligatorio (non memcpy): sorgente e destinazione si
     * sovrappongono di VGA_WIDTH celle. Il rep movsl sottostante e'
     * molto piu' rapido del loop per-cella sul percorso VRAM lento. */
    memmove((void *)VGA_BUFFER,
            (const void *)(VGA_BUFFER + VGA_WIDTH),
            VGA_WIDTH * (VGA_HEIGHT - 1) * sizeof(uint16_t));

    uint16_t blank = vga_entry(' ', s_attr);
    volatile uint16_t *last = VGA_BUFFER + (VGA_HEIGHT - 1) * VGA_WIDTH;
    for (uint32_t x = 0; x < VGA_WIDTH; x++)
    {
        last[x] = blank;
    }
}

/* Scrive un carattere nel buffer VGA SENZA toccare il cursore hardware
 * e senza lock: i chiamanti serializzano (o sono in panic). */
static void vga_emit(char c)
{
    if (c == '\n')
    {
        s_col = 0;
        s_row++;
    }
    else if (c == '\r')
    {
        s_col = 0;
    }
    else if (c == '\t')
    {
        s_col = (s_col + 8) & ~7u;
    }
    else if (c == '\b')
    {
        if (s_col > 0)
        {
            s_col--;
            VGA_BUFFER[s_row * VGA_WIDTH + s_col] = vga_entry(' ', s_attr);
        }
    }
    else
    {
        VGA_BUFFER[s_row * VGA_WIDTH + s_col] = vga_entry(c, s_attr);
        s_col++;
    }

    if (s_col >= VGA_WIDTH)
    {
        s_col = 0;
        s_row++;
    }
    while (s_row >= VGA_HEIGHT)
    {
        vga_scroll_one_row();
        s_row--;
    }
}

/* ======================================================================
 * Verbi seriale (COM1)
 * ==================================================================== */

/* Attesa THR-empty LIMITATA: su UART assente/incastrato si molla dopo
 * ~64k letture invece di appendere il kernel (l'1.0 attendeva per
 * sempre). Il carattere si perde: meglio di un boot congelato. */
static bool serial_wait_tx_ready(void)
{
    for (uint32_t guard = 0; guard < 65536u; guard++)
    {
        if (inb(COM1_BASE + 5) & 0x20)
        {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static void serial_emit(char c)
{
    if (!s_serial_ok)
    {
        return;
    }
    if (c == '\n')
    {
        if (serial_wait_tx_ready())
        {
            outb(COM1_BASE, '\r');          /* terminali seri vogliono CRLF */
        }
    }
    if (!serial_wait_tx_ready())
    {
        s_serial_ok = false;                /* UART morto: mirror spento */
        return;
    }
    outb(COM1_BASE, (uint8_t)c);
}

/* Programmazione UART: 115200 8N1, FIFO on, poi self-test in loopback
 * (MCR bit 4): un byte spedito deve tornare identico sulla RX. Se non
 * torna, li' non c'e' un 16550 funzionante e il mirror resta spento —
 * niente scritture nel vuoto ne' false attese su hardware senza COM1. */
static bool serial_bring_up(void)
{
    uint16_t divisor = (uint16_t)(115200u / COM1_BAUD);    /* = 1 */

    outb(COM1_BASE + 1, 0x00);              /* interrupt UART spenti     */
    outb(COM1_BASE + 3, 0x80);              /* DLAB on                   */
    outb(COM1_BASE + 0, (uint8_t)(divisor & 0xFF));
    outb(COM1_BASE + 1, (uint8_t)(divisor >> 8));
    outb(COM1_BASE + 3, 0x03);              /* 8N1, DLAB off             */
    outb(COM1_BASE + 2, 0xC7);              /* FIFO on, soglia 14, reset */

    outb(COM1_BASE + 4, 0x1E);              /* loopback + OUT1/OUT2/RTS  */
    outb(COM1_BASE + 0, 0xAE);              /* byte sonda                */
    if (inb(COM1_BASE + 0) != 0xAE)
    {
        return false;
    }

    outb(COM1_BASE + 4, 0x0B);              /* modo normale: DTR/RTS/OUT2 */
    return true;
}

/* ======================================================================
 * Emissione combinata + sink per il formatter
 * ==================================================================== */

static void console_emit(char c)
{
    vga_emit(c);
    serial_emit(c);
}

static void console_sink(char c, void *ctx UNUSED)
{
    console_emit(c);
}

/* ======================================================================
 * API
 * ==================================================================== */

void console_init(void)
{
    s_row  = 0;
    s_col  = 0;
    s_attr = (uint8_t)((CON_BLACK << 4) | CON_LIGHT_GREY);
    s_serial_ok = serial_bring_up();
    console_clear();
}

void console_clear(void)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);

    uint16_t blank = vga_entry(' ', s_attr);
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
    {
        VGA_BUFFER[i] = blank;
    }
    s_row = 0;
    s_col = 0;
    vga_sync_cursor();

    spinlock_release_irqrestore(&s_lock, flags);
}

void console_set_color(con_color_t fg, con_color_t bg)
{
    s_attr = (uint8_t)(((uint8_t)bg << 4) | (uint8_t)fg);
}

void console_putchar(char c)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    console_emit(c);
    vga_sync_cursor();
    spinlock_release_irqrestore(&s_lock, flags);
}

void console_write(const char *s)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);
    while (*s)
    {
        console_emit(*s++);
    }
    vga_sync_cursor();
    spinlock_release_irqrestore(&s_lock, flags);
}

bool console_serial_present(void)
{
    return s_serial_ok;
}

void kprintf(const char *fmt, ...)
{
    uint32_t flags = spinlock_acquire_irqsave(&s_lock);

    va_list args;
    va_start(args, fmt);
    format_output(console_sink, NULL, fmt, args);
    va_end(args);

    vga_sync_cursor();                      /* una sync a messaggio */
    spinlock_release_irqrestore(&s_lock, flags);
}

/* ======================================================================
 * kpanic — dichiarata in kernel.h, vive qui (usa formatter + device)
 * ==================================================================== */

/* === Panic: disegno del testo nel FRAMEBUFFER attivo ================== *
 *
 * Con la GUI attiva lo schermo e' un framebuffer (BGA/LFB), non il testo
 * VGA a 0xB8000. Invece di cambiare modo video (fragile e specifico per
 * scheda), si disegna il testo del panic direttamente nei pixel del
 * framebuffer GIA' acceso: il driver video registra indirizzo e geometria
 * via SYS_SET_PANIC_FB e il kernel lo mappa in VA propria (mmio_map). Se
 * non c'e' framebuffer (panic al boot) si ripiega sul testo VGA. Font 8x16
 * in g_panic_font. Percorso panic: IF gia' a 0, lock console ignorato
 * (riprenderlo sarebbe deadlock, e non si torna piu' indietro). */

#define PANIC_FB_BG   0x00AA0000u          /* rosso   */
#define PANIC_FB_FG   0x00FFFFFFu          /* bianco  */

extern const uint8_t g_panic_font[256][16];

struct panic_fb
{
    volatile uint8_t *base;                /* VA kernel del framebuffer     */
    uint32_t          width;
    uint32_t          height;
    uint32_t          pitch;               /* byte per riga                 */
    uint32_t          bpp;                 /* 32 / 24 / 16                  */
    bool              valid;
};
static struct panic_fb s_pfb;
static uint32_t s_fb_row;                  /* cursore, in celle carattere   */
static uint32_t s_fb_col;

/* --- Registrazione: chiamata dalla syscall del driver video ----------- */

void console_register_panic_fb(uint32_t phys, uint32_t width, uint32_t height,
                               uint32_t pitch, uint32_t bpp)
{
    s_pfb.valid = false;
    if (phys == 0 || width == 0 || height == 0 || pitch == 0)
    {
        return;
    }
    if (bpp != 32 && bpp != 24 && bpp != 16)
    {
        return;
    }

    uint32_t vv = 0;
    uint32_t pp = 0;
    void *base = mmio_map((uint64_t)phys, height * pitch, true, &vv, &pp);
    if (base == NULL)
    {
        return;
    }

    s_pfb.base   = (volatile uint8_t *)base;
    s_pfb.width  = width;
    s_pfb.height = height;
    s_pfb.pitch  = pitch;
    s_pfb.bpp    = bpp;
    s_pfb.valid  = true;
}

/* --- Blocchi esecutivi: un pixel, un riempimento, un glifo ------------ */

static void fb_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= s_pfb.width || y >= s_pfb.height)
    {
        return;
    }
    volatile uint8_t *p = s_pfb.base + (uint32_t)y * s_pfb.pitch
                                     + x * (s_pfb.bpp >> 3);
    if (s_pfb.bpp == 32)
    {
        *(volatile uint32_t *)p = rgb;
    }
    else if (s_pfb.bpp == 24)
    {
        p[0] = (uint8_t)(rgb);
        p[1] = (uint8_t)(rgb >> 8);
        p[2] = (uint8_t)(rgb >> 16);
    }
    else /* 16 bpp, 5-6-5 */
    {
        uint8_t r = (uint8_t)(rgb >> 16);
        uint8_t g = (uint8_t)(rgb >> 8);
        uint8_t b = (uint8_t)(rgb);
        *(volatile uint16_t *)p =
            (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

static void fb_fill(uint32_t rgb)
{
    for (uint32_t y = 0; y < s_pfb.height; y++)
    {
        for (uint32_t x = 0; x < s_pfb.width; x++)
        {
            fb_put_pixel(x, y, rgb);
        }
    }
}

static void fb_draw_glyph(uint32_t px, uint32_t py, unsigned char ch,
                          uint32_t fg, uint32_t bg)
{
    for (uint32_t row = 0; row < 16; row++)
    {
        uint8_t bits = g_panic_font[ch][row];
        for (uint32_t col = 0; col < 8; col++)
        {
            fb_put_pixel(px + col, py + row,
                         (bits & (0x80u >> col)) ? fg : bg);
        }
    }
}

/* --- Orchestratore: emette UN carattere nel framebuffer -------------- */

static void fb_emit(char c)
{
    uint32_t cols = s_pfb.width / 8;
    uint32_t rows = s_pfb.height / 16;

    if (c == '\n')
    {
        s_fb_col = 0;
        if (s_fb_row + 1 < rows) { s_fb_row++; }
        return;
    }
    if (c == '\r')
    {
        s_fb_col = 0;
        return;
    }
    if (s_fb_col >= cols)
    {
        s_fb_col = 0;
        if (s_fb_row + 1 < rows) { s_fb_row++; }
    }
    fb_draw_glyph(s_fb_col * 8, s_fb_row * 16,
                  (unsigned char)c, PANIC_FB_FG, PANIC_FB_BG);
    s_fb_col++;
}

/* === Panic: emissione ================================================= */

/* Instrada un carattere: SEMPRE seriale; a schermo nel framebuffer se
 * registrato, altrimenti nel testo VGA (panic al boot, GUI non attiva). */
static void panic_emit(char c)
{
    serial_emit(c);
    if (s_pfb.valid)
    {
        fb_emit(c);
    }
    else
    {
        vga_emit(c);
    }
}

static void panic_sink(char c, void *ctx UNUSED)
{
    panic_emit(c);
}

/* Orchestratore top-level del panic: prepara lo schermo, emette
 * intestazione + messaggio + coda, poi ferma la macchina. */
void kpanic(const char *fmt, ...)
{
    cpu_cli();

    s_attr = (uint8_t)((CON_RED << 4) | CON_WHITE);
    if (s_pfb.valid)
    {
        fb_fill(PANIC_FB_BG);
        s_fb_row = 0;
        s_fb_col = 0;
    }

    const char *head = "\n === KERNEL PANIC ===\n ";
    for (const char *p = head; *p; p++)
    {
        panic_emit(*p);
    }

    va_list args;
    va_start(args, fmt);
    format_output(panic_sink, NULL, fmt, args);
    va_end(args);

    const char *tail = "\n Sistema arrestato.\n";
    for (const char *p = tail; *p; p++)
    {
        panic_emit(*p);
    }

    cpu_halt_forever();
}
