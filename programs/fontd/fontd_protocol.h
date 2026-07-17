/* fontd -- IPC protocol between font clients and the font daemon.
 *
 * A client asks "which face for (family, weight, italic) that contains
 * this code point?"; fontd replies with a font file path + face index +
 * synthetic-style hints. The client opens/caches that file with
 * libdobfont and rasterizes locally (per-app atlas). Resolution policy
 * is centralized; pixels stay in the app -- the hybrid split.
 *
 * Structs are fixed-size so they marshal as a flat IPC payload.
 */

#ifndef FONTD_PROTOCOL_H
#define FONTD_PROTOCOL_H

#include <dob/types.h>

#define FONTD_SERVICE "fontd"

enum { FONTD_OP_RESOLVE = 1 };

typedef struct
{
    char     family[64];
    uint16_t weight;      /* 100..900 (400 normal, 700 bold); 0 -> 400 */
    uint8_t  italic;      /* 0 / 1 */
    uint8_t  _pad;
    uint32_t codepoint;   /* must be present in the chosen face; 0 = ignore */
} fontd_request_t;

typedef struct
{
    int32_t  status;      /* dob_status_t: DOB_OK on success */
    char     path[256];   /* font file to open */
    uint32_t face_index;  /* face within the file (.ttc / CFF) */
    float    embolden_em; /* synthetic bold per-em (multiply by pixel size) */
    float    slant;       /* synthetic oblique shear, 0 = upright */
} fontd_reply_t;

/* ---- optional header-only client ---- */
#ifndef FONTD_NO_CLIENT
#include <dob/registry.h>
#include <dob/ipc.h>
#include <string.h>

/* Resolve via the running fontd. Returns DOB_ERR_NOT_FOUND if the
 * service is not registered, otherwise the transport status; on DOB_OK
 * inspect reply->status for the resolution result. */
static inline dob_status_t fontd_resolve(const fontd_request_t *req,
                                         fontd_reply_t *reply)
{
    uint32_t port = dob_registry_find(FONTD_SERVICE);
    if (port == 0) return DOB_ERR_NOT_FOUND;          /* 0 = not registered */

    dob_msg_t msg; memset(&msg, 0, sizeof msg);
    msg.code         = FONTD_OP_RESOLVE;
    msg.payload      = (void *)req;
    msg.payload_size = sizeof(*req);

    dob_msg_t rep; memset(&rep, 0, sizeof rep);
    rep.payload      = reply;
    rep.payload_size = sizeof(*reply);

    return dob_ipc_call(port, &msg, &rep);
}
#endif /* FONTD_NO_CLIENT */

#endif /* FONTD_PROTOCOL_H */
