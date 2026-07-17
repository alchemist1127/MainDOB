/* DobArchive IPC Protocol — native archive API opcodes.
 *
 * DobArchive.mdl exposes two disjoint opcode ranges on the same IPC port:
 *
 *   1..17    = dobfs_protocol (mount/browse as filesystem, read-only v1)
 *   100..199 = archive-native API (installer, low-level tooling)
 *
 * The native API is entry-index based. Indexing happens once at startup,
 * when the server slurps the archive into RAM, and every native call
 * references entries by their position in that flat index table.
 *
 * Shared between server (main.c) and client stub (DobArchive_stub.c).
 */

#ifndef MAINDOB_ARCHIVE_PROTOCOL_H
#define MAINDOB_ARCHIVE_PROTOCOL_H

#include <dob/types.h>

/* Opcodes */

#define ARCHIVE_COUNT          100  /* no args              -> reply.arg0=count, arg1=format */
#define ARCHIVE_GET_FORMAT     101  /* no args              -> reply.arg0=ARCHIVE_FMT_*      */
#define ARCHIVE_ENTRY_INFO     102  /* arg0=index           -> reply.payload=archive_entry_info_t */
#define ARCHIVE_READ_ENTRY     103  /* arg0=index, arg1=off, arg2=max -> reply.payload=bytes, arg0=n */
#define ARCHIVE_EXTRACT_TO     104  /* arg0=index, payload=dest_path\0 -> reply.arg0=bytes_written */
#define ARCHIVE_CLOSE          105  /* no args              -> server exits cleanly              */

/* Format tags (returned by GET_FORMAT and in COUNT reply.arg1) */

#define ARCHIVE_FMT_UNKNOWN    0
#define ARCHIVE_FMT_TAR        1
#define ARCHIVE_FMT_ZIP        2

/* Entry types */

#define ARCHIVE_TYPE_FILE      1
#define ARCHIVE_TYPE_DIR       2

/* Entry info payload (ARCHIVE_ENTRY_INFO reply) */

#define ARCHIVE_MAX_PATH       256

typedef struct
{
    char     path[ARCHIVE_MAX_PATH];  /* absolute inside archive, leading '/' */
    uint32_t size;                    /* uncompressed size in bytes           */
    uint32_t compressed_size;         /* raw bytes in archive                 */
    uint32_t mode;                    /* Unix mode bits; 0 if unavailable     */
    uint32_t mtime;                   /* Unix timestamp; 0 if unavailable     */
    uint8_t  type;                    /* ARCHIVE_TYPE_FILE | ARCHIVE_TYPE_DIR */
    uint8_t  compression;             /* 0 = stored, 8 = deflate, ...          */
    uint8_t  _pad[2];
} archive_entry_info_t;

#endif /* MAINDOB_ARCHIVE_PROTOCOL_H */
