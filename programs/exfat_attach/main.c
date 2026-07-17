/* MainDOB exFAT attach — boot-time one-shot
 *
 * Reads /SYSTEM/CONFIG/Definitive_volume (written by the installer) and, if
 * present, mounts the designated exFAT "definitive" volume by spawning a
 * secondary DobFileSystem with --mount. This is the runtime half of the
 * "kernel boots from FAT32, then attaches the definitive exFAT" design: the
 * small FAT32 system partition holds kernel + GRUB + exfat.mem + this helper,
 * and the large exFAT volume (the only option for disks > ~120 GB) is attached
 * here at startup.
 *
 * The config file IS the --mount argument string — a single line of
 * comma-separated key=value pairs, e.g.:
 *
 *     provider=ahci,selector=0,lba=2099200,id=7,fs=exfat
 *
 * We pass it through verbatim and let DobFileSystem's --mount parser validate
 * it. fs= is informational: fat32_mount() reads the boot sector and routes to
 * exfat.mem when it sees the exFAT signature, so an exFAT volume is mounted
 * read-write regardless of the fs= hint.
 *
 * Boot ordering: list this AFTER DobFileSystem in Startup_modules (or give it
 * a needs:DobFileSystem clause) so the root filesystem is up when we read the
 * config. If the config is missing or empty, this is a silent no-op.
 */

#include <unistd.h>
#include <string.h>
#include <dob/spawn.h>
#include <DobFileSystem.h>

#define DEFINITIVE_CONFIG  "/SYSTEM/CONFIG/Definitive_volume"
#define DOBFS_MDL          "/SYSTEM/OperatingSystem/DobFileSystem/DobFileSystem.mdl"

int main(void)
{
    char buf[256];

    int fd = dobfs_Open(DEFINITIVE_CONFIG, FS_READ);
    if (fd < 0)
        return 0;                       /* no definitive volume configured */

    int total = 0, got;
    while (total < (int)sizeof(buf) - 1 &&
           (got = dobfs_Read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += got;
    dobfs_Close(fd);
    buf[total] = '\0';

    /* Trim trailing whitespace / newlines (the file may end with a newline). */
    while (total > 0)
    {
        char c = buf[total - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            buf[--total] = '\0';
        else
            break;
    }

    /* Skip leading whitespace. */
    char *arg = buf;
    while (*arg == ' ' || *arg == '\t' || *arg == '\n' || *arg == '\r')
        arg++;

    if (*arg == '\0')
        return 0;                       /* empty config: nothing to attach */

    /* Sanity: the mount string must identify a partition somehow, or there is
     * nothing for DobFileSystem to mount. Refuse to spawn on garbage. */
    if (!strstr(arg, "lba=") && !strstr(arg, "token="))
        return 0;

    const char *av[3] = { "--mount", arg, NULL };
    spawn_file(DOBFS_MDL, av);
    return 0;
}
