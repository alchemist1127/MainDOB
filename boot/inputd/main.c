/* MainDOB Input Daemon — Centralized PS/2 Driver
 *
 * Sole owner of the PS/2 controller (ports 0x60, 0x64).
 * IRQ1 (keyboard) + IRQ12 (mouse) on a single IPC port.
 *
 * Pure event-driven, zero buffers, zero timers.
 *
 * Mouse events are coalesced per drain cycle: all PS/2 packets
 * received in one IRQ wakeup are summed into a single IPC message.
 *
 * Protocol (to dobinterface):
 *   code=50  INPUT_MOUSE   arg0=buttons arg1=dx arg2=dy arg3=scroll
 *   code=51  INPUT_KEY     arg0=key_code
 *   code=1   SUBSCRIBE     arg0=target_port (dobinterface tells us where)
 */

#include <unistd.h>
#include <string.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/input_layout.h>

/* Constants */

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define INPUT_MOUSE     50
#define INPUT_KEY       51
#define INPUT_MODCHANGE 52
#define INPUT_SUBSCRIBE 1

#define INPUT_MOD_CTRL  0x01
#define INPUT_MOD_SHIFT 0x02

#define SKEY_UP     128
#define SKEY_DOWN   129
#define SKEY_LEFT   130
#define SKEY_RIGHT  131
#define SKEY_HOME   132
#define SKEY_END    133
#define SKEY_DELETE 134
#define SKEY_PGUP   135
#define SKEY_PGDN   136
#define SKEY_PRTSC     137

/* Active keyboard layout.
 *
 * Initialised to the US layout so the very first keystrokes work
 * before keymap (the tray applet) pushes the user-selected layout:
 * inputd starts well before the filesystem, so this compiled-in
 * table is the boot-time guarantee and the permanent fallback.
 *
 * keymap replaces it at runtime via INPUT_SETLAYOUT. The altgr
 * table is zero here -- the US layout has no AltGr level. */
static kbd_layout_t layout =
{
    .normal =
    {
        0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    },
    .shift =
    {
        0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    },
};

/* State */

static uint32_t target_port = 0;
static uint32_t my_port = 0;

/* Keyboard modifiers */
static bool kbd_shift = false;
static bool kbd_ctrl = false;
static bool kbd_caps = false;
static bool kbd_altgr = false;
static bool kbd_extended = false;
static bool kbd_alt      = false;   /* Alt SINISTRO (scancode 0x38 non
                                     * esteso; AltGr = E0 38 e' a parte:
                                     * quello seleziona il layout, questo
                                     * serve solo alle combinazioni). */

/* Mouse packet assembly */
static uint8_t mouse_pkt[4];
static int     mouse_pkt_idx = 0;
static bool    mouse_has_wheel = false;

/* Per-drain mouse accumulator (reset every drain cycle) */
static int     drain_dx = 0;
static int     drain_dy = 0;
static int     drain_scroll = 0;
static uint8_t drain_buttons = 0;
static bool    drain_has_mouse = false;

/* Two-button scroll gesture: on hardware with no scroll wheel, holding
 * BOTH mouse buttons and moving up/down scrolls. State persists across
 * packets (the gesture spans many). gesture_accum collects raw vertical
 * motion; every GESTURE_SCROLL_DIV pixels emits one wheel tick. */
#define GESTURE_SCROLL_DIV  16
static bool    gesture_active = false;
static int     gesture_accum  = 0;

/* PS/2 helpers */

static void ps2_wait_in(void)
{
    for (int t = 100000; t > 0 && (io_inb(PS2_STATUS) & 0x02); t--);
}

static void ps2_wait_out(void)
{
    for (int t = 100000; t > 0 && !(io_inb(PS2_STATUS) & 0x01); t--);
}

static void ps2_cmd(uint8_t c)  { ps2_wait_in(); io_outb(PS2_CMD, c); }
static void ps2_data(uint8_t d) { ps2_wait_in(); io_outb(PS2_DATA, d); }
static uint8_t ps2_read(void)   { ps2_wait_out(); return io_inb(PS2_DATA); }

