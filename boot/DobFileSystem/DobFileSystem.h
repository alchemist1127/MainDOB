#ifndef MAINDOB_STUBS_DOBFILESYSTEM_H
#define MAINDOB_STUBS_DOBFILESYSTEM_H

/* DobFileSystem Entry Point
 *
 * Usage:
 *   #include <DobFileSystem.h>
 *
 *   int fd = dobfs.file.Open("/DATA/Documents/readme.txt", FS_READ);
 *   char buf[4096];
 *   int n = dobfs.file.Read(fd, buf, sizeof(buf));
 *   dobfs.file.Close(fd);
 *
 *   dobfs.dir.List("/DATA/Music", entries, 64, &count);
 *
 *   dobfs.volume.Mount("/dev/sda1", "/DATA");
 */

#include <dob/types.h>
#include "dobfs_protocol.h"   /* dobfs_mounted_info_t, dobfs_df_info_t */

/* Open flags */
#define FS_READ     (1 << 0)
#define FS_WRITE    (1 << 1)
#define FS_CREATE   (1 << 2)
#define FS_APPEND   (1 << 3)
#define FS_TRUNC    (1 << 4)

/* File types */
#define FS_TYPE_FILE    1
#define FS_TYPE_DIR     2

/* Directory entry */
typedef struct
{
    char     name[256];
    uint32_t size;
    uint8_t  type;
} dobfs_dirent_t;

/* File stat */
typedef struct
{
    uint64_t size;
    uint8_t  type;
} dobfs_stat_t;

/* file.Open: open a file. Returns fd >= 0, or < 0 on error. */
int dobfs_Open(const char *path, uint32_t flags);

/* file.OpenOn: open a file on an explicit service, bypassing the
 * default path-prefix routing. Used by cross-service operations
 * (copy from CD to HDD) where the caller already knows the source
 * lives on a specific backend. Returns fd >= 0, or < 0 on error. */
int dobfs_OpenOn(const char *service, const char *path, uint32_t flags);

/* file.Read: read up to max bytes. Returns bytes read, or < 0. */
int dobfs_Read(int fd, void *buf, uint32_t max);

/* file.Write: write bytes. Returns bytes written, or < 0. */
int dobfs_Write(int fd, const void *buf, uint32_t len);

/* file.Close: close file descriptor. */
int dobfs_Close(int fd);

/* Whence values for dobfs_Seek. */
#define FS_SEEK_SET  0   /* offset is absolute */

/* file.Seek: reposition fd cursor. Returns the new absolute position,
 * or -1 on error. offset is a signed 64-bit value for CUR/END (pass
 * negative to seek backwards) and an absolute position for SET.
 * Read-only fds are clamped to EOF; write-capable fds can seek past
 * EOF and the file extends on the next write. */
int64_t dobfs_Seek(int fd, int64_t offset, int whence);

/* file.Stat: get file info. Returns 0 on success. */
int dobfs_Stat(const char *path, dobfs_stat_t *out);

/* file.StatOn: stat on an explicit service. Mirror of dobfs_OpenOn. */
int dobfs_StatOn(const char *service, const char *path, dobfs_stat_t *out);

/* dir.List: list directory entries. Returns count, or < 0. */
int dobfs_List(const char *path, dobfs_dirent_t *entries, uint32_t max, uint32_t *count);

/* dir.Mkdir: create a directory. Returns 0 on success. */
int dobfs_Mkdir(const char *path);

/* dir.MkdirOn: mkdir on an explicit service. Mirror of dobfs_OpenOn.
 * Used by cross-service operations (installer creating directories
 * on a freshly-mounted target) where the path doesn't match the
 * service's normal prefix routing. */
int dobfs_MkdirOn(const char *service, const char *path);

/* file.Unlink: delete a file or empty directory. Returns 0 on success. */
int dobfs_Unlink(const char *path);

/* file.Rename: rename/move a file or directory. Returns 0 on success. */
int dobfs_Rename(const char *old_path, const char *new_path);

/* ====================================================================
 *  Common files -- the shared, sandbox-free fallback area
 *
 *  Everything under /common_files is readable (and creatable) by any
 *  program regardless of its sandbox. Treat it as the place to fall
 *  back to for resources -- fonts, shared .MEM files, anything -- that
 *  a program does not ship in its own directory. MainDOB's analogue of
 *  .NET's My.Computer.FileSystem.SpecialDirectories.
 * ==================================================================== */
#define DOBFS_COMMON_FILES   "/system/config/common_files"
#define DOBFS_COMMON_FONTS   DOBFS_COMMON_FILES "/fonts"
#define DOBFS_COMMON_MEM     DOBFS_COMMON_FILES "/mem"

