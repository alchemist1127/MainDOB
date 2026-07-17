/* dobui_cmdbuf -- DobInterface command buffer wire format.
 *
 * The drawing primitives (FillRect/DrawText/DrawRect/DrawPixel/BlitBuffer)
 * exposed by libdobui are non longer immediate writes to a shared
 * framebuffer.  Each call appends a record to a per-context cmdbuf
 * kept by the client stub.  At Invalidate the whole cmdbuf is shipped
 * to dobinterface as the payload of a single GUI_WIN_INVALIDATE /
 * GUI_WIDGET_INVALIDATE async post.  dobinterface resets the window's
 * dv_cmdlist and replays each record as the corresponding dv_cmdlist_*
 * primitive.
 *
 * This header is the only ABI shared between the two sides; it must
 * stay byte-stable.  When a new opcode is added, append it at the end
 * of the enum (do not renumber).  The magic word and reserved field
 * give room for one explicit ABI break later if needed -- bump magic.
 *
 * === Payload layout ========================================================
 *
 *   offset 0  u32  magic     = DOBUI_CMDBUF_MAGIC
 *   offset 4  u32  reserved  = 0   (future capability flags)
 *   offset 8  u8[] records, packed back-to-back, no alignment, no
 *                  terminator -- the server iterates until the byte
 *                  offset equals payload_size.
 *
 * === Records ===============================================================
 *
 * All multi-byte fields are little-endian.  No alignment padding inside
 * or between records.  Coordinates are int16; sizes uint16; colors
 * uint32 in 0x00RRGGBB form (alpha byte ignored, opacity is decided
 * by the layer's per-pixel-alpha flag server-side).
 *
 *   OP_FILL_RECT   (1)  u8 op, i16 x, i16 y, u16 w, u16 h, u32 color
 *                       -- 13 byte
 *
 *   OP_DRAW_RECT   (2)  u8 op, i16 x, i16 y, u16 w, u16 h, u32 color
 *                       -- 13 byte (same shape as FILL_RECT, server emits
 *                       four thin fill_rects for the four edges)
 *
 *   OP_DRAW_PIXEL  (3)  u8 op, i16 x, i16 y, u32 color
 *                       -- 9 byte
 *
 *   OP_DRAW_TEXT   (4)  u8 op, i16 x, i16 y, u32 fg, u32 bg,
 *                       u16 len, u8 text[len]
 *                       -- 13 + len byte.  No NUL terminator.  Server
 *                       routes to dv_cmdlist_draw_glyphs against the
 *                       compositor's pre-uploaded glyph atlas.  A
 *                       u16 len caps text records at 64 KiB, which is
 *                       way past any realistic single label.
 *
 *   OP_BLIT_INLINE (5)  u8 op, i16 x, i16 y, u16 src_w, u16 src_h,
 *                       u32 pixels[src_w * src_h]
 *                       -- 9 + 4*w*h byte.  Only emitted when
 *                       4*w*h <= DOBUI_BLIT_INLINE_THRESHOLD_BYTES.
 *                       Server uploads the pixels into a scratch
 *                       texture and emits a dv_cmdlist_blit_alpha (for
 *                       transparency-aware sources, magic value
 *                       0xFF000000 = transparent kept for legacy
 *                       BlitBuffer semantics).
 *
 *   OP_BLIT_TEX    (6)  u8 op, i16 x, i16 y, u32 texture_handle,
 *                       u16 src_w, u16 src_h
 *                       -- 13 byte.  Texture allocated server-side via
 *                       GUI_WIN_TEX_ALLOC and uploaded via
 *                       GUI_WIN_TEX_UPDATE; the handle in this record
 *                       identifies a previously-allocated entry in the
 *                       window's texture table.  Server emits a
 *                       dv_cmdlist_blit_alpha referencing that texture.
 *
 * Opcodes 0 and 7..255 are reserved.  The server treats unknown
 * opcodes as fatal format errors and drops the cmdbuf (frame renders
 * nothing for that window).  Client never emits anything but the
 * defined six.
 */

#ifndef MAINDOB_DOB_DOBUI_CMDBUF_H
#define MAINDOB_DOB_DOBUI_CMDBUF_H

#include <dob/types.h>

/* 'CBUF' in little-endian: byte0='C' (0x43), byte1='B' (0x42),
 * byte2='U' (0x55), byte3='F' (0x46) -> read as LE u32 = 0x46554243. */
#define DOBUI_CMDBUF_MAGIC          0x46554243u
#define DOBUI_CMDBUF_HDR_SIZE       8u

/* Trasporto segmentato del cmdbuf (Invalidate).
 *
 * Il buffer IPC del ricevente e' una risorsa finita (IPC_BUF_SIZE, ABI
 * 1.0 = 64 KiB): un payload che la eccede viene STRIPPATO in consegna
 * (il ricevente vede payload nullo). Un cmdbuf cresce con il contenuto
 * della finestra e puo' legittimamente superare quel tetto — quindi il
 * flush lo spedisce a SEGMENTI, ognuno ben sotto il limite:
 *
 *   arg0 = id finestra/widget (come sempre)
 *   arg1 = indice segmento, 0-based
 *   arg2 = numero totale di segmenti (0 = messaggio legacy monoblocco)
 *   arg3 = byte totali del cmdbuf completo
 *   payload = fetta grezza del cmdbuf (il segmento 0 inizia col magic)
 *
 * Il ricevente riassembla per id; un buco nella sequenza (segmento
 * perso su porta piena) scarta il parziale e riparte dal prossimo
 * segmento 0 — il frame successivo risana da solo. */
