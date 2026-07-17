/* exfat_api.h — public API of exfat.mem
 *
 * exfat.mem is a PIC ELF shared object (see docs/mem.md) loaded by
 * DobFileSystem via dob_mem_load(). It touches no hardware: every disk
 * access goes through the rd/wr sector callbacks handed to mount(),
 * which DobFileSystem wires to its own disk_read_sectors /
 * disk_write_sectors. The same .mem therefore serves a native partition
 * AND a USB stick with no change here — DobFileSystem is one binary for
 * both mounts, so there is exactly one exFAT implementation.
 *
 * Sectors are MainDOB 512-byte block-layer sectors, partition-relative
 * (sector 0 = the exFAT boot sector). exfat.mem translates the volume's
 * own BytesPerSectorShift (512..4096) into 512-byte units internally, so
 * "custom block sizes" are handled here, not by the caller.
 *
 * Sizes and offsets are 64-bit: exFAT exists to pass the FAT32 4 GB file
 * / 2 TB volume ceilings, so the whole API is uint64 from the start.
 *
 * The only external symbols the .mem references are the two callbacks
 * below (elf_load_shared accepts only R_386_RELATIVE; any other
 * unresolved reference fails the load). Everything else is defined
 * inside the .mem or inlined from headers.
 */

#ifndef MAINDOB_DOBFS_EXFAT_API_H
#define MAINDOB_DOBFS_EXFAT_API_H

#include <dob/types.h>

#define EXFAT_NAME_MAX   255   /* file name: up to 255 UTF-16 code units */

/* Read / write `count` 512-byte sectors at partition-relative `lba`.
 * Return 0 on success, negative on I/O error. `ctx` is the opaque cookie
 * the caller handed to mount(). */
typedef int (*exfat_rdsec_t)(void *ctx, uint32_t lba, uint32_t count, void *buf);
typedef int (*exfat_wrsec_t)(void *ctx, uint32_t lba, uint32_t count, const void *buf);

/* Opaque mount handle. Its internals (geometry, bitmap, up-case table,
 * the io callbacks) live in the .mem; the caller never inspects it. */
typedef struct exfat_volume exfat_volume_t;

typedef struct
{
    uint64_t size;       /* DataLength, bytes */
    bool     is_dir;
} exfat_stat_t;

typedef struct
{
    char     name[EXFAT_NAME_MAX + 1];   /* UTF-8, NUL-terminated */
    uint64_t size;
    bool     is_dir;
} exfat_dirent_t;

/* Open-file cursor. The caller holds this token between open() and the
 * read()/write()/flush() calls. The directory coordinates are carried so
 * the write path can update the entry set in place without re-walking. */
typedef struct
{
    uint32_t first_cluster;   /* 0 = empty file */
    uint64_t size;            /* DataLength */
    uint64_t valid;           /* ValidDataLength (<= size); bytes past it read as 0 */
    uint64_t pos;             /* current byte offset */
    uint32_t dir_cluster;     /* cluster of the directory holding this entry set */
    uint32_t set_offset;      /* byte offset of the entry set within that directory */
    bool     contiguous;      /* NoFatChain: chain is implicit, no FAT walk */
    bool     is_dir;
    bool     dirty;           /* metadata changed; flush() must commit it */
} exfat_file_t;

/* Return-code convention across stat/open/read/readdir/write/...:
 *     >= 0   success (read/write return the byte count moved)
 *     -1     not found / invalid argument
 *     -2     I/O error from a sector callback
 *     -3     unsupported (a feature not yet implemented in this .mem)
 *     -4     no space / read-only / would overflow
 * mount() returns NULL on any failure. */
typedef struct
{
    /* --- lifecycle --- */
    exfat_volume_t *(*mount)  (exfat_rdsec_t rd, exfat_wrsec_t wr, void *ctx);
    void            (*unmount)(exfat_volume_t *v);

    /* --- read path (Phase 1) --- */
    int  (*stat)   (exfat_volume_t *v, const char *path, exfat_stat_t *out);
    int  (*open)   (exfat_volume_t *v, const char *path, exfat_file_t *out);
    int  (*read)   (exfat_volume_t *v, exfat_file_t *f, void *buf, uint32_t n);
    int  (*readdir)(exfat_volume_t *v, const char *path,
                    exfat_dirent_t *out, int max);

    /* --- write path (implemented) --- */
    int  (*write)  (exfat_volume_t *v, exfat_file_t *f,
                    const void *buf, uint32_t n);
    int  (*ftrunc) (exfat_volume_t *v, exfat_file_t *f, uint64_t length);
    int  (*create) (exfat_volume_t *v, const char *path);
    int  (*mkdir)  (exfat_volume_t *v, const char *path);
    int  (*unlink) (exfat_volume_t *v, const char *path);
    int  (*rename) (exfat_volume_t *v, const char *from, const char *to);
    int  (*flush)  (exfat_volume_t *v, exfat_file_t *f);

    /* --- format (implemented) --- */
    int  (*mkfs)   (exfat_rdsec_t rd, exfat_wrsec_t wr, void *ctx,
                    uint64_t sectors, uint32_t bytes_per_sector,
                    const char *label);
} exfat_api_t;

#endif /* MAINDOB_DOBFS_EXFAT_API_H */
