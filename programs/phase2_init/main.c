/* MainDOB Phase-2 boot loader — root-on-exFAT model.
 *
 * In the split layout the FAT32 partition is a minimal boot stub. The
 * kernel loads only Phase-1 modules from it (the disk driver,
 * DobFileSystem, the modules service and this loader) via its built-in
 * FAT32 reader, then shuts that reader (bootfs) down. DobFileSystem then
 * pivots the root onto the large exFAT volume. From that point the rest
 * of the system — hotplug, the GUI server, the settings/input servers,
 * user programs — lives on exFAT, which the kernel can no longer read.
 *
 * This one-shot is listed in the FAT32 Startup_modules with
 * needs:DobFileSystem. It waits for the pivot to finish (the root mount
 * reports fs_type == "exfat"), then reads /SYSTEM/CONFIG/Startup_modules
 * — which now resolves on exFAT — and launches every module that is not
 * part of the Phase-1 set. spawn_file_sync reads each .mdl through
 * DobFileSystem, i.e. from exFAT.
 *
 * Driver promotion: modules whose Startup_modules line carries the
 * "driver" flag (the video driver, dobinterface, hotplug, ...) need
 * make_driver() to reach hardware/ports — exactly what the kernel does
 * for boot drivers. We spawn synchronously (so this one-shot does not
 * exit before the spawn completes) and call make_driver() on the new pid
 * for driver lines. Without this the GUI/video drivers come up without
 * privileges and the desktop never appears.
 *
 * On a classic FAT32-root install there is no pivot: the wait times out
 * and this loader exits doing nothing. Zero regression.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <dob/spawn.h>
#include <dob/registry.h>
#include <DobFileSystem.h>

#define STARTUP_PATH   "/SYSTEM/CONFIG/Startup_modules"

/* Phase-1 modules: already launched by the kernel from the FAT32 stub. */
static const char *const phase1_names[] =
{
    "ata", "ahci", "DobFileSystem", "modules", "phase2_init",
};

static int is_phase1(const char *name)
{
    for (unsigned i = 0; i < sizeof(phase1_names) / sizeof(phase1_names[0]); i++)
        if (strcmp(name, phase1_names[i]) == 0)
            return 1;
    return 0;
}

/* Diagnostic: list a directory on the (now exFAT) root and print each entry.
 * Used when Startup_modules can't be opened, to see how far the on-disk
 * structure written by the installer actually persisted. */
static void dump_dir(const char *path)
{
    static dobfs_dirent_t ents[48];
    uint32_t count = 0;
    int rc = dobfs_List(path, ents, 48, &count);

    char dbg[160];
    snprintf(dbg, sizeof(dbg), "[phase2_init] list \"%s\": rc=%d count=%u\n",
             path, rc, (unsigned)count);
    debug_print(dbg);

    for (uint32_t i = 0; i < count && i < 48; i++)
    {
        snprintf(dbg, sizeof(dbg), "[phase2_init]    %c %s (%u)\n",
                 ents[i].type == FS_TYPE_DIR ? 'd' : '-', ents[i].name, ents[i].size);
        debug_print(dbg);
    }
}

/* basename(path) minus a trailing ".mdl", written into out. */
static void clean_name(const char *path, char *out, int cap)
{
    const char *bn = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            bn = s + 1;

    int n = 0;
    while (bn[n] && n < cap - 1)
    {
        out[n] = bn[n];
        n++;
    }
    out[n] = '\0';

    if (n > 4 && strcmp(out + n - 4, ".mdl") == 0)
        out[n - 4] = '\0';
}