static void mouse_write(uint8_t val)
{
    ps2_cmd(0xD4);
    ps2_data(val);
    ps2_read();
}

/* Mouse init (once at startup) */

static void mouse_init(void)
{
    ps2_cmd(0xA8);
    ps2_cmd(0x20);
    uint8_t cfg = ps2_read();
    cfg |= 0x02;
    cfg &= (uint8_t)~0x20;
    ps2_cmd(0x60);
    ps2_data(cfg);

    mouse_write(0xFF);
    /* Wait for mouse reset — timer-based, zero CPU */
    {
        int tid = timer_set(my_port, 100, 0);
        dob_msg_t wm;
        memset(&wm, 0, sizeof(wm));
        dob_ipc_receive(my_port, &wm);
        if (wm.type == 3) { irq_done(1); irq_done(12); }
        (void)tid;
    }

    /* Flush stale bytes — only auxiliary (mouse) bytes */
    for (int i = 0; i < 16; i++)
    {
        uint8_t st = io_inb(PS2_STATUS);
        if (!(st & 0x01)) break;
        io_inb(PS2_DATA);
    }

    mouse_write(0xF6);
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);

    mouse_write(0xF2);
    /* Wait for mouse ID response — timer-based */
    {
        int tid = timer_set(my_port, 20, 0);
        dob_msg_t wm;
        memset(&wm, 0, sizeof(wm));
        dob_ipc_receive(my_port, &wm);
        if (wm.type == 3) { irq_done(1); irq_done(12); }
        (void)tid;
    }
    uint8_t id = 0;
    if (io_inb(PS2_STATUS) & 0x01)
        id = io_inb(PS2_DATA);

    mouse_has_wheel = (id == 3 || id == 4);
    mouse_write(0xF4);

    debug_print(mouse_has_wheel
        ? "[inputd] Scroll wheel detected.\n"
        : "[inputd] Standard mouse.\n");
}

/* Key delivery */

static void deliver_key(uint8_t key)
{
    if (target_port == 0) return;

    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code = INPUT_KEY;
    m.arg0 = key;
    dob_ipc_post(target_port, &m);
}

/* Notify dobinterface that the active modifier set (CTRL/SHIFT) changed.
 * Posted only on transitions, never repeated. dobinterface forwards the
 * message to the focused window so applications such as DobFiles can
 * track the modifier state without intercepting raw scancodes. */
static void deliver_modchange(void)
{
    if (target_port == 0) return;

    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code = INPUT_MODCHANGE;
    m.arg0 = (kbd_ctrl  ? INPUT_MOD_CTRL  : 0)
           | (kbd_shift ? INPUT_MOD_SHIFT : 0);
    dob_ipc_post(target_port, &m);
}

/* Keyboard processing */