#define DOBUI_CMDBUF_SEG_BYTES      32768u

/* Opcode values.  Stable -- new ops APPEND, never renumber. */
#define DOBUI_OP_FILL_RECT          1u
#define DOBUI_OP_DRAW_RECT          2u
#define DOBUI_OP_DRAW_PIXEL         3u
#define DOBUI_OP_DRAW_TEXT          4u
#define DOBUI_OP_BLIT_INLINE        5u
#define DOBUI_OP_BLIT_TEX           6u
/* Same wire format as DOBUI_OP_DRAW_TEXT (reuses DOBUI_REC_DRAW_TEXT_HDR);
 * asks the compositor to lay the run out monospace (fixed pitch) instead
 * of the proportional default. Used by editable text fields whose cursor
 * geometry is cell-based. */
#define DOBUI_OP_DRAW_TEXT_FIXED    7u
/* Blit dal pannello SHM della finestra (vedi GUI_WIN_SHM_ENSURE):
 * i pixel NON viaggiano nel cmdbuf ne' in texture — vivono in un
 * buffer di memoria condivisa che l'app scrive e il server legge al
 * bake. Record: x,y (body-relative) + w,h del pannello + banda sporca
 * dichiarata dal client (band_y0, band_rows in coordinate pannello):
 *   band_rows == 0                 -> copia integrale;
 *   band_y0 == band_rows == 0xFFFF -> contenuto INVARIATO, zero copia
 *                                     (il corpo persiste fra i bake);
 *   altrimenti                     -> copia delle sole righe di banda.
 * Il server ignora la banda (copia integrale) finche' il corpo non e'
 * sincronizzato col pannello (primo bake, resize). E' il percorso a
 * copia singola — e con la banda, a copia MINIMA — per i contenuti
 * grandi e vivi (la pagina di un editor). */
#define DOBUI_OP_BLIT_SHMPANEL      8u
#define DOBUI_SHMPANEL_UNCHANGED    0xFFFFu

/* Fixed record sizes (in bytes).  DRAW_TEXT and BLIT_INLINE add a
 * variable payload to a fixed header; the *_HDR macros give the
 * header-only size for those two. */
#define DOBUI_REC_FILL_RECT_SZ      13u
#define DOBUI_REC_DRAW_RECT_SZ      13u
#define DOBUI_REC_DRAW_PIXEL_SZ     9u
#define DOBUI_REC_DRAW_TEXT_HDR     13u
#define DOBUI_REC_BLIT_INLINE_HDR   9u
#define DOBUI_REC_BLIT_TEX_SZ       13u
#define DOBUI_REC_BLIT_SHMPANEL_SZ  13u

/* BlitBuffer dispatch threshold (in bytes of pixel data, = 4*w*h).
 * At or below: inline the pixels in the cmdbuf via OP_BLIT_INLINE.
 * Above: allocate a server-side texture once, reuse the handle in
 * OP_BLIT_TEX from subsequent calls.
 *
 * 4 KiB sized for 32x32 icons (the common case in window-content
 * thumbnails and palette previews).  Buffers bigger than this are
 * rare and benefit from staying server-side. */
/* BlitBuffer dispatch threshold.
 *
 * History: previously rasters at-or-below this size took an "inline"
 * fast path — pixels embedded in the cmdlist, server uploads to a
 * global scratch texture on replay, queues a blit referencing it.
 * That fast path is fundamentally racy: window B's replay can
 * overwrite the scratch slots window A's GPU cmdlist still
 * references, so A's icons turn into B's data on next composite
 * (visible as "anomalies on the non-active window's icons").
 *
 * Fix: threshold lowered to 0 so EVERY BlitBuffer call goes through
 * the texture-pool path. Each distinct raster gets its own
 * per-window texture handle, persistent across replays — no scratch,
 * no races. Cost is one handle per icon (modest), benefit is that
 * cmdlist replay just blits an existing texture instead of
 * uploading every frame, which is also faster.
 *
 * The opcode and inline-pump infrastructure are kept for now in case
 * a future use case wants them (e.g. one-shot diagnostic raster), but
 * no caller hits this path under normal operation. */
#define DOBUI_BLIT_INLINE_THRESHOLD_BYTES   0u

/* Initial capacity of the per-context cmdbuf in the client stub.
 * Grows by doubling up to DOBUI_CMDBUF_MAX_BYTES on append; a single
 * Invalidate over this cap is treated as overflow and the buffer is
 * dropped with a debug warning (sanity guard against runaway
 * cmdbufs, e.g. an app accidentally redraws in a tight loop). */
#define DOBUI_CMDBUF_INITIAL_BYTES   8192u
#define DOBUI_CMDBUF_MAX_BYTES       (1u * 1024u * 1024u)

#endif /* MAINDOB_DOB_DOBUI_CMDBUF_H */
