/* Public API of iso9660.mem.
 *
 * The cdrom driver loads this .mem at startup with dob_mem_load() and
 * casts the returned __mem_exports pointer to (iso9660_api_t *). The
 * .mem touches no hardware: every disk access goes through the rdsec
 * callback the driver passes to mount(), which turns one ISO sector
 * (LBA, 2048 bytes) into one ATAPI READ(10).
 *
 * This split keeps the parser orthogonal to the transport — the same
 * .mem could service an ISO image on a regular disk, a USB CDROM, or a
 * RAM-backed image, with no changes here.
 */

#ifndef MAINDOB_CDROM_ISO9660_API_H
#define MAINDOB_CDROM_ISO9660_API_H

#include <dob/types.h>

#define ISO_SECTOR_SIZE   2048
#define ISO_NAME_MAX      64

/* Read exactly one ISO sector at `lba` into `buf`. Returns 0 on
 * success, negative on error. `ctx` is the opaque cookie the caller
 * passed to mount(). */
typedef int (*iso_rdsec_t)(void *ctx, uint32_t lba, void *buf);

typedef struct iso_volume iso_volume_t;

typedef struct
{
    char     name[ISO_NAME_MAX];
    uint32_t lba;
    uint32_t size;
    bool     is_dir;
} iso_dirent_t;

typedef struct
{
    uint32_t lba;
    uint32_t size;
    bool     is_dir;
} iso_stat_t;

typedef struct
{
    uint32_t lba;
    uint32_t size;
    uint32_t pos;
} iso_file_t;

/* The exported API struct.
 *
 * Return code conventions across stat/open/read/readdir:
 *    0  / >=0  success
 *   -1         not found / invalid
 *   -2         I/O error from rdsec callback
 *
 * mount() returns NULL on any failure (bad PVD, rdsec error, second
 * mount on the same .mem instance — only one volume at a time).
 *
 * read() returns the number of bytes read; a short read at EOF is not
 * an error.
 *
 * readdir() returns the number of entries written, clamped to `max`;
 * `.` and `..` are filtered out by the parser. */
typedef struct
{
    iso_volume_t *(*mount)   (iso_rdsec_t rdsec, void *ctx);
    void          (*unmount) (iso_volume_t *v);
    int           (*stat)    (iso_volume_t *v, const char *path,
                              iso_stat_t *out);
    int           (*open)    (iso_volume_t *v, const char *path,
                              iso_file_t *out);
    int           (*read)    (iso_volume_t *v, iso_file_t *f,
                              void *buf, uint32_t n);
    int           (*readdir) (iso_volume_t *v, const char *path,
                              iso_dirent_t *out, int max);
} iso9660_api_t;

#endif /* MAINDOB_CDROM_ISO9660_API_H */