/* Join `sub` onto the common area into `buf`:
 *     dobfs_Common("fonts/sys.dmf", buf, sizeof buf)
 *         -> "/system/config/common_files/fonts/sys.dmf"
 * A leading '/' on `sub` is ignored; `sub` may be NULL for the bare
 * root. Returns `buf`, or NULL if the result would not fit. */
static inline const char *dobfs_Common(const char *sub, char *buf, uint32_t cap)
{
    if (!buf || cap == 0) return (const char *)0;
    const char *r = DOBFS_COMMON_FILES;
    uint32_t j = 0;
    while (*r && j + 1 < cap) buf[j++] = *r++;
    if (*r) { buf[cap - 1] = 0; return (const char *)0; }       /* root truncated */
    if (sub) {
        while (*sub == '/') sub++;
        if (*sub) {
            if (j + 1 < cap) buf[j++] = '/';
            while (*sub && j + 1 < cap) buf[j++] = *sub++;
            if (*sub) { buf[j] = 0; return (const char *)0; }   /* sub truncated */
        }
    }
    buf[j] = 0;
    return buf;
}

/* Open a resource, the program's own directory first, the common area
 * second -- the fallback the common area exists for.
 *   progdir : the program's directory (e.g. "/SYSTEM/PROGRAMS/DobWrite"),
 *             or NULL to look only in the common area. MainDOB does not
 *             hand a program its own path, so the caller supplies it.
 *   name    : resource path relative to either base, e.g. "notes.MEM"
 *             or "fonts/sys.dmf".
 * Tries "<progdir>/<name>" then "<common_files>/<name>"; returns the
 * first fd that opens, or -1 if neither exists. */
static inline int dobfs_OpenResource(const char *progdir, const char *name,
                                     uint32_t flags)
{
    char path[256];
    if (progdir && progdir[0] && name) {
        uint32_t j = 0;
        for (const char *p = progdir; *p && j + 1 < sizeof path; ) path[j++] = *p++;
        const char *n = name; while (*n == '/') n++;
        if (j + 1 < sizeof path) path[j++] = '/';
        for (; *n && j + 1 < sizeof path; ) path[j++] = *n++;
        path[j < sizeof path ? j : sizeof path - 1] = 0;
        int fd = dobfs_Open(path, flags);
        if (fd >= 0) return fd;
    }
    if (name && dobfs_Common(name, path, (uint32_t)sizeof path)) {
        int fd = dobfs_Open(path, flags);
        if (fd >= 0) return fd;
    }
    return -1;
}

/* Switch the filesystem service this stub talks to. By default the
 * stub binds to "DobFileSystem" — the boot disk. A satellite file
 * manager mounted on a different storage backend (floppy, CD, image
 * file, ...) calls this to redirect every subsequent dobfs_* call to
 * its own provider, which speaks the same dobfs_protocol.h. The first
 * call after the switch will lazily resolve the new service through
 * the registry, so the new provider only needs to be registered, not
 * waited on. */
void dobfs_set_service(const char *name);

/* Returns the name currently bound as the default routing service
 * for paths that don't match a well-known prefix (/DATA, /SYSTEM,
 * /u[0-9]). Returns "DobFileSystem" if untouched. Useful for
 * spawners that want to propagate their own mount to children. */
const char *dobfs_get_service(void);

/* Introspection on a specific dobfs_* instance. service is "DobFileSystem"
 * for the root mount or "dobfs_<token>" for a secondary. Returns 0 on
 * success. */
int dobfs_GetMountedOn(const char *service, dobfs_mounted_info_t *out);
int dobfs_DFOn        (const char *service, dobfs_df_info_t      *out);

/* Returns the human-readable reason string accompanying the most
 * recent DOB_ERR_REJECTED reply from a dobfs_* call, or "" if the
 * last reply was something else (success, generic error, etc).
 *
 * Drivers signal a refusable operation by returning DOB_ERR_REJECTED
 * with the reason in the reply payload (NUL-terminated UTF-8, ≤191
 * bytes after truncation). Typical examples: floppy "Disco pieno
 * (1.44 MB)"; CD "CD di sola lettura"; per-file ACL violation;
 * future quota systems.
 *
 * Strings are localised by the driver — DobFiles surfaces the
 * message verbatim in a popup. The buffer is process-local and
 * overwritten by the next dobfs_* call, so copy out if you need to
 * keep the string across further calls. */
const char *dobfs_last_rejection(void);

#endif
