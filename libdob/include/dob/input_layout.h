/* MainDOB keyboard layout -- shared protocol between inputd and the
 * keymap tray applet.
 *
 * A keyboard layout is pure data: scancode (PS/2 set 1, low 7 bits)
 * mapped to a character, one table per modifier level. It carries no
 * code -- inputd owns the single translation routine, layouts only
 * change the table contents.
 *
 * keymap loads a layout from a .kbl data file, fills a kbd_layout_t,
 * and ships it to inputd in the payload of an INPUT_SETLAYOUT message.
 * inputd swaps its active table. */

#ifndef MAINDOB_DOB_INPUT_LAYOUT_H
#define MAINDOB_DOB_INPUT_LAYOUT_H

#include <dob/types.h>

/* PS/2 set-1 make codes use the low 7 bits; 128 entries cover them. */
#define KBD_LAYOUT_KEYS   128

/* Character values are byte codepoints in the shared 256-glyph font
 * space (ASCII + Latin-1/-9). Entry 0 means "this key produces no
 * character at this modifier level". */
typedef struct
{
    uint8_t normal     [KBD_LAYOUT_KEYS];   /* no modifier            */
    uint8_t shift      [KBD_LAYOUT_KEYS];   /* Shift held             */
    uint8_t altgr      [KBD_LAYOUT_KEYS];   /* AltGr (right Alt) held */
    uint8_t altgr_shift[KBD_LAYOUT_KEYS];   /* AltGr + Shift held     */
} kbd_layout_t;

/* inputd IPC request codes. INPUT_SUBSCRIBE (1) is defined in inputd;
 * INPUT_SETLAYOUT carries a kbd_layout_t in msg.payload (payload_size
 * == sizeof(kbd_layout_t)). The reply sets arg0 = 1 on success, 0 on
 * a malformed or undersized payload. */
#define INPUT_SETLAYOUT   2

#endif /* MAINDOB_DOB_INPUT_LAYOUT_H */