static void process_keyboard(uint8_t sc)
{
    if (sc == 0xE0) { kbd_extended = true; return; }

    if (kbd_extended)
    {
        kbd_extended = false;

        /* AltGr = right Alt, scancode E0 38 (press) / E0 B8 (release).
         * Handled before the release-discard below because we need
         * both edges. AltGr only selects a translation table, so --
         * unlike Ctrl/Shift -- it is not broadcast via deliver_modchange(). */
        if (sc == 0x38) { kbd_altgr = true;  return; }
        if (sc == 0xB8) { kbd_altgr = false; return; }

        if (sc & 0x80) return;

        uint8_t key = 0;
        switch (sc)
        {
            case 0x48: key = SKEY_UP;     break;
            case 0x50: key = SKEY_DOWN;   break;
            case 0x4B: key = SKEY_LEFT;   break;
            case 0x4D: key = SKEY_RIGHT;  break;
            case 0x47: key = SKEY_HOME;   break;
            case 0x4F: key = SKEY_END;    break;
            case 0x53: key = SKEY_DELETE; break;
            case 0x49: key = SKEY_PGUP;   break;
            case 0x51: key = SKEY_PGDN;   break;
            /* Stamp R Sist (PrtScr): make = E0 2A, E0 37 — il fake
             * shift E0 2A cade gia' nel ramo "estenso ignoto" sopra;
             * qui arriva il 37. Consegnato come tasto speciale al
             * subscriber (dobinterface), che lo INTERCETTA prima del
             * routing alla finestra a fuoco e scatta l'istantanea:
             * il compositore possiede la scena, la cattura e'
             * driver-indipendente per costruzione. */
            case 0x37: key = SKEY_PRTSC;  break;
        }
        if (key) deliver_key(key);
        return;
    }

    if (sc == 0x2A || sc == 0x36) { kbd_shift = true;  deliver_modchange(); return; }
    if (sc == 0xAA || sc == 0xB6) { kbd_shift = false; deliver_modchange(); return; }
    if (sc == 0x1D)               { kbd_ctrl = true;   deliver_modchange(); return; }
    if (sc == 0x9D)               { kbd_ctrl = false;  deliver_modchange(); return; }
    if (sc == 0x38)               { kbd_alt = true;    return; }
    if (sc == 0xB8)               { kbd_alt = false;   return; }
    if (sc == 0x3A)               { kbd_caps = !kbd_caps; return; }
    if (sc & 0x80) return;
    if ((sc & 0x7F) == 0x01) { deliver_key(27); return; }

    uint8_t idx = (uint8_t)(sc & 0x7F);

    /* Ctrl+Alt+Shift+P = screenshot alternativo (per gli emulatori,
     * dove Stamp R Sist e' spesso rapito dall'host — es. QEMU su Mac).
     * Entrambi gli Alt valgono: option puo' presentarsi come Alt
     * sinistro (0x38) o AltGr (E0 38) a seconda del lato. Controllo a
     * livello di SCANCODE (0x19 = tasto P), prima della traduzione di
     * layout: la combinazione non dipende dalla mappa attiva. */
    if (kbd_ctrl && kbd_shift && (kbd_alt || kbd_altgr) && idx == 0x19)
    {
        deliver_key(SKEY_PRTSC);
        return;
    }

    uint8_t c;
    if (kbd_altgr && kbd_shift) c = layout.altgr_shift[idx];
    else if (kbd_altgr)         c = layout.altgr[idx];
    else if (kbd_shift)         c = layout.shift[idx];
    else                        c = layout.normal[idx];

    /* Caps Lock case folding. ASCII letters use the usual Caps^Shift rule.
     * Latin-1 accented letters are ALSO folded to uppercase under Caps — so
     * the accent keys yield À È É Ì Ò Ù Ç ..., a case almost no OS covers.
     * The accent fold is Shift-independent: the accent keys use Shift for the
     * OTHER accent (è vs é), not for case, so both levels uppercase
     * (è->È, é->É). 0xE0..0xFE are the Latin-1 lowercase letters; 0xF7 (÷) is
     * a math sign, not a letter. Uppercase forms are 0xC0..0xDE (-0x20). */
    if (!kbd_altgr && kbd_caps)
    {
        if (c >= 'a' && c <= 'z' && !kbd_shift)       c -= 32;
        else if (c >= 'A' && c <= 'Z' && kbd_shift)   c += 32;
        else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 32;
    }
    if (kbd_ctrl && c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 1);
    if (c) deliver_key(c);
}

/* Mouse processing — accumulates into drain-cycle locals */

