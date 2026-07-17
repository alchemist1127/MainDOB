/* DobFiles Entry Point — File Dialog API (path-only)
 *
 * Opens the file explorer in dialog mode. The user navigates
 * and picks a file/location. Only the PATH is returned.
 * The caller reads/writes the file directly via DobFileSystem.
 *
 * Usage:
 *   char path[256];
 *   if (dobfiles_PickFile(".txt|.md", "/DATA/Documents", path, sizeof(path)) == 0)
 *       load_my_file(path);
 *
 *   if (dobfiles_PickSavePath("readme.txt", ".txt", "/DATA/Documents", path, sizeof(path)) == 0)
 *       save_my_file(path);
 */

#ifndef MAINDOB_STUBS_DOBFILES_H
#define MAINDOB_STUBS_DOBFILES_H

#include <dob/types.h>

/* Open file picker. Returns 0 + path on success, -1 on cancel. */
int dobfiles_PickFile(const char *extensions, const char *default_path,
                      char *out_path, uint32_t path_size);

/* Save path picker. Returns 0 + path on success, -1 on cancel. */
int dobfiles_PickSavePath(const char *default_name, const char *extensions,
                          const char *default_path,
                          char *out_path, uint32_t path_size);

/* Open the file explorer mounted on a specific filesystem backend.
 * `service` is the name a storage provider has registered with the
 * registry (it must speak the same protocol as DobFileSystem).
 * `root_path` is the directory the explorer should land on inside
 * that backend (e.g. "/u0" for floppy unit 0). Used by storage
 * drivers to surface their content as a regular file-explorer
 * window without DobFiles needing to know anything about the
 * underlying device.
 *
 * `hijack_port` selects the destination:
 *   0  → spawn a new DobFiles satellite window
 *  !=0 → IPC FILES_CMD_MOUNT to that specific port (typically a
 *        DobFiles.dobui_port() forwarded through ICON_ACTIVATED
 *        when the user clicked a device in DobFiles' Monta view)
 *
 * Returns 0 on success, -1 on failure. */
int dobfiles_OpenMount(const char *service, const char *root_path,
                       uint32_t hijack_port);

#endif /* MAINDOB_STUBS_DOBFILES_H */
