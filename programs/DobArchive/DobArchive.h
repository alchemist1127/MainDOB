/* DobArchive Entry Point — native archive API
 *
 * Usage:
 *   #include <DobArchive.h>
 *
 *   // 1. Find a running DobArchive instance by its service name.
 *   //    Spawner contract: spawn DobArchive.mdl with argv = { path, NULL }
 *   //    where `path` is the archive file. Pass "--silent" anywhere in
 *   //    argv (e.g. { "--silent", path, NULL }) to suppress the default
 *   //    DobFiles auto-mount on top of the archive. DobArchive registers
 *   //    as "archiveN" for the first free N.
 *
 *   uint32_t n; uint8_t fmt;
 *   dobarchive_Count("archive0", &n, &fmt);
 *
 *   archive_entry_info_t info;
 *   dobarchive_EntryInfo("archive0", 0, &info);
 *
 *   char buf[4096];
 *   int got = dobarchive_ReadEntry("archive0", 0, 0, buf, sizeof(buf));
 *
 *   dobarchive_ExtractTo("archive0", 0, "/SYSTEM/PROGRAMS/foo/foo.mdl");
 *
 *   dobarchive_Close("archive0");
 */

#ifndef MAINDOB_STUBS_DOBARCHIVE_H
#define MAINDOB_STUBS_DOBARCHIVE_H

#include <dob/types.h>
#include "archive_protocol.h"

/* Fetch entry count + format tag. Returns 0 on success, < 0 on error. */
int dobarchive_Count(const char *service, uint32_t *count_out, uint8_t *format_out);

/* Fetch format tag only. Returns ARCHIVE_FMT_* on success, < 0 on error. */
int dobarchive_GetFormat(const char *service);

/* Fill `info` with metadata for entry at `index`. 0 on success, < 0 on error. */
int dobarchive_EntryInfo(const char *service, uint32_t index,
                         archive_entry_info_t *info);

/* Read up to `max` bytes from entry `index` starting at byte `offset`.
 * Returns the number of bytes actually read (0 on EOF), < 0 on error. */
int dobarchive_ReadEntry(const char *service, uint32_t index,
                         uint32_t offset, void *buf, uint32_t max);

/* Extract entry `index` to `dest_path` on the boot disk. Returns the
 * number of bytes written, < 0 on error. */
int dobarchive_ExtractTo(const char *service, uint32_t index,
                         const char *dest_path);

/* Tell the server to shut down cleanly (frees archive RAM and exits).
 * Returns 0 on success, < 0 if the server was unreachable. */
int dobarchive_Close(const char *service);

#endif /* MAINDOB_STUBS_DOBARCHIVE_H */