static void process_mouse_byte(uint8_t byte)
{
    /* First byte: bit 3 must be set, no overflow */
    if (mouse_pkt_idx == 0)
    {
        if (!(byte & 0x08) || (byte & 0xC0))
        {
            /* Bad byte — discard and stay at idx 0.
             * No flush: let drain_ps2's normal routing handle
             * subsequent bytes correctly. This avoids the old bug
             * where a flush loop consumed keyboard bytes. */
            return;
        }
    }

    mouse_pkt[mouse_pkt_idx++] = byte;

    int pkt_size = mouse_has_wheel ? 4 : 3;
    if (mouse_pkt_idx < pkt_size)
        return;

    /* Packet complete — accumulate into drain-cycle locals */
    mouse_pkt_idx = 0;

    int16_t dx = (int16_t)mouse_pkt[1];
    int16_t dy = (int16_t)mouse_pkt[2];
    if (mouse_pkt[0] & 0x10) dx |= (int16_t)0xFF00;
    if (mouse_pkt[0] & 0x20) dy |= (int16_t)0xFF00;

    uint8_t new_buttons = mouse_pkt[0] & 0x07;

    /* Two-button scroll gesture (for hardware without a wheel): while
     * BOTH left and right buttons are held, vertical motion scrolls
     * instead of moving the cursor or firing button logic. We swallow
     * dx/dy and the button bits for the gesture's duration, turning dy
     * into wheel ticks.
     *
     * Sign: PS/2 dy is positive when the mouse moves UP. Elsewhere this
     * file does `drain_dy -= dy` (screen Y grows downward) and the wheel
     * field is taken as-is. To match a real wheel — push up = scroll up
     * = positive ticks — dy maps to scroll with the SAME sign as dy. We
     * accumulate and emit one tick per GESTURE_SCROLL_DIV pixels so a
     * small drag is one tick, matching wheel granularity. */
    bool both_down = (new_buttons & 0x01) && (new_buttons & 0x02);

    if (both_down)
    {
        if (!gesture_active) { gesture_active = true; gesture_accum = 0; }
        gesture_accum += dy;                 /* dy > 0 = moved up */
        int ticks = gesture_accum / GESTURE_SCROLL_DIV;
        if (ticks != 0)
        {
            drain_scroll += ticks;
            gesture_accum -= ticks * GESTURE_SCROLL_DIV;
            drain_has_mouse = true;
        }
        return;                              /* swallow movement+buttons */
    }
    if (gesture_active)
    {
        /* Gesture ended: reset, resync button state without a click. */
        gesture_active = false;
        gesture_accum  = 0;
        drain_buttons  = new_buttons;
        drain_has_mouse = true;
        return;
    }

    /* Button state changed — flush accumulated movement BEFORE updating.
     * Movement is additive (dx+dx'), but button state is a TRANSITION
     * that cannot be coalesced. If press and release happen in the same
     * drain cycle, the last one overwrites the first and the click is lost.
     * Flushing on every transition guarantees each press/release is seen. */
    if (drain_has_mouse && new_buttons != drain_buttons)
    {
        dob_msg_t m;
        memset(&m, 0, sizeof(m));
        m.code = INPUT_MOUSE;
        m.arg0 = drain_buttons;
        m.arg1 = (uint32_t)(int16_t)drain_dx;
        m.arg2 = (uint32_t)(int16_t)drain_dy;
        m.arg3 = (uint32_t)(int16_t)drain_scroll;
        if (target_port != 0)
            dob_ipc_post(target_port, &m);

        drain_dx = 0;
        drain_dy = 0;
        drain_scroll = 0;
    }

    drain_dx += dx;
    drain_dy -= dy;
    drain_buttons = new_buttons;

    if (mouse_has_wheel)
        drain_scroll += (int)(int8_t)mouse_pkt[3];

    drain_has_mouse = true;
}

/*  *  Drain — read all pending PS/2 bytes, coalesce mouse
 *
 *  Keys: delivered immediately (one IPC post per keystroke).
 *  Mouse: accumulated in drain_dx/dy/scroll, flushed once at end.
 *  This means N mouse packets = 1 IPC message. No queue flooding.
 */

static void drain_ps2(void)
{
    /* Reset per-drain MOVEMENT accumulators only.
     * Button state is NOT reset — it carries over from the last known
     * state. Resetting to 0 caused false release events at the start
     * of every drain when buttons were held down. */
    drain_dx = 0;
    drain_dy = 0;
    drain_scroll = 0;
    drain_has_mouse = false;

    for (int limit = 64; limit > 0; limit--)
    {
        uint8_t status = io_inb(PS2_STATUS);
        if (!(status & 0x01))
            break;

        uint8_t byte = io_inb(PS2_DATA);

        if (!(status & 0x20))
            process_keyboard(byte);
        else
            process_mouse_byte(byte);
    }

    /* Flush coalesced mouse event — one message for all packets */
    if (drain_has_mouse && target_port != 0)
    {
        dob_msg_t m;
        memset(&m, 0, sizeof(m));
        m.code = INPUT_MOUSE;
        m.arg0 = drain_buttons;
        m.arg1 = (uint32_t)(int16_t)drain_dx;
        m.arg2 = (uint32_t)(int16_t)drain_dy;
        m.arg3 = (uint32_t)(int16_t)drain_scroll;
        dob_ipc_post(target_port, &m);
    }
}

