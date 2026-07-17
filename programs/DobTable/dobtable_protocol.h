/* DobTable IPC protocol — opcodes and wire formats.
 *
 * The caller spawns DobTable.mdl with a unique service name in argv[0].
 * DobTable registers itself under that name and accepts the opcodes
 * below until the user closes the window or DOBTABLE_CLOSE is sent.
 *
 * Wire is the standard dob_msg_t: arg0..arg3 + payload.  Payload
 * strings are NUL-terminated; multi-string payloads pack them
 * back-to-back.  The widget owns a copy of every string, so the
 * caller's buffers can be reused or freed immediately after the call
 * returns. */

#ifndef MAINDOB_DOBTABLE_PROTOCOL_H
#define MAINDOB_DOBTABLE_PROTOCOL_H

/* Capacity caps — enforced server-side, return DOB_ERR_INVALID
 * when the caller would exceed them. */
#define DOBTABLE_MAX_ROWS         256
#define DOBTABLE_MAX_KEY_LEN       64    /* including NUL */
#define DOBTABLE_MAX_VALUE_LEN    256    /* including NUL */
#define DOBTABLE_MAX_TITLE_LEN     64    /* including NUL */
#define DOBTABLE_MAX_HEADER_LEN    64    /* including NUL, per header */

/* Default window size. The caller cannot resize it for now — keep
 * tables compact, that is what they are for. */
#define DOBTABLE_WIN_W            480
#define DOBTABLE_WIN_H            400

/* Opcodes — range 400..409 reserved for DobTable.  All synchronous
 * (the client should use UI_CALL-style send/wait). */
#define DOBTABLE_SET_TITLE        400  /* payload: title\0                          */
#define DOBTABLE_SET_HEADERS      401  /* payload: key_h\0value_h\0                 */
#define DOBTABLE_ADD_ROWS         402  /* arg0=count, payload: (key\0value\0)*count */
#define DOBTABLE_CLEAR            403  /* no args                                   */
#define DOBTABLE_SHOW             404  /* no args — paints, raises, focuses        */
#define DOBTABLE_CLOSE            405  /* no args — server replies then _exit(0)   */

#endif
