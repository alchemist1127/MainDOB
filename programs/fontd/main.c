/* fontd -- the MainDOB font daemon.
 *
 * Loads the installed fonts (each described by a type=font manifest.dob),
 * builds a libdobfont resolver set, registers the "fontd" service, and
 * answers resolution requests: family+weight+italic+codepoint -> a font
 * file + face index + synthetic-style hints. Clients (the word processor,
 * any UI) open that file with libdobfont and rasterize locally.
 *
 * The resolver and manifest parsing are real. The one stub is font
 * DISCOVERY: MainDOB's directory-enumeration API isn't wired here, so the
 * set of manifests is a fixed list -- replace g_manifests[] (or the loop)
 * with a real scan of the fonts directory when that API is available.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <DobFileSystem.h>
#include <dob/server.h>
#include <dob/ipc.h>
#include <dobfont/dobfont.h>
#include <dobfont/resolver.h>
#include "fontd_protocol.h"

#define READ_CHUNK 65536
#define MAX_BLOBS  256

static df_fontset *g_set;
static void       *g_blobs[MAX_BLOBS];   /* font bytes kept alive for the daemon's life */
static int         g_nblobs;

/* ---- helpers ---- */

static void copy_bounded(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool read_all(const char *path, uint8_t **out, uint32_t *size)
{
    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return false;
    uint8_t *buf = NULL; uint32_t len = 0, cap = 0;
    for (;;) {
        if (len + READ_CHUNK > cap) {
            uint32_t nc = cap ? cap * 2 : READ_CHUNK * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, nc);
            if (!nb) { free(buf); dobfs_Close(fd); return false; }
            buf = nb; cap = nc;
        }
        int n = dobfs_Read(fd, (char *)(buf + len), READ_CHUNK);
        if (n < 0) { free(buf); dobfs_Close(fd); return false; }
        if (n == 0) break;
        len += (uint32_t)n;
    }
    dobfs_Close(fd);
    *out = buf; *size = len;
    return true;
}

/* directory portion of a path (everything up to the last '/') */
static void dir_of(char *dst, size_t cap, const char *path)
{
    copy_bounded(dst, path, cap);
    char *slash = NULL;
    for (char *p = dst; *p; p++) if (*p == '/') slash = p;
    if (slash) *slash = '\0';
    else dst[0] = '\0';
}

static void join_path(char *dst, size_t cap, const char *dir, const char *file)
{
    if (file[0] == '/' || dir[0] == '\0') { copy_bounded(dst, file, cap); return; }
    copy_bounded(dst, dir, cap);
    size_t n = strlen(dst);
    if (n + 1 < cap) { dst[n++] = '/'; dst[n] = '\0'; }
    copy_bounded(dst + n, file, cap - n);
}

static void load_manifest(const char *manifest_path)
{
    uint8_t *mtext; uint32_t mlen;
    if (!read_all(manifest_path, &mtext, &mlen)) {
        printf("fontd: skip %s (unreadable)\n", manifest_path);
        return;
    }
    df_font_manifest mf;
    df_result pr = df_font_manifest_parse((const char *)mtext, mlen, &mf);
    free(mtext);
    if (pr != DF_OK) return;

    char dir[256]; dir_of(dir, sizeof dir, manifest_path);

    for (int i = 0; i < mf.num_faces; i++) {
        char full[256];
        join_path(full, sizeof full, dir, mf.faces[i].file);

        uint8_t *fb; uint32_t fsz;
        if (!read_all(full, &fb, &fsz)) { printf("fontd: skip %s\n", full); continue; }

        df_face *face;
        if (df_open(fb, fsz, mf.faces[i].index, &face) != DF_OK) { free(fb); continue; }

        if (g_nblobs < MAX_BLOBS) g_blobs[g_nblobs++] = fb;   /* keep alive */
        df_fontset_add(g_set, mf.family, face, full, mf.faces[i].index);

        printf("fontd: + %s  [%s]  w=%u%s%s\n",
               mf.family, full, df_face_weight(face),
               df_face_is_italic(face) ? " italic" : "",
               df_has_outlines(face) ? "" : " (no outlines)");
    }
}

/* STUB: which manifests to load. Replace with a scan of the fonts dir. */
static const char *g_manifests[] = {
    "/fonts/Inter/manifest.dob",
    "/fonts/NotoSans/manifest.dob",
};

/* ---- request handler ---- */

static dob_status_t handler(dob_msg_t *msg, dob_msg_t *reply)
{
    static fontd_reply_t rep;        /* static: outlives this call for the reply copy */
    memset(&rep, 0, sizeof rep);

    if (msg->code != FONTD_OP_RESOLVE) {
        rep.status = DOB_ERR_INVALID;
    } else if (!msg->payload || msg->payload_size < sizeof(fontd_request_t)) {
        rep.status = DOB_ERR_INVALID;
    } else {
        const fontd_request_t *req = (const fontd_request_t *)msg->payload;

        df_query q;
        q.family    = req->family[0] ? req->family : NULL;
        q.weight    = req->weight ? req->weight : 400;
        q.italic    = req->italic != 0;
        q.codepoint = req->codepoint;

        df_resolved r;
        if (df_resolve(g_set, &q, &r) != DF_OK) {
            rep.status = DOB_ERR_NOT_FOUND;
        } else {
            rep.status      = DOB_OK;
            copy_bounded(rep.path, r.path, sizeof rep.path);
            rep.face_index  = r.face_index;
            rep.embolden_em = r.embolden_em;
            rep.slant       = r.slant;
        }
    }

    reply->code         = FONTD_OP_RESOLVE;
    reply->payload      = &rep;
    reply->payload_size = sizeof rep;
    return DOB_OK;
}

int main(void)
{
    g_set = df_fontset_create();
    if (!g_set) { printf("fontd: out of memory\n"); return 1; }

    /* fallback chain for code points the requested family lacks */
    static const char *fb[] = { "Noto Sans", "Inter" };
    df_fontset_set_fallback(g_set, fb, 2);

    for (size_t i = 0; i < sizeof(g_manifests) / sizeof(g_manifests[0]); i++)
        load_manifest(g_manifests[i]);

    if (dob_server_init(FONTD_SERVICE) != DOB_OK) {
        printf("fontd: could not register service\n");
        return 1;
    }
    dob_server_register(handler);
    printf("fontd: ready\n");
    dob_server_loop();          /* never returns */
    return 0;
}