/* IPC request handling */

static void handle_request(dob_msg_t *msg)
{
    dob_msg_t reply;
    memset(&reply, 0, sizeof(reply));

    if (msg->code == INPUT_SUBSCRIBE && msg->arg0 != 0)
    {
        target_port = msg->arg0;
        debug_print("[inputd] Subscriber connected.\n");
    }
    else if (msg->code == INPUT_SETLAYOUT)
    {
        /* keymap pushes a new active layout. Copy it out of the IPC
         * payload only if the payload is at least the expected size;
         * a short or absent payload is rejected and the current
         * layout is kept. reply.arg0 reports the outcome. */
        if (msg->payload && msg->payload_size >= sizeof(kbd_layout_t))
        {
            memcpy(&layout, msg->payload, sizeof(kbd_layout_t));
            reply.arg0 = 1;
            debug_print("[inputd] Keyboard layout updated.\n");
        }
        else
        {
            debug_print("[inputd] INPUT_SETLAYOUT: bad payload, ignored.\n");
        }
    }

    dob_ipc_reply(msg->sender_tid, &reply);
}

/* Main */

int main(void)
{
    set_priority(1);
    debug_print("[inputd] Starting centralized input driver...\n");

    my_port = (uint32_t)port_create();
    irq_register(1, my_port);
    irq_register(12, my_port);

    mouse_init();

    /* Flush stale bytes from controller */
    while (io_inb(PS2_STATUS) & 0x01)
        io_inb(PS2_DATA);

    dob_registry_register("inputd", my_port);

    /* Drain periodico di sicurezza. Il kernel
     * MASCHERA la linea a ogni fire; su routing IOAPIC un fronte
     * edge-triggered che arriva mentre la RTE e' mascherata VA PERSO.
     * Finestra mortale: byte che entra nell'8042 fra l'ultimo check di
     * stato del drain e l'unmask post-irq_done -> fronte contro la
     * maschera -> all'unmask la linea e' gia' alta (OBF pieno) ->
     * niente nuovo fronte, MAI: input morto per sempre, e l'8042 non
     * accetta altri byte. Piu' mouse si muove, piu' finestre si aprono:
     * "funziona un po', poi muore". Questo timer da 500 ms fa da rete:
     * un byte incastrato viene drenato al giro dopo, il controller
     * abbassa la linea, il fronte successivo rinasce. Peggior caso:
     * un singhiozzo di mezzo secondo invece della morte. */
    timer_set(my_port, 500, 1);

    debug_print("[inputd] Ready.\n");

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(my_port, &msg);

        if (msg.type == 3)      /* IRQ notification */
        {
            drain_ps2();

            /* inputd owns both IRQ lines on this single port, and
             * drain_ps2() services keyboard AND mouse together (one shared
             * 8042 data port). So after a drain both lines are serviced no
             * matter which one's notify woke us. Ack BOTH unconditionally:
             * irq_done on a line that wasn't masked is a harmless no-op, and
             * this removes the latency trap where line 12's own notify lags
             * past the unmask safety window while its data was already
             * drained here — the cause of the repeating "line 12 irq_done
             * timeout, force-unmasking". */
            irq_done(1);
            irq_done(12);

            /* Re-drain after ACK (edge-triggered PIC) */
            drain_ps2();
        }
        else if (msg.code == 70) /* timer: drain di sicurezza */
        {
            drain_ps2();
            irq_done(1);            /* no-op se non mascherate */
            irq_done(12);
        }
        else if (msg.type == 1) /* IPC request */
        {
            handle_request(&msg);
        }
    }

    return 0;
}
