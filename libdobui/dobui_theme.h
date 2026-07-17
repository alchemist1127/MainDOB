/* libdobui/dobui_theme.h
 *
 * mainDOB — palette UI centralizzata.
 * UNICA fonte di verita' per i colori dei widget DobUITools.
 * Vedi docs/STYLE_PROTOCOL.md.
 *
 * Formato: 0x00RRGGBB (byte alto ignorato dall'engine).
 *
 * Regola d'oro: il colore = significato.
 *   giallo  = roba dell'utente (testo digitato + azione/focus primario)
 *   ciano   = testo fisso / etichette
 *   bianco  = testo fisso secondario
 *   rosso   = errore / distruttivo / allarme
 *   verde   = riempimenti "sani" / ready / switch OFF (mai testo)
 */

#ifndef DOBUI_THEME_H
#define DOBUI_THEME_H

/* --- Blu --- */
#define DOBUI_SURFACE   0x000000AA  /* blu 170 - sfondo finestre/pannelli      */
#define DOBUI_RELIEF    0x000000FF  /* blu 255 - cornici, bordi, title bar     */

/* --- Inserto --- */
#define DOBUI_INSET     0x00000000  /* nero - campi, aree-dati, sfondo bottoni */

/* --- Testo --- */
#define DOBUI_INPUT     0x00FFFF00  /* giallo puro - testo utente + azione/focus */
#define DOBUI_TEXT      0x0000FFFF  /* ciano  - testo fisso primario/etichette */
#define DOBUI_TEXT_ALT  0x00FFFFFF  /* bianco - testo fisso secondario         */

/* --- Semantici --- */
#define DOBUI_DANGER    0x00FF3333  /* rosso  - errore/distruttivo/allarme     */
#define DOBUI_SUCCESS   0x0000CC33  /* verde  - riempimenti sani/ready/OFF     */
#define DOBUI_DISABLED  0x00556699  /* blu-grigio - elementi disabilitati      */

/* --- Opzionale, raro, fuori standard --- */
#define DOBUI_SPECIAL   0x00FF44AA  /* magenta                                 */

#endif /* DOBUI_THEME_H */