int main(void)
{
    debug_print("[phase2_init] start; waiting for exFAT root pivot\n");

    /* 1. Wait (bounded) for the root pivot onto exFAT. */
    int pivoted = 0;
    for (int i = 0; i < 200; i++)        /* ~10 s at 50 ms */
    {
        dobfs_mounted_info_t info;
        if (dobfs_GetMountedOn("DobFileSystem", &info) == 0 &&
            info.is_root_mount &&
            strcmp(info.fs_type, "exfat") == 0)
        {
            pivoted = 1;
            break;
        }
        sleep_ms(50);
    }
    if (!pivoted)
    {
        debug_print("[phase2_init] no exFAT pivot (classic FAT32 root); exiting\n");
        return 0;
    }
    debug_print("[phase2_init] pivot detected; reading Startup_modules from exFAT\n");

    /* 2. Read the Startup_modules list, now resolving on exFAT. */
    static char buf[8192];
    int fd = dobfs_Open(STARTUP_PATH, FS_READ);
    if (fd < 0)
    {
        debug_print("[phase2_init] ERROR: cannot open Startup_modules on exFAT\n");
        debug_print("[phase2_init] listing exFAT root tree to diagnose:\n");
        dump_dir("/");
        dump_dir("/SYSTEM");
        dump_dir("/SYSTEM/CONFIG");
        return 0;
    }

    int total = 0, got;
    while (total < (int)sizeof(buf) - 1 &&
           (got = dobfs_Read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += got;
    dobfs_Close(fd);
    buf[total] = '\0';

    {
        char dbg[80];
        snprintf(dbg, sizeof(dbg), "[phase2_init] Startup_modules: %d bytes\n", total);
        debug_print(dbg);
    }

    /* 3. Launch each Phase-2 module. Line: "<path>\t<flags...>".
     *    Driver lines get make_driver() after a synchronous spawn. */
    int launched = 0;
    char *line = buf;
    while (line && *line)
    {
        char *eol = strchr(line, '\n');
        if (eol)
            *eol = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p && *p != '#')
        {
            char path[160];
            int n = 0;
            while (p[n] && p[n] != '\t' && p[n] != ' ' && n < (int)sizeof(path) - 1)
            {
                path[n] = p[n];
                n++;
            }
            path[n] = '\0';

            const char *flags  = p + n;                        /* rest of line */
            int         is_drv = (strstr(flags, "driver") != NULL);

            char nm[64];
            clean_name(path, nm, sizeof(nm));
            if (!is_phase1(nm))
            {
                /* Honor needs:NAME the way the kernel parks a needs: module
                 * at boot: block until the dependency registers before we
                 * spawn. dobinterface (needs:video) must wait for the BGA
                 * driver -- which hotplug spawns asynchronously after its PCI
                 * scan -- to register "video", or dv_vproc_attach fails (-4)
                 * and the desktop dies with a black screen. hotplug is earlier
                 * in the list, so it is already running and provides video
                 * while we block here. */
                const char *nd = strstr(flags, "needs:");
                if (nd)
                {
                    nd += 6;
                    char dep[64];
                    int dn = 0;
                    while (nd[dn] && nd[dn] != ' ' && nd[dn] != '\t' &&
                           dn < (int)sizeof(dep) - 1)
                    {
                        dep[dn] = nd[dn];
                        dn++;
                    }
                    dep[dn] = '\0';
                    if (dn > 0)
                    {
                        char w[140];
                        snprintf(w, sizeof(w),
                                 "[phase2_init] %s needs:%s -- waiting\n", nm, dep);
                        debug_print(w);
                        dob_registry_wait(dep, 8000);   /* up to 8s for the dep */
                    }
                }

                const char *argv[] = { 0 };
                pid_t pid = spawn_file_sync(path, argv);
                if (pid > 0 && is_drv)
                    make_driver(pid);

                char dbg[220];
                snprintf(dbg, sizeof(dbg),
                         "[phase2_init] spawn %s drv=%d pid=%d\n",
                         nm, is_drv, (int)pid);
                debug_print(dbg);
                if (pid > 0)
                    launched++;
            }
        }

        line = eol ? eol + 1 : 0;
    }

    {
        char dbg[80];
        snprintf(dbg, sizeof(dbg), "[phase2_init] done; %d module(s) launched\n", launched);
        debug_print(dbg);
    }
    return 0;
}
