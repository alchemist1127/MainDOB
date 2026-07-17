/* modules.mdl — Module Launcher & Library  (v2)
 *
 * Two tabs:
 *
 *   "Tutti i programmi"  — every installed program & game, scrollable,
 *                          split by separators into Programmi / Giochi.
 *                          Drivers are HIDDEN by default and appear as
 *                          a third section only when the panel toggle
 *                          "Mostra driver" is on (they ship with
 *                          Visible=0, which is exactly this opt-in).
 *                          Icons drag from here into the Library tab.
 *
 *   "Libreria"           — a free grid the user arranges by hand.
 *                          Items and pseudo-folders at chosen cells.
 *                          Opening a folder pops an in-window overlay.
 *                          Layout persists in /SYSTEM/CONFIG/Library.
 *
 * Look matches DobFiles: navy background, yellow text, yellow
 * selection. The selected item's FULL name is shown in a status bar at
 * the bottom (grid labels are truncated, the status bar is not).
 *
 * Icons live inside the program's own .mdl as a ".dobicon" ELF section
 * (16-bit RGB565). Missing → a coloured gear fallback.
 *
 * IMPORTANT pixel convention (matches the DobInterface texture-pool
 * blit path): a pixel is OPAQUE when its top byte is non-zero
 * (0xFFRRGGBB) and TRANSPARENT when the top byte is zero (0x00000000).
 * This is the INVERSE of the retired inline-blit path where 0xFF000000
 * meant transparent; using that here renders icons as black squares.
 * Every pixel buffer we build therefore carries 0xFF in the alpha slot
 * for opaque colours and 0x00000000 for holes.
 *
 * Performance: every icon and gear is rasterised ONCE into a small
 * uint32_t buffer and drawn with a single dobui_BlitBuffer. No
 * per-pixel DrawPixel in the draw path, and the window is not fully
 * repainted on every drag move.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <app.h>
#include <DobInterface.h>
#include <DobFileSystem.h>
#include <DobConfig.h>
#include <DobPopup.h>
#include <dob/spawn.h>
#include <dob/types.h>

/* ===================================================================
 * Geometry
 * =================================================================== */

#define WIN_W           720
#define WIN_H           520

#define ICON_SIZE       48
#define LABEL_H         14
#define CELL_PAD_X      22
#define CELL_PAD_Y      10
#define CELL_W          (ICON_SIZE + CELL_PAD_X * 2)          /* 92 */
#define CELL_H          (ICON_SIZE + 6 + LABEL_H + CELL_PAD_Y) /* 78 */

#define TAB_H           32
#define SEP_H           22                  /* category separator band */
#define STATUS_H        24                  /* bottom status bar       */

#define MAX_MODULES     128
#define MAX_LIB_ITEMS   128
#define MAX_LIB_FOLDERS 32
#define MAX_FOLDER_KIDS 64

#define DRAG_THRESHOLD  6

/* ===================================================================
 * Palette — identical to DobFiles (0x00RRGGBB for FillRect/DrawText,
 * which take the MainDOB 0x00BBGGRR-style constants; the blit buffers
 * use 0xFFRRGGBB opacity, see note at top).
 * =================================================================== */

#define COL_NAVY        0x00000C28   /* main background (even row navy) */
#define COL_NAVY_ALT    0x0000183E   /* odd-row / panel cyan-blue       */
#define COL_NAVY_DK     0x00000820   /* near-black blue (bars)          */
#define COL_RIBBON      0x00001A50   /* ribbon / tab strip              */
#define COL_TAB_ACT     0x00104070   /* active tab cyan-blue            */

#define COL_YELLOW      0x0000FFFF   /* primary text / accent           */
#define COL_SEL_BG      0x00FFE000   /* selection: yellow               */
#define COL_SEL_TEXT    0x00000000   /* text over yellow selection      */
#define COL_TEXT        0x0000FFFF   /* body text: yellow               */
#define COL_TEXT_MUTE   0x00339999   /* muted yellow (separators)       */
#define COL_SEP_LINE    0x00104070   /* separator hairline              */
#define COL_RED         0x00FF2050   /* accent red                      */

#define COL_FOLDER      0x00FFE000   /* folder tile body (yellow)       */

/* Blit-buffer colours (0xFFRRGGBB opaque, 0x00000000 transparent). */
#define BPX_NONE        0x00000000u
#define BPX_GEAR_PROG   0xFF7088B0u
#define BPX_GEAR_GAME   0xFF50C070u
#define BPX_GEAR_DRV    0xFFC08850u
#define BPX_GEAR_HOLE   0xFF001640u   /* navy, reads as the background  */
#define BPX_CHIP        0xFF202830u
#define BPX_CHIP_PIN    0xFFB0B8C0u
#define BPX_FOLDER      0xFFFFE000u
#define BPX_FOLDER_EDGE 0xFFC0A800u
#define BPX_MINI_BG     0xFF104070u
#define BPX_DANGLE      0xFF806010u   /* dim yellow for dangling refs   */

/* ===================================================================
 * Module model
 * =================================================================== */

typedef enum { MOD_PROGRAM = 0, MOD_GAME, MOD_DRIVER } mod_type_t;

typedef struct
{
    char        name[64];
    char        mdl_name[64];
    char        mdl_path[160];
    mod_type_t  type;
    bool        is_driver;
    bool        has_manifest;

    uint32_t   *icon_px;          /* decoded ICON_SIZE*ICON_SIZE, or NULL */
    int         icon_w, icon_h;
    bool        icon_tried;
} module_entry_t;

static module_entry_t modules[MAX_MODULES];
static int module_count = 0;

/* Driver visibility toggle (panel command). Off by default. */
static bool show_drivers = false;

/* ===================================================================
 * ELF .dobicon reader  (16-bit RGB565 → 0xFFRRGGBB opaque)
 * =================================================================== */

#define DOBICON_MAGIC     0xD0B1
#define DOBICON_FLAG_CKEY 0x0001
#define ELF_MAGIC_LE      0x464C457Fu

typedef struct
{
    uint32_t name; uint32_t type; uint32_t flags; uint32_t addr;
    uint32_t offset; uint32_t size; uint32_t link; uint32_t info;
    uint32_t addralign; uint32_t entsize;
} __attribute__((packed)) el_shdr_t;

/* RGB565 → 0xFFRRGGBB (opaque; top byte 0xFF per the blit convention). */
static inline uint32_t rgb565_to_opaque(uint16_t c)
{
    uint32_t r = (c >> 11) & 0x1F;
    uint32_t g = (c >> 5)  & 0x3F;
    uint32_t b =  c        & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_at_forward(int fd, uint32_t want_off, uint32_t *cur_off,
                           void *buf, uint32_t len)
{
    static char skip[512];
    while (*cur_off < want_off)
    {
        uint32_t gap = want_off - *cur_off;
        uint32_t chunk = gap > sizeof(skip) ? sizeof(skip) : gap;
        int n = dobfs_Read(fd, skip, chunk);
        if (n <= 0) return -1;
        *cur_off += (uint32_t)n;
    }
    if (*cur_off != want_off) return -1;
    int got = dobfs_Read(fd, buf, len);
    if (got > 0) *cur_off += (uint32_t)got;
    return got;
}

static bool load_dobicon(const char *mdl_path,
                         uint32_t **out_px, int *out_w, int *out_h)
{
    *out_px = NULL; *out_w = 0; *out_h = 0;

    int fd = dobfs_Open(mdl_path, FS_READ);
    if (fd < 0) return false;

    uint32_t cur = 0;
    uint8_t hbuf[52];
    if (read_at_forward(fd, 0, &cur, hbuf, sizeof(hbuf)) != (int)sizeof(hbuf))
    { dobfs_Close(fd); return false; }
    if (rd32(hbuf + 0) != ELF_MAGIC_LE) { dobfs_Close(fd); return false; }

    uint32_t shoff    = rd32(hbuf + 32);
    uint16_t shentsz  = rd16(hbuf + 46);
    uint16_t shnum    = rd16(hbuf + 48);
    uint16_t shstrndx = rd16(hbuf + 50);

    if (shoff == 0 || shnum == 0 || shentsz < sizeof(el_shdr_t) ||
        shnum > 256 || shstrndx >= shnum)
    { dobfs_Close(fd); return false; }

    uint32_t sht_bytes = (uint32_t)shnum * shentsz;
    uint8_t *sht = (uint8_t *)malloc(sht_bytes);
    if (!sht) { dobfs_Close(fd); return false; }
    if (read_at_forward(fd, shoff, &cur, sht, sht_bytes) != (int)sht_bytes)
    { free(sht); dobfs_Close(fd); return false; }

    const uint8_t *strsh = sht + (uint32_t)shstrndx * shentsz;
    uint32_t strtab_off  = rd32(strsh + 16);
    uint32_t strtab_size = rd32(strsh + 20);
    if (strtab_size == 0 || strtab_size > (1u << 20))
    { free(sht); dobfs_Close(fd); return false; }

    char *strtab = (char *)malloc(strtab_size + 1);
    if (!strtab) { free(sht); dobfs_Close(fd); return false; }
    if (read_at_forward(fd, strtab_off, &cur, strtab, strtab_size) != (int)strtab_size)
    { free(strtab); free(sht); dobfs_Close(fd); return false; }
    strtab[strtab_size] = '\0';

    uint32_t icon_off = 0, icon_size = 0;
    for (uint16_t i = 0; i < shnum; i++)
    {
        const uint8_t *sh = sht + (uint32_t)i * shentsz;
        uint32_t nameoff = rd32(sh + 0);
        if (nameoff >= strtab_size) continue;
        if (strcmp(strtab + nameoff, ".dobicon") == 0)
        { icon_off = rd32(sh + 16); icon_size = rd32(sh + 20); break; }
    }
    free(strtab); free(sht);

    if (icon_off == 0 || icon_size < 12 || icon_off < cur)
    { dobfs_Close(fd); return false; }

    uint8_t ih[12];
    if (read_at_forward(fd, icon_off, &cur, ih, sizeof(ih)) != (int)sizeof(ih))
    { dobfs_Close(fd); return false; }
    if (rd16(ih + 0) != DOBICON_MAGIC) { dobfs_Close(fd); return false; }

    int      w     = rd16(ih + 2);
    int      h     = rd16(ih + 4);
    uint16_t flags = rd16(ih + 6);
    uint16_t ckey  = rd16(ih + 8);
    if (w <= 0 || h <= 0 || w > 64 || h > 64) { dobfs_Close(fd); return false; }

    uint32_t need = (uint32_t)w * (uint32_t)h * 2u;
    if (icon_size < 12u + need) { dobfs_Close(fd); return false; }

    uint16_t *raw = (uint16_t *)malloc(need);
    if (!raw) { dobfs_Close(fd); return false; }
    uint32_t got = 0;
    while (got < need)
    {
        int n = dobfs_Read(fd, (uint8_t *)raw + got, need - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    dobfs_Close(fd);
    if (got != need) { free(raw); return false; }

    uint32_t *px = (uint32_t *)malloc((uint32_t)w * (uint32_t)h * sizeof(uint32_t));
    if (!px) { free(raw); return false; }

    bool has_key = (flags & DOBICON_FLAG_CKEY) != 0;
    for (int i = 0; i < w * h; i++)
    {
        uint16_t s = raw[i];
        px[i] = (has_key && s == ckey) ? BPX_NONE : rgb565_to_opaque(s);
    }
    free(raw);

    *out_px = px; *out_w = w; *out_h = h;
    return true;
}

/* ===================================================================
 * Gear fallback — rasterised into a 48x48 buffer ONCE per type, then
 * blitted. No DrawPixel in the hot path.
 * =================================================================== */

static uint32_t gear_buf[3][ICON_SIZE * ICON_SIZE];   /* prog/game/drv */
static uint32_t gear_buf_drv_chip[ICON_SIZE * ICON_SIZE];
static bool     gears_built = false;

static void plot(uint32_t *buf, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= ICON_SIZE || y >= ICON_SIZE) return;
    buf[y * ICON_SIZE + x] = c;
}

static void fill_disc(uint32_t *buf, int cx, int cy, int r, uint32_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r) plot(buf, cx+dx, cy+dy, c);
}

static void fill_box(uint32_t *buf, int x, int y, int w, int h, uint32_t c)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            plot(buf, x+xx, y+yy, c);
}

static void build_one_gear(uint32_t *buf, uint32_t body, bool chip)
{
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) buf[i] = BPX_NONE;

    int cx = ICON_SIZE/2, cy = ICON_SIZE/2;
    int r  = ICON_SIZE/2 - 6;
    fill_disc(buf, cx, cy, r, body);
    fill_disc(buf, cx, cy, r/3, BPX_GEAR_HOLE);

    int t = ICON_SIZE/2 - 2, ts = 7;
    int td[8][2] = {
        {0,-t},{0,t},{-t,0},{t,0},
        {-t*7/10,-t*7/10},{t*7/10,-t*7/10},{-t*7/10,t*7/10},{t*7/10,t*7/10}
    };
    for (int i = 0; i < 8; i++)
        fill_box(buf, cx+td[i][0]-ts/2, cy+td[i][1]-ts/2, ts, ts, body);

    if (chip)
    {
        fill_box(buf, cx-7, cy-7, 14, 14, BPX_CHIP);
        for (int i = -1; i <= 1; i++)
        {
            fill_box(buf, cx+i*4-1, cy-10, 3, 4, BPX_CHIP_PIN);
            fill_box(buf, cx+i*4-1, cy+6,  3, 4, BPX_CHIP_PIN);
            fill_box(buf, cx-10, cy+i*4-1, 4, 3, BPX_CHIP_PIN);
            fill_box(buf, cx+6,  cy+i*4-1, 4, 3, BPX_CHIP_PIN);
        }
    }
}

/* 8-bit green ghost for game icons (drawn in a 48x48 buffer once).
 * Classic arcade-ghost silhouette: domed top, wavy skirt, two eyes.
 * Built at "logical" 12x12 and scaled x4 to fill the 48 box with
 * chunky pixels, for that deliberate 8-bit look. */
static uint32_t ghost_icon[ICON_SIZE * ICON_SIZE];
static bool     ghost_icon_built = false;

#define GBODY 0xFF50C070u   /* green body  */
#define GEYE  0xFFFFFFFFu   /* white eye   */
#define GPUP  0xFF101840u   /* navy pupil  */

static void build_ghost_icon(void)
{
    if (ghost_icon_built) return;

    /* 12x12 logical grid. 0 transparent, 1 body, 2 eye, 3 pupil. */
    static const uint8_t G[12][12] = {
        {0,0,0,1,1,1,1,1,1,0,0,0},
        {0,0,1,1,1,1,1,1,1,1,0,0},
        {0,1,1,1,1,1,1,1,1,1,1,0},
        {0,1,1,2,2,1,1,2,2,1,1,0},
        {1,1,2,2,2,2,2,2,2,2,1,1},
        {1,1,2,3,2,2,2,2,3,2,1,1},
        {1,1,2,3,2,2,2,2,3,2,1,1},
        {1,1,2,2,2,2,2,2,2,2,1,1},
        {1,1,1,1,1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1,1,1,1,1},
        {1,1,1,1,1,1,1,1,1,1,1,1},
        {1,0,1,1,0,1,1,0,1,1,0,1},   /* wavy skirt */
    };

    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) ghost_icon[i] = BPX_NONE;

    int scale = ICON_SIZE / 12;          /* 4 */
    int off   = (ICON_SIZE - 12 * scale) / 2;
    for (int gy = 0; gy < 12; gy++)
        for (int gx = 0; gx < 12; gx++)
        {
            uint8_t v = G[gy][gx];
            if (!v) continue;
            uint32_t c = (v == 1) ? GBODY : (v == 2) ? GEYE : GPUP;
            fill_box(ghost_icon, off + gx*scale, off + gy*scale, scale, scale, c);
        }
    ghost_icon_built = true;
}

static void build_gears(void)
{
    if (gears_built) return;
    build_one_gear(gear_buf[MOD_PROGRAM], BPX_GEAR_PROG, false);
    build_one_gear(gear_buf[MOD_GAME],    BPX_GEAR_GAME, false);
    build_one_gear(gear_buf[MOD_DRIVER],  BPX_GEAR_DRV,  false);
    build_one_gear(gear_buf_drv_chip,     BPX_GEAR_DRV,  true);
    build_ghost_icon();
    gears_built = true;
}

/* ===================================================================
 * Module discovery
 * =================================================================== */

static bool str_ends_with(const char *s, const char *suffix)
{
    int sl = (int)strlen(s), fl = (int)strlen(suffix);
    if (fl > sl) return false;
    return strcmp(s + sl - fl, suffix) == 0;
}

static void strip_ext(const char *src, char *dst, int dst_size)
{
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    char *last_dot = NULL;
    for (char *p = dst; *p; p++) if (*p == '.') last_dot = p;
    if (last_dot) *last_dot = '\0';
}

/* Pretty names for the system modules that ship without a manifest, so
 * they don't show up as bare DOS-style stems. Looked up case-
 * insensitively against the directory stem. */
static const struct { const char *key; const char *pretty; } pretty_names[] = {
    { "snake",       "Snake"            },
    { "tetris",      "Tetris"           },
    { "minesweeper", "Campo minato"     },
    { "solitaire",   "Solitario"        },
    { "arrowsweeper","Arrow Sweeper"    },
    { "uidemo",      "Demo interfaccia" },
    { "rtc",         "Orologio (RTC)"   },
    { "ata",         "Disco ATA"        },
    { "ahci",        "Disco AHCI"       },
    { "floppy",      "Floppy"           },
    { "cdrom",       "CD-ROM"           },
    { "bga",         "Video BGA"        },
    { "mach64",      "Video Mach64"     },
    { "ac97",        "Audio AC'97"      },
    { "maestro2e",   "Audio Maestro-2E" },
    { "usb_uhci",    "USB UHCI"         },
    { "usb_ehci",    "USB EHCI"         },
    { "usb_xhci",    "USB xHCI"         },
};

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

/* Turn a raw stem (often UPPERCASE 8.3 from FAT) into a readable name:
 * first try the pretty table, else Title-case it (SNAKE → Snake). */
static void prettify_name(const char *stem, char *out, int out_sz)
{
    for (unsigned i = 0; i < sizeof(pretty_names)/sizeof(pretty_names[0]); i++)
        if (ieq(stem, pretty_names[i].key))
        {
            strncpy(out, pretty_names[i].pretty, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }

    int j = 0;
    bool start = true;
    for (int i = 0; stem[i] && j < out_sz - 1; i++)
    {
        char c = stem[i];
        if (c == '_' || c == '-') { out[j++] = ' '; start = true; continue; }
        if (start) { if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A'); start = false; }
        else       { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
        out[j++] = c;
    }
    out[j] = '\0';
}

static bool module_exists(const char *mdl_path)
{
    for (int i = 0; i < module_count; i++)
        if (strcmp(modules[i].mdl_path, mdl_path) == 0) return true;
    return false;
}

static int module_by_path(const char *mdl_path)
{
    for (int i = 0; i < module_count; i++)
        if (strcmp(modules[i].mdl_path, mdl_path) == 0) return i;
    return -1;
}

static void parse_manifest(const char *dir, const char *manifest_name,
                           mod_type_t dir_type)
{
    if (module_count >= MAX_MODULES) return;

    char path[240];
    snprintf(path, sizeof(path), "%s/%s", dir, manifest_name);

    int fd = dobfs_Open(path, FS_READ);
    if (fd < 0) return;
    char buf[512];
    int len = dobfs_Read(fd, buf, sizeof(buf) - 1);
    dobfs_Close(fd);
    if (len <= 0) return;
    buf[len] = '\0';

    module_entry_t *m = &modules[module_count];
    memset(m, 0, sizeof(*m));
    m->has_manifest = true;

    /* default name from the manifest filename stem (prettified) */
    {
        char stem[64];
        strip_ext(manifest_name, stem, sizeof(stem));
        prettify_name(stem, m->name, sizeof(m->name));
    }
    m->type = dir_type;

    char *line = buf;
    while (*line)
    {
        char *eol = line; while (*eol && *eol != '\n') eol++;
        char saved = *eol; *eol = '\0';

        if (line[0] != '#' && line[0] != '\0')
        {
            char *eq = line; while (*eq && *eq != '=') eq++;
            if (*eq == '=')
            {
                *eq = '\0';
                char *key = line, *val = eq + 1;
                if (strcmp(key, "name") == 0)
                { strncpy(m->name, val, sizeof(m->name) - 1); m->name[sizeof(m->name)-1]='\0'; }
                else if (strcmp(key, "mdl") == 0)
                {
                    strncpy(m->mdl_name, val, sizeof(m->mdl_name) - 1);
                    char pp[200]; snprintf(pp, sizeof(pp), "%s/%s", dir, val);
                    strncpy(m->mdl_path, pp, sizeof(m->mdl_path) - 1);
                    m->mdl_path[sizeof(m->mdl_path)-1] = '\0';
                }
                else if (strcmp(key, "type") == 0)
                {
                    if (strcmp(val, "game") == 0)        m->type = MOD_GAME;
                    else if (strcmp(val, "driver") == 0) m->type = MOD_DRIVER;
                    else                                 m->type = MOD_PROGRAM;
                }
                else if (strcmp(key, "driver") == 0)
                {
                    if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
                        m->is_driver = true;
                }
            }
        }
        if (saved == '\0') break;
        line = eol + 1;
    }

    if (m->mdl_name[0] == '\0')
    {
        /* derive .mdl from directory stem */
        char stem[64];
        const char *base = dir;
        for (const char *p = dir; *p; p++) if (*p == '/') base = p + 1;
        strncpy(stem, base, sizeof(stem) - 1); stem[sizeof(stem)-1]='\0';
        char tmp[80]; snprintf(tmp, sizeof(tmp), "%s.mdl", stem);
        strncpy(m->mdl_name, tmp, sizeof(m->mdl_name) - 1); m->mdl_name[sizeof(m->mdl_name)-1]='\0';
        char pp[200]; snprintf(pp, sizeof(pp), "%s/%s", dir, tmp);
        strncpy(m->mdl_path, pp, sizeof(m->mdl_path) - 1); m->mdl_path[sizeof(m->mdl_path)-1]='\0';
    }
    if (m->type == MOD_DRIVER) m->is_driver = true;
    if (module_exists(m->mdl_path)) return;
    module_count++;
}

static void add_mdl_entry(const char *filename, const char *dir,
                          mod_type_t type, bool driver)
{
    if (module_count >= MAX_MODULES) return;
    char full[200];
    snprintf(full, sizeof(full), "%s/%s", dir, filename);
    if (module_exists(full)) return;

    module_entry_t *m = &modules[module_count];
    memset(m, 0, sizeof(*m));

    char stem[64];
    strip_ext(filename, stem, sizeof(stem));
    prettify_name(stem, m->name, sizeof(m->name));   /* SNAKE → Snake */

    strncpy(m->mdl_name, filename, sizeof(m->mdl_name) - 1);
    strncpy(m->mdl_path, full, sizeof(m->mdl_path) - 1);
    m->type = type;
    m->is_driver = driver;
    m->has_manifest = false;
    module_count++;
}

/* Scan a category directory. We DO NOT honour the per-module "Visible"
 * flag here: that flag keeps modules off the Desktop, not out of the
 * launcher. Drivers are instead gated by the show_drivers toggle at
 * display time, so the Module Manager always knows about everything
 * installed and can reveal drivers on demand. */
static void scan_directory(const char *dir, mod_type_t type)
{
    dobfs_dirent_t entries[64];
    uint32_t count = 0;
    if (dobfs_List(dir, entries, 64, &count) < 0 || count == 0) return;

    bool is_driver = (type == MOD_DRIVER);

    for (uint32_t i = 0; i < count; i++)
    {
        if (entries[i].type != FS_TYPE_DIR) continue;

        char subdir[320];
        snprintf(subdir, sizeof(subdir), "%s/%s", dir, entries[i].name);

        dobfs_dirent_t sub[32];
        uint32_t sc = 0;
        if (dobfs_List(subdir, sub, 32, &sc) < 0 || sc == 0) continue;

        bool had_manifest = false;
        for (uint32_t j = 0; j < sc; j++)
        {
            if (sub[j].type == FS_TYPE_DIR) continue;
            if (str_ends_with(sub[j].name, ".manifest") ||
                ieq(sub[j].name, "manifest.dob"))
            {
                parse_manifest(subdir, sub[j].name, type);
                had_manifest = true;
            }
        }
        if (had_manifest) continue;

        for (uint32_t j = 0; j < sc; j++)
        {
            if (sub[j].type == FS_TYPE_DIR) continue;
            if (str_ends_with(sub[j].name, ".mdl") || str_ends_with(sub[j].name, ".MDL"))
                add_mdl_entry(sub[j].name, subdir, type, is_driver);
        }
    }
}

static void load_modules(void)
{
    for (int i = 0; i < module_count; i++)
        if (modules[i].icon_px) { free(modules[i].icon_px); modules[i].icon_px = NULL; }

    module_count = 0;
    scan_directory("/SYSTEM/PROGRAMS", MOD_PROGRAM);
    scan_directory("/SYSTEM/GAMES",    MOD_GAME);
    scan_directory("/SYSTEM/DRIVERS",  MOD_DRIVER);
}

static void ensure_icon(module_entry_t *m)
{
    if (m->icon_tried) return;
    m->icon_tried = true;
    if (m->mdl_path[0])
        load_dobicon(m->mdl_path, &m->icon_px, &m->icon_w, &m->icon_h);
}

/* ===================================================================
 * Library model — persisted in /SYSTEM/CONFIG/Library
 * =================================================================== */

/* Library layout lives in /DATA/ — the only area a normal (non-driver)
 * program may write to under the filesystem sandbox, and the one that
 * persists on an installed disk. /SYSTEM/CONFIG/ is reserved for
 * privileged services (the config daemon), so a direct write there
 * from this app is refused. On a live (ISO) boot the disk is read-only
 * by design: saves silently no-op and changes last only for the
 * session — expected for a live medium. */
#define LIBRARY_PATH    "/DATA/Library"

typedef struct { char path[160]; } lib_ref_t;

typedef struct {
    char       name[48];
    int        gx, gy;
    lib_ref_t  kids[MAX_FOLDER_KIDS];
    int        kid_count;
} lib_folder_t;

typedef struct { char path[160]; int gx, gy; } lib_item_t;

static lib_folder_t lib_folders[MAX_LIB_FOLDERS];
static int          lib_folder_count = 0;
static lib_item_t   lib_items[MAX_LIB_ITEMS];
static int          lib_item_count = 0;

/* ===================================================================
 * Drawing globals + icon blitting (VRAM-frugal, DobFiles model)
 *
 * Hardware reality: the target (e500-class, 8 MB VRAM) has ~2 MB free
 * after double buffering. A full-content offscreen texture (~1.2 MB)
 * starves that budget — dv_texture_create fails, BlitBuffer is
 * silently dropped, and icons vanish while text (glyph atlas, 0 VRAM)
 * still shows. That was the "names but no icons" symptom.
 *
 * So we follow what DobFiles does: a SMALL set of SHARED 48x48 icon
 * buffers (gear per type, ghost for games, folder, dangling), each a
 * stable pointer reused for every matching entry. The server texture
 * pool is keyed on (w,h,src); identical sources collapse to ONE
 * texture. Total VRAM for icons: a handful of 48x48x4 = ~9 KB each,
 * well within budget, and never more than a few pool slots regardless
 * of how many programs are shown.
 *
 * Embedded .dobicon images (one distinct pointer per program) would
 * each take a slot; today no program ships one, so in practice every
 * icon is a shared gear/ghost. If many custom icons ever appear, the
 * 16-slot pool recycles oldest-first — acceptable degradation, not a
 * VRAM blowout.
 *
 * The previous offscreen primitives (ob_*) are kept as thin direct
 * wrappers so the drawing code didn't have to change shape: ob_fill →
 * FillRect, ob_blit → BlitBuffer, and setup/clear/flush are no-ops
 * (the per-frame navy wipe in redraw() clears the background).
 * =================================================================== */

static uint32_t g_win;
static int      g_w = WIN_W, g_h = WIN_H;

/* Shared folder icon (generic yellow tile, no per-folder previews —
 * previews would need a distinct texture per folder, which the small
 * VRAM budget can't afford). Built once. */
static uint32_t folder_icon[ICON_SIZE * ICON_SIZE];
static bool     folder_icon_built = false;

static void build_folder_icon(void)
{
    if (folder_icon_built) return;
    for (int i = 0; i < ICON_SIZE * ICON_SIZE; i++) folder_icon[i] = BPX_NONE;
    /* Folder body with a tab, classic look. */
    fill_box(folder_icon, 4, 14, ICON_SIZE - 8, ICON_SIZE - 20, BPX_FOLDER);
    fill_box(folder_icon, 4, 10, 18, 6, BPX_FOLDER);          /* tab */
    for (int xx = 4; xx < ICON_SIZE - 4; xx++)
    { plot(folder_icon, xx, 14, BPX_FOLDER_EDGE); plot(folder_icon, xx, ICON_SIZE-7, BPX_FOLDER_EDGE); }
    for (int yy = 14; yy < ICON_SIZE - 6; yy++)
    { plot(folder_icon, 4, yy, BPX_FOLDER_EDGE); plot(folder_icon, ICON_SIZE-5, yy, BPX_FOLDER_EDGE); }
    folder_icon_built = true;
}

/* ob_setup/clear/flush are no-ops now (background handled by redraw's
 * navy wipe). Kept so the draw functions stay unchanged. */
static bool ob_setup(int x0, int y0, int w, int h) { (void)x0;(void)y0;(void)w;(void)h; return true; }
static void ob_clear(uint32_t c) { (void)c; }
static void ob_flush(void) { }

/* Vertical clip box for the content area. Direct FillRect/BlitBuffer
 * clip only to the whole window, not to our tab/status chrome, so
 * during scroll a partially-visible top/bottom row would draw over the
 * tabs. We clip here; the draw functions set the box first. */
static int clip_y0 = 0, clip_y1 = 1 << 30;
static void set_clip(int y0, int y1) { clip_y0 = y0; clip_y1 = y1; }
static void clear_clip(void) { clip_y0 = 0; clip_y1 = 1 << 30; }

/* Direct opaque rectangle, vertically clipped to the content box. */
static void ob_fill(int wx, int wy, int w, int h, uint32_t c)
{
    int y0 = wy, y1 = wy + h;
    if (y0 < clip_y0) y0 = clip_y0;
    if (y1 > clip_y1) y1 = clip_y1;
    if (y1 <= y0) return;
    dobui_FillRect(g_win, wx, y0, w, y1 - y0, c & 0x00FFFFFFu);
}

/* Transparency-aware blit, vertically clipped to the content box. A partially
 * visible icon is drawn by blitting a SUB-REGION of the source buffer -- same
 * base pointer, offset to the first visible row -- rather than copying the
 * cropped rows into a private scratch buffer. That keeps the pool key stable:
 * every cell showing the same icon at the same clip collapses onto ONE pooled
 * texture, instead of each partial cell minting its own and overflowing the
 * 16-slot pool (which is what aliased gears onto the games ghost during
 * scroll). The shared source is contiguous ICON_SIZE-wide rows, so `src +
 * top_skip*sw` points at the first visible row; embedded icons (own width)
 * work the same way via the sw stride. */
static void ob_blit(int wx, int wy, const uint32_t *src, int sw, int sh)
{
    int top_skip = (wy < clip_y0) ? (clip_y0 - wy) : 0;
    int vis_h = sh - top_skip;
    if (wy + sh > clip_y1) vis_h -= (wy + sh - clip_y1);
    if (vis_h <= 0) return;
    dobui_BlitBuffer(g_win, wx, wy + top_skip, src + (size_t)top_skip * (size_t)sw, sw, vis_h);
}

static void ob_module_icon(int wx, int wy, module_entry_t *m)
{
    ensure_icon(m);
    if (m->icon_px && m->icon_w > 0 && m->icon_h > 0)
    {
        int ox = wx + (ICON_SIZE - m->icon_w) / 2;
        int oy = wy + (ICON_SIZE - m->icon_h) / 2;
        ob_blit(ox, oy, m->icon_px, m->icon_w, m->icon_h);
        return;
    }
    build_gears();
    /* Games without an embedded icon get the 8-bit ghost; programs and
     * drivers fall back to the coloured gear. */
    if (m->type == MOD_GAME) { ob_blit(wx, wy, ghost_icon, ICON_SIZE, ICON_SIZE); return; }
    uint32_t *g = (m->type == MOD_DRIVER)
                    ? (m->is_driver ? gear_buf_drv_chip : gear_buf[MOD_DRIVER])
                    : gear_buf[m->type];
    ob_blit(wx, wy, g, ICON_SIZE, ICON_SIZE);
}

static void ob_dangling(int wx, int wy)
{
    static uint32_t buf[ICON_SIZE * ICON_SIZE];
    static bool built = false;
    if (!built) { build_one_gear(buf, BPX_DANGLE, false); built = true; }
    ob_blit(wx, wy, buf, ICON_SIZE, ICON_SIZE);
}

static void ob_folder_tile(int wx, int wy, lib_folder_t *f)
{
    (void)f;                 /* generic shared folder icon, no previews */
    build_folder_icon();
    ob_blit(wx, wy, folder_icon, ICON_SIZE, ICON_SIZE);
}

/* Centered, ellipsised label under a cell. Text is drawn directly to
 * the window (glyph atlas; no texture-pool cost), layered over the
 * already-flushed offscreen. */
static void draw_label(int cell_x, int cell_y, const char *name,
                       int maxw, uint32_t fg, uint32_t bg)
{
    int maxchars = maxw / 8; if (maxchars < 1) maxchars = 1;
    char buf[40];
    int nlen = (int)strlen(name);
    if (nlen <= maxchars)
    { strncpy(buf, name, sizeof(buf) - 1); buf[sizeof(buf)-1]='\0'; }
    else
    {
        /* Trailing "..." like the file manager, so the user sees the
         * prefix and knows the full name is in the status bar. */
        int keep = maxchars - 3; if (keep < 1) keep = 1;
        if (keep > (int)sizeof(buf) - 4) keep = (int)sizeof(buf) - 4;
        memcpy(buf, name, (uint32_t)keep);
        buf[keep] = '.'; buf[keep+1] = '.'; buf[keep+2] = '.'; buf[keep+3] = '\0';
    }
    int tlen = (int)strlen(buf);
    int lx = cell_x + (CELL_W - tlen * 8) / 2;
    int ty = cell_y + ICON_SIZE + 5;
    /* Honour the content clip box (text isn't clipped by the server to
     * our chrome): skip labels whose line falls outside it. */
    if (ty < clip_y0 || ty + 12 > clip_y1) return;
    dobui_DrawText(g_win, lx, ty, buf, fg, bg);
}

/* ===================================================================
 * Library persistence & mutation
 * =================================================================== */

static void library_save(void);

static bool cell_occupied(int gx, int gy, int ignore_folder, int ignore_item)
{
    for (int i = 0; i < lib_folder_count; i++)
        if (i != ignore_folder && lib_folders[i].gx == gx && lib_folders[i].gy == gy) return true;
    for (int i = 0; i < lib_item_count; i++)
        if (i != ignore_item && lib_items[i].gx == gx && lib_items[i].gy == gy) return true;
    return false;
}

static void first_free_cell(int cols, int *out_gx, int *out_gy)
{
    for (int gy = 0; gy < 64; gy++)
        for (int gx = 0; gx < cols; gx++)
            if (!cell_occupied(gx, gy, -1, -1)) { *out_gx = gx; *out_gy = gy; return; }
    *out_gx = 0; *out_gy = 0;
}

static bool library_contains(const char *path)
{
    for (int i = 0; i < lib_item_count; i++)
        if (strcmp(lib_items[i].path, path) == 0) return true;
    for (int f = 0; f < lib_folder_count; f++)
        for (int k = 0; k < lib_folders[f].kid_count; k++)
            if (strcmp(lib_folders[f].kids[k].path, path) == 0) return true;
    return false;
}

static void library_load(void)
{
    lib_folder_count = 0; lib_item_count = 0;

    int fd = dobfs_Open(LIBRARY_PATH, FS_READ);
    if (fd < 0) return;

    static char buf[8192];
    int total = 0, n;
    while (total < (int)sizeof(buf) - 1 &&
           (n = dobfs_Read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    dobfs_Close(fd);
    if (total <= 0) return;
    buf[total] = '\0';

    lib_folder_t *cur_folder = NULL;
    char *line = buf;
    while (*line)
    {
        char *eol = line; while (*eol && *eol != '\n') eol++;
        char saved = *eol; *eol = '\0';

        bool indented = (line[0] == ' ' || line[0] == '\t');
        char *p = line; while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0' || *p == '#') { /* skip */ }
        else if (indented && cur_folder)
        {
            if (cur_folder->kid_count < MAX_FOLDER_KIDS)
            {
                strncpy(cur_folder->kids[cur_folder->kid_count].path, p,
                        sizeof(cur_folder->kids[0].path) - 1);
                cur_folder->kid_count++;
            }
        }
        else if (strncmp(p, "folder ", 7) == 0)
        {
            cur_folder = NULL;
            if (lib_folder_count < MAX_LIB_FOLDERS)
            {
                lib_folder_t *f = &lib_folders[lib_folder_count];
                memset(f, 0, sizeof(*f));
                char *q = p + 7; while (*q == ' ') q++;
                if (*q == '"')
                {
                    q++;
                    int ni = 0;
                    while (*q && *q != '"' && ni < (int)sizeof(f->name) - 1) f->name[ni++] = *q++;
                    f->name[ni] = '\0';
                    if (*q == '"') q++;
                }
                while (*q == ' ') q++;
                f->gx = atoi(q);
                while (*q && *q != ' ') q++;
                while (*q == ' ') q++;
                f->gy = atoi(q);
                cur_folder = f;
                lib_folder_count++;
            }
        }
        else if (strncmp(p, "item ", 5) == 0)
        {
            cur_folder = NULL;
            if (lib_item_count < MAX_LIB_ITEMS)
            {
                lib_item_t *it = &lib_items[lib_item_count];
                memset(it, 0, sizeof(*it));
                char *q = p + 5; while (*q == ' ') q++;
                int pi = 0;
                while (*q && *q != ' ' && pi < (int)sizeof(it->path) - 1) it->path[pi++] = *q++;
                it->path[pi] = '\0';
                while (*q == ' ') q++;
                it->gx = atoi(q);
                while (*q && *q != ' ') q++;
                while (*q == ' ') q++;
                it->gy = atoi(q);
                lib_item_count++;
            }
        }
        if (saved == '\0') break;
        line = eol + 1;
    }
}

static void fput(int fd, const char *s) { dobfs_Write(fd, s, (uint32_t)strlen(s)); }

static void library_save(void)
{
    int fd = dobfs_Open(LIBRARY_PATH, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) return;

    char line[256];
    fput(fd, "# MainDOB Library layout — managed by the Module Manager.\n");
    fput(fd, "# folder \"name\" gx gy / indented child paths / item path gx gy\n\n");

    for (int f = 0; f < lib_folder_count; f++)
    {
        lib_folder_t *fl = &lib_folders[f];
        snprintf(line, sizeof(line), "folder \"%s\" %d %d\n", fl->name, fl->gx, fl->gy);
        fput(fd, line);
        for (int k = 0; k < fl->kid_count; k++)
        { snprintf(line, sizeof(line), "  %s\n", fl->kids[k].path); fput(fd, line); }
    }
    for (int i = 0; i < lib_item_count; i++)
    {
        snprintf(line, sizeof(line), "item %s %d %d\n",
                 lib_items[i].path, lib_items[i].gx, lib_items[i].gy);
        fput(fd, line);
    }
    dobfs_Close(fd);
}

static bool library_add_item(const char *path, int gx, int gy)
{
    if (library_contains(path)) return false;
    if (lib_item_count >= MAX_LIB_ITEMS) return false;
    lib_item_t *it = &lib_items[lib_item_count++];
    memset(it, 0, sizeof(*it));
    strncpy(it->path, path, sizeof(it->path) - 1);
    it->gx = gx; it->gy = gy;
    return true;
}

static void library_remove_item(int idx)
{
    if (idx < 0 || idx >= lib_item_count) return;
    for (int i = idx; i < lib_item_count - 1; i++) lib_items[i] = lib_items[i + 1];
    lib_item_count--;
}

static bool folder_add_kid(int f, const char *path)
{
    if (f < 0 || f >= lib_folder_count) return false;
    if (library_contains(path)) return false;
    lib_folder_t *fl = &lib_folders[f];
    if (fl->kid_count >= MAX_FOLDER_KIDS) return false;
    strncpy(fl->kids[fl->kid_count].path, path, sizeof(fl->kids[0].path) - 1);
    fl->kid_count++;
    return true;
}

static void folder_remove_kid(int f, int k)
{
    if (f < 0 || f >= lib_folder_count) return;
    lib_folder_t *fl = &lib_folders[f];
    if (k < 0 || k >= fl->kid_count) return;
    for (int i = k; i < fl->kid_count - 1; i++) fl->kids[i] = fl->kids[i + 1];
    fl->kid_count--;
}

static int library_new_folder(const char *name, int cols)
{
    if (lib_folder_count >= MAX_LIB_FOLDERS) return -1;
    int gx, gy; first_free_cell(cols, &gx, &gy);
    lib_folder_t *f = &lib_folders[lib_folder_count];
    memset(f, 0, sizeof(*f));
    strncpy(f->name, name, sizeof(f->name) - 1);
    f->gx = gx; f->gy = gy;
    return lib_folder_count++;
}

static void library_delete_folder(int f)
{
    if (f < 0 || f >= lib_folder_count) return;
    for (int i = f; i < lib_folder_count - 1; i++) lib_folders[i] = lib_folders[i + 1];
    lib_folder_count--;
}

/* ===================================================================
 * UI state
 * =================================================================== */

typedef enum { TAB_ALL = 0, TAB_LIBRARY = 1 } tab_t;
static tab_t active_tab = TAB_LIBRARY;

typedef struct { bool is_sep; char sep_label[24]; int mod_idx; } flow_slot_t;
static flow_slot_t flow[MAX_MODULES + 4];
static int         flow_count = 0;

static int all_scroll = 0;
static int lib_scroll = 0;
static int selected_mod = -1;       /* selection in All tab            */
static int lib_sel_item = -1;       /* selection in Library tab        */
static int lib_sel_folder = -1;

/* Drag state */
static bool    dragging = false, drag_armed = false;
static int     press_x, press_y, drag_cur_x, drag_cur_y;
typedef enum { DRAG_NONE, DRAG_MODULE, DRAG_LIB_ITEM, DRAG_LIB_FOLDER, DRAG_FOLDER_KID } drag_kind_t;
static drag_kind_t drag_kind = DRAG_NONE;
static int  drag_mod_idx = -1, drag_item_idx = -1, drag_fold_idx = -1, drag_kid_idx = -1;
static char drag_label[64];

static bool overlay_open = false;
static int  overlay_folder = -1;

/* The full name to show in the status bar (selected element). */
static char status_name[96] = "";

/* Search box (All-programs tab) — uses the shared dob_textbox_t widget
 * (already linked via libdobui). Case-insensitive substring filter on
 * the module display name; empty box = no filtering. */
#include <textbox.h>
#define SEARCH_H     26
static dob_textbox_t search_tb;
static bool          search_tb_ready = false;

/* ===================================================================
 * Content area helpers
 * =================================================================== */

static int content_top(void)    { return TAB_H + (active_tab == TAB_ALL ? SEARCH_H : 0); }
static int content_bottom(void) { return g_h - STATUS_H; }
static int content_h(void)      { return content_bottom() - content_top(); }

static int tab_width(void) { return g_w / 2; }

/* ===================================================================
 * Tabs
 * =================================================================== */

static void draw_tabs(void)
{
    int tw = tab_width();
    dobui_FillRect(g_win, 0, 0, g_w, TAB_H, COL_RIBBON);

    /* Visual order: slot 0 (left) = Libreria, slot 1 (right) = Tutti. */
    const tab_t   slot_tab[2]   = { TAB_LIBRARY, TAB_ALL };
    const char   *slot_label[2] = { "Libreria", "Tutti i programmi" };
    for (int i = 0; i < 2; i++)
    {
        int x = i * tw;
        bool act = (active_tab == slot_tab[i]);
        uint32_t bg = act ? COL_TAB_ACT : COL_RIBBON;
        dobui_FillRect(g_win, x, 0, tw, TAB_H, bg);
        if (act) dobui_FillRect(g_win, x, TAB_H - 3, tw, 3, COL_YELLOW);
        if (i == 1) dobui_FillRect(g_win, x, 4, 1, TAB_H - 8, COL_NAVY_DK);

        int tlen = (int)strlen(slot_label[i]);
        int lx = x + (tw - tlen * 8) / 2;
        dobui_DrawText(g_win, lx, (TAB_H - 12) / 2, slot_label[i], COL_YELLOW, bg);
    }
}

/* ===================================================================
 * Status bar (bottom) — full name of the selected element
 * =================================================================== */

static void draw_status(void)
{
    int y = g_h - STATUS_H;
    dobui_FillRect(g_win, 0, y, g_w, STATUS_H, COL_NAVY_DK);
    dobui_FillRect(g_win, 0, y, g_w, 1, COL_SEP_LINE);

    const char *txt = status_name[0] ? status_name : "";
    /* clamp to width */
    char buf[96];
    int maxc = (g_w - 16) / 8;
    if (maxc > (int)sizeof(buf) - 1) maxc = (int)sizeof(buf) - 1;
    int n = (int)strlen(txt);
    if (n > maxc) n = maxc;
    memcpy(buf, txt, (uint32_t)n); buf[n] = '\0';
    dobui_DrawText(g_win, 8, y + (STATUS_H - 12) / 2, buf, COL_YELLOW, COL_NAVY_DK);
}

/* ===================================================================
 * "Tutti i programmi" tab
 * =================================================================== */

static int all_cols(void)
{
    int c = (g_w - CELL_PAD_X) / CELL_W;
    return c < 1 ? 1 : c;
}

/* Build the flow: Programmi, Giochi, and (only if show_drivers) Driver. */
/* Case-insensitive substring test of the search box against a name. */
static bool matches_search(const char *name)
{
    if (!search_tb_ready) return true;
    const char *q = dobtb_GetText(&search_tb);
    if (!q || q[0] == '\0') return true;

    char nl[80], ql[64];
    int i = 0;
    for (; name[i] && i < (int)sizeof(nl) - 1; i++)
    { char c = name[i]; if (c >= 'A' && c <= 'Z') c += 32; nl[i] = c; }
    nl[i] = '\0';
    int j = 0;
    for (; q[j] && j < (int)sizeof(ql) - 1; j++)
    { char c = q[j]; if (c >= 'A' && c <= 'Z') c += 32; ql[j] = c; }
    ql[j] = '\0';
    if (ql[0] == '\0') return true;
    return strstr(nl, ql) != NULL;
}

static void build_flow(void)
{
    flow_count = 0;
    struct { mod_type_t type; const char *label; bool include; } groups[3] = {
        { MOD_PROGRAM, "Programmi", true        },
        { MOD_GAME,    "Giochi",    true        },
        { MOD_DRIVER,  "Driver",    show_drivers },
    };

    for (int g = 0; g < 3; g++)
    {
        if (!groups[g].include) continue;
        bool any = false;
        for (int i = 0; i < module_count; i++)
            if (modules[i].type == groups[g].type && matches_search(modules[i].name)) { any = true; break; }
        if (!any) continue;

        flow_slot_t *s = &flow[flow_count++];
        s->is_sep = true;
        strncpy(s->sep_label, groups[g].label, sizeof(s->sep_label) - 1);
        s->sep_label[sizeof(s->sep_label) - 1] = '\0';
        s->mod_idx = -1;

        for (int i = 0; i < module_count; i++)
            if (modules[i].type == groups[g].type && matches_search(modules[i].name) &&
                flow_count < (int)(sizeof(flow)/sizeof(flow[0])))
            {
                flow_slot_t *m = &flow[flow_count++];
                m->is_sep = false; m->mod_idx = i;
            }
    }
}

static int all_content_height(void)
{
    int cols = all_cols(), y = 0, col = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep) { if (col != 0) { y += CELL_H; col = 0; } y += SEP_H; }
        else { col++; if (col >= cols) { y += CELL_H; col = 0; } }
    }
    if (col != 0) y += CELL_H;
    return y;
}

static void draw_all_tab(void)
{
    int top = content_top();
    int ch  = content_h();

    if (module_count == 0)
    {
        dobui_FillRect(g_win, 0, top, g_w, ch, COL_NAVY);
        dobui_DrawText(g_win, 20, top + 20, "(nessun modulo installato)", COL_TEXT_MUTE, COL_NAVY);
        return;
    }

    int cols = all_cols(), x0 = CELL_PAD_X;
    int bot = content_bottom();

    /* --- Pass 1: compose background + selection + icons + separator
     * lines into the offscreen, then a SINGLE blit. --- */
    if (ob_setup(0, top, g_w, ch))
    {
        ob_clear(COL_NAVY | 0xFF000000u);   /* opaque navy */
        set_clip(top, bot);                  /* keep icons off the tabs */

        int y = top - all_scroll, col = 0;
        for (int i = 0; i < flow_count; i++)
        {
            if (flow[i].is_sep)
            {
                if (col != 0) { y += CELL_H; col = 0; }
                if (y + SEP_H > top && y < bot)
                {
                    int line_x = x0 + (int)strlen(flow[i].sep_label) * 8 + 10;
                    if (line_x < g_w - CELL_PAD_X)
                        ob_fill(line_x, y + 9, g_w - CELL_PAD_X - line_x, 1, COL_SEP_LINE | 0xFF000000u);
                }
                y += SEP_H;
            }
            else
            {
                int cell_x = x0 + col * CELL_W, cell_y = y;
                if (cell_y + CELL_H > top && cell_y < bot)
                {
                    module_entry_t *m = &modules[flow[i].mod_idx];
                    bool sel = (flow[i].mod_idx == selected_mod);
                    if (sel)
                        ob_fill(cell_x + 3, cell_y + 2, CELL_W - 6, CELL_H - 6, COL_SEL_BG | 0xFF000000u);
                    if (!(dragging && drag_kind == DRAG_MODULE && drag_mod_idx == flow[i].mod_idx))
                    {
                        int icon_x = cell_x + (CELL_W - ICON_SIZE) / 2;
                        ob_module_icon(icon_x, cell_y + 6, m);
                    }
                }
                col++;
                if (col >= cols) { y += CELL_H; col = 0; }
            }
        }
        ob_flush();
    }

    /* --- Pass 2: text on top (separator labels + icon labels). Text
     * uses the glyph atlas and costs no texture-pool slots. Clip stays
     * active so labels don't spill onto the tabs during scroll. --- */
    set_clip(top, bot);
    int y = top - all_scroll, col = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep)
        {
            if (col != 0) { y += CELL_H; col = 0; }
            if (y + 4 >= top && y + 4 + 12 <= bot)
                dobui_DrawText(g_win, x0, y + 4, flow[i].sep_label, COL_TEXT_MUTE, COL_NAVY);
            y += SEP_H;
        }
        else
        {
            int cell_x = x0 + col * CELL_W, cell_y = y;
            if (cell_y + CELL_H > top && cell_y < bot)
            {
                module_entry_t *m = &modules[flow[i].mod_idx];
                bool sel = (flow[i].mod_idx == selected_mod);
                if (!(dragging && drag_kind == DRAG_MODULE && drag_mod_idx == flow[i].mod_idx))
                    draw_label(cell_x, cell_y + 6, m->name, CELL_W - 6,
                               sel ? COL_SEL_TEXT : COL_TEXT, sel ? COL_SEL_BG : COL_NAVY);
            }
            col++;
            if (col >= cols) { y += CELL_H; col = 0; }
        }
    }
    clear_clip();
}

/* ===================================================================
 * "Libreria" tab
 * =================================================================== */

static int lib_cols(void) { int c = (g_w - CELL_PAD_X) / CELL_W; return c < 1 ? 1 : c; }

static void lib_cell_origin(int gx, int gy, int *px, int *py)
{
    *px = CELL_PAD_X + gx * CELL_W;
    *py = content_top() + 6 + gy * CELL_H - lib_scroll;
}

static bool lib_pixel_to_cell(int lx, int ly, int *gx, int *gy)
{
    int rel_x = lx - CELL_PAD_X;
    int rel_y = ly - (content_top() + 6) + lib_scroll;
    if (rel_x < 0 || rel_y < 0) return false;
    *gx = rel_x / CELL_W; *gy = rel_y / CELL_H;
    if (*gx >= lib_cols()) return false;
    return true;
}

static int lib_content_height(void)
{
    int maxrow = 0;
    for (int i = 0; i < lib_item_count; i++) if (lib_items[i].gy > maxrow) maxrow = lib_items[i].gy;
    for (int f = 0; f < lib_folder_count; f++) if (lib_folders[f].gy > maxrow) maxrow = lib_folders[f].gy;
    return (maxrow + 2) * CELL_H;
}

static void draw_lib_tab(void)
{
    int top = content_top(), bot = content_bottom();
    int ch  = content_h();

    /* --- Pass 1: offscreen compose (background + selections + folder
     * tiles + item icons) → single blit. --- */
    if (ob_setup(0, top, g_w, ch))
    {
        ob_clear(COL_NAVY | 0xFF000000u);
        set_clip(top, bot);

        for (int f = 0; f < lib_folder_count; f++)
        {
            if (dragging && drag_kind == DRAG_LIB_FOLDER && drag_fold_idx == f) continue;
            int px, py; lib_cell_origin(lib_folders[f].gx, lib_folders[f].gy, &px, &py);
            if (py + CELL_H <= top || py >= bot) continue;
            if (f == lib_sel_folder)
                ob_fill(px + 3, py + 2, CELL_W - 6, CELL_H - 6, COL_SEL_BG | 0xFF000000u);
            int icon_x = px + (CELL_W - ICON_SIZE) / 2;
            ob_folder_tile(icon_x, py + 6, &lib_folders[f]);
        }

        for (int i = 0; i < lib_item_count; i++)
        {
            if (dragging && drag_kind == DRAG_LIB_ITEM && drag_item_idx == i) continue;
            int px, py; lib_cell_origin(lib_items[i].gx, lib_items[i].gy, &px, &py);
            if (py + CELL_H <= top || py >= bot) continue;
            if (i == lib_sel_item)
                ob_fill(px + 3, py + 2, CELL_W - 6, CELL_H - 6, COL_SEL_BG | 0xFF000000u);
            int idx = module_by_path(lib_items[i].path);
            int icon_x = px + (CELL_W - ICON_SIZE) / 2;
            if (idx >= 0) ob_module_icon(icon_x, py + 6, &modules[idx]);
            else          ob_dangling(icon_x, py + 6);
        }
        ob_flush();
    }

    /* Empty-state hint (text only). */
    if (lib_item_count == 0 && lib_folder_count == 0)
    {
        dobui_DrawText(g_win, 20, top + 20,
            "Trascina qui i programmi dalla scheda \"Tutti i programmi\".", COL_TEXT_MUTE, COL_NAVY);
        dobui_DrawText(g_win, 20, top + 40,
            "Usa il pannello per creare cartelle.", COL_TEXT_MUTE, COL_NAVY);
    }

    /* --- Pass 2: labels on top (clip active so they don't spill). --- */
    set_clip(top, bot);
    for (int f = 0; f < lib_folder_count; f++)
    {
        if (dragging && drag_kind == DRAG_LIB_FOLDER && drag_fold_idx == f) continue;
        int px, py; lib_cell_origin(lib_folders[f].gx, lib_folders[f].gy, &px, &py);
        if (py + CELL_H <= top || py >= bot) continue;
        bool sel = (f == lib_sel_folder);
        draw_label(px, py + 6, lib_folders[f].name, CELL_W - 6,
                   sel ? COL_SEL_TEXT : COL_TEXT, sel ? COL_SEL_BG : COL_NAVY);
    }
    for (int i = 0; i < lib_item_count; i++)
    {
        if (dragging && drag_kind == DRAG_LIB_ITEM && drag_item_idx == i) continue;
        int px, py; lib_cell_origin(lib_items[i].gx, lib_items[i].gy, &px, &py);
        if (py + CELL_H <= top || py >= bot) continue;
        bool sel = (i == lib_sel_item);
        int idx = module_by_path(lib_items[i].path);
        const char *nm;
        if (idx >= 0) nm = modules[idx].name;
        else
        {
            const char *tail = lib_items[i].path;
            for (const char *p = lib_items[i].path; *p; p++) if (*p == '/') tail = p + 1;
            nm = tail;
        }
        draw_label(px, py + 6, nm, CELL_W - 6,
                   sel ? COL_SEL_TEXT : COL_TEXT, sel ? COL_SEL_BG : COL_NAVY);
    }
    clear_clip();
}

/* ===================================================================
 * Folder overlay
 * =================================================================== */

#define OVL_MARGIN   36
#define OVL_PAD      14
#define OVL_TITLE_H  28

static void overlay_rect(int *ox, int *oy, int *ow, int *oh)
{
    *ox = OVL_MARGIN;
    *oy = content_top() + OVL_MARGIN;
    *ow = g_w - OVL_MARGIN * 2;
    *oh = content_bottom() - (content_top() + OVL_MARGIN) - OVL_MARGIN;
}

static int overlay_cols(int ow) { int c = (ow - OVL_PAD * 2 + CELL_PAD_X) / CELL_W; return c < 1 ? 1 : c; }

static void draw_overlay(void)
{
    if (!overlay_open || overlay_folder < 0 || overlay_folder >= lib_folder_count) return;
    lib_folder_t *f = &lib_folders[overlay_folder];

    int top = content_top(), ch = content_h();
    int ox, oy, ow, oh; overlay_rect(&ox, &oy, &ow, &oh);

    /* --- Pass 1: offscreen = dim scrim + popup body + child icons,
     * all in one blit. --- */
    if (ob_setup(0, top, g_w, ch))
    {
        ob_clear(COL_NAVY_DK | 0xFF000000u);               /* scrim */
        ob_fill(ox, oy, ow, oh, COL_NAVY_ALT | 0xFF000000u); /* body */
        ob_fill(ox, oy, ow, 1, COL_YELLOW | 0xFF000000u);
        ob_fill(ox, oy + oh - 1, ow, 1, COL_YELLOW | 0xFF000000u);
        ob_fill(ox, oy, 1, oh, COL_YELLOW | 0xFF000000u);
        ob_fill(ox + ow - 1, oy, 1, oh, COL_YELLOW | 0xFF000000u);
        ob_fill(ox, oy, ow, OVL_TITLE_H, COL_TAB_ACT | 0xFF000000u);

        if (f->kid_count > 0)
        {
            int cols = overlay_cols(ow);
            int gx0 = ox + OVL_PAD, gy0 = oy + OVL_TITLE_H + OVL_PAD;
            for (int k = 0; k < f->kid_count; k++)
            {
                int row = k / cols, colp = k % cols;
                int cx = gx0 + colp * CELL_W, cy = gy0 + row * CELL_H;
                if (cy + CELL_H > oy + oh - OVL_PAD) break;
                int idx = module_by_path(f->kids[k].path);
                int icon_x = cx + (CELL_W - ICON_SIZE) / 2;
                if (idx >= 0) ob_module_icon(icon_x, cy, &modules[idx]);
                else          ob_dangling(icon_x, cy);
            }
        }
        ob_flush();
        clear_clip();
    }

    /* --- Pass 2: text on top. --- */
    dobui_DrawText(g_win, ox + OVL_PAD, oy + (OVL_TITLE_H - 12) / 2, f->name, COL_YELLOW, COL_TAB_ACT);
    dobui_DrawText(g_win, ox + ow - 20, oy + (OVL_TITLE_H - 12) / 2, "x", COL_RED, COL_TAB_ACT);

    if (f->kid_count == 0)
    {
        dobui_DrawText(g_win, ox + OVL_PAD, oy + OVL_TITLE_H + OVL_PAD,
                       "(cartella vuota)", COL_TEXT_MUTE, COL_NAVY_ALT);
        return;
    }

    int cols = overlay_cols(ow);
    int gx0 = ox + OVL_PAD, gy0 = oy + OVL_TITLE_H + OVL_PAD;
    for (int k = 0; k < f->kid_count; k++)
    {
        int row = k / cols, colp = k % cols;
        int cx = gx0 + colp * CELL_W, cy = gy0 + row * CELL_H;
        if (cy + CELL_H > oy + oh - OVL_PAD) break;
        int idx = module_by_path(f->kids[k].path);
        const char *nm;
        if (idx >= 0) nm = modules[idx].name;
        else
        {
            const char *tail = f->kids[k].path;
            for (const char *p = f->kids[k].path; *p; p++) if (*p == '/') tail = p + 1;
            nm = tail;
        }
        draw_label(cx, cy, nm, CELL_W - 6, COL_TEXT, COL_NAVY_ALT);
    }
}

/* ===================================================================
 * Drag ghost
 * =================================================================== */

/* Ghost icon: blit the SAME shared buffer the grid uses for this
 * module (m->icon_px for embedded, or a gear/ghost/folder buffer).
 * Those pointers are stable AND their content never changes, so the
 * server's pointer-keyed texture cache stays correct. The earlier
 * approaches (one scratch buffer, then a rotating pool) reused
 * pointers whose CONTENT changed between drags; the cache, keyed on
 * pointer, skipped the re-upload and froze the ghost on the first
 * icon. Blitting the fixed shared buffers sidesteps that entirely. */
static const uint32_t *module_icon_buf(module_entry_t *m, int *w, int *h)
{
    ensure_icon(m);
    if (m->icon_px && m->icon_w > 0 && m->icon_h > 0)
    { *w = m->icon_w; *h = m->icon_h; return m->icon_px; }
    build_gears();
    *w = ICON_SIZE; *h = ICON_SIZE;
    if (m->type == MOD_GAME) return ghost_icon;
    if (m->type == MOD_DRIVER) return m->is_driver ? gear_buf_drv_chip : gear_buf[MOD_DRIVER];
    return gear_buf[m->type];
}

static const uint32_t *dangling_buf(void)
{
    static uint32_t buf[ICON_SIZE * ICON_SIZE];
    static bool built = false;
    if (!built) { build_one_gear(buf, BPX_DANGLE, false); built = true; }
    return buf;
}

static void draw_drag_ghost(void)
{
    if (!dragging) return;
    int gx = drag_cur_x - ICON_SIZE / 2, gy = drag_cur_y - ICON_SIZE / 2;

    const uint32_t *src = NULL; int sw = ICON_SIZE, sh = ICON_SIZE;

    if (drag_kind == DRAG_MODULE && drag_mod_idx >= 0)
        src = module_icon_buf(&modules[drag_mod_idx], &sw, &sh);
    else if (drag_kind == DRAG_LIB_ITEM && drag_item_idx >= 0)
    {
        int idx = module_by_path(lib_items[drag_item_idx].path);
        src = (idx >= 0) ? module_icon_buf(&modules[idx], &sw, &sh) : dangling_buf();
    }
    else if (drag_kind == DRAG_LIB_FOLDER && drag_fold_idx >= 0)
    { build_folder_icon(); src = folder_icon; }
    else if (drag_kind == DRAG_FOLDER_KID && drag_fold_idx >= 0)
    {
        int idx = module_by_path(lib_folders[drag_fold_idx].kids[drag_kid_idx].path);
        src = (idx >= 0) ? module_icon_buf(&modules[idx], &sw, &sh) : dangling_buf();
    }

    if (src)
    {
        int ox = gx + (ICON_SIZE - sw) / 2, oy = gy + (ICON_SIZE - sh) / 2;
        dobui_BlitBuffer(g_win, ox, oy, src, sw, sh);
    }

    if (drag_label[0])
    {
        int tlen = (int)strlen(drag_label); if (tlen > 12) tlen = 12;
        int lx = drag_cur_x - tlen * 4;
        dobui_FillRect(g_win, lx - 3, gy + ICON_SIZE + 2, tlen * 8 + 6, 14, COL_SEL_BG);
        char tmp[16]; memcpy(tmp, drag_label, (uint32_t)tlen); tmp[tlen] = '\0';
        dobui_DrawText(g_win, lx, gy + ICON_SIZE + 3, tmp, COL_SEL_TEXT, COL_SEL_BG);
    }
}

/* ===================================================================
 * Master redraw
 * =================================================================== */

static void redraw(void)
{
    /* Safety wipe: clear the whole content area to navy before drawing
     * anything. The per-tab offscreen blit should already cover it, but
     * this guarantees nothing from a previous tab/state can bleed
     * through (e.g. the All-tab icons showing under an empty Library). */
    dobui_FillRect(g_win, 0, TAB_H, g_w, g_h - TAB_H - STATUS_H, COL_NAVY);

    draw_tabs();

    if (active_tab == TAB_ALL)
    {
        /* Search bar strip under the tabs: label + textbox widget. */
        int sy = TAB_H;
        dobui_FillRect(g_win, 0, sy, g_w, SEARCH_H, COL_RIBBON);
        dobui_DrawText(g_win, 8, sy + (SEARCH_H - 12) / 2, "Cerca:", COL_TEXT_MUTE, COL_RIBBON);
        if (search_tb_ready)
        {
            /* keep the widget positioned/sized to the current width */
            search_tb.x = 8 + 7 * 8;
            search_tb.y = sy + 2;
            search_tb.w = g_w - search_tb.x - 12;
            search_tb.h = SEARCH_H - 4;
            dobtb_Draw(&search_tb);
        }

        build_flow();
        draw_all_tab();
    }
    else
        draw_lib_tab();

    if (overlay_open) draw_overlay();
    draw_drag_ghost();
    draw_status();
    dobui_Invalidate(g_win);
}

/* ===================================================================
 * Hit testing
 * =================================================================== */

static int all_hit(int lx, int ly)
{
    if (ly < content_top() || ly >= content_bottom()) return -1;
    int cols = all_cols(), x0 = CELL_PAD_X;
    int y = content_top() - all_scroll, col = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep) { if (col != 0) { y += CELL_H; col = 0; } y += SEP_H; }
        else
        {
            int cell_x = x0 + col * CELL_W, cell_y = y;
            if (lx >= cell_x && lx < cell_x + CELL_W && ly >= cell_y && ly < cell_y + CELL_H)
                return flow[i].mod_idx;
            col++; if (col >= cols) { y += CELL_H; col = 0; }
        }
    }
    return -1;
}

typedef enum { LH_NONE, LH_ITEM, LH_FOLDER, LH_EMPTY } lib_hit_kind_t;
typedef struct { lib_hit_kind_t kind; int idx; int gx, gy; } lib_hit_t;

static lib_hit_t lib_hit(int lx, int ly)
{
    lib_hit_t h = { LH_NONE, -1, 0, 0 };
    if (ly < content_top() || ly >= content_bottom()) return h;
    int gx, gy;
    if (!lib_pixel_to_cell(lx, ly, &gx, &gy)) return h;
    h.gx = gx; h.gy = gy;
    for (int f = 0; f < lib_folder_count; f++)
        if (lib_folders[f].gx == gx && lib_folders[f].gy == gy) { h.kind = LH_FOLDER; h.idx = f; return h; }
    for (int i = 0; i < lib_item_count; i++)
        if (lib_items[i].gx == gx && lib_items[i].gy == gy) { h.kind = LH_ITEM; h.idx = i; return h; }
    h.kind = LH_EMPTY;
    return h;
}

static int overlay_hit(int lx, int ly)
{
    int ox, oy, ow, oh; overlay_rect(&ox, &oy, &ow, &oh);
    if (lx < ox || lx >= ox + ow || ly < oy || ly >= oy + oh) return -3;
    if (ly < oy + OVL_TITLE_H && lx >= ox + ow - 28) return -2;
    if (ly < oy + OVL_TITLE_H) return -1;

    lib_folder_t *f = &lib_folders[overlay_folder];
    int cols = overlay_cols(ow);
    int gx0 = ox + OVL_PAD, gy0 = oy + OVL_TITLE_H + OVL_PAD;
    for (int k = 0; k < f->kid_count; k++)
    {
        int row = k / cols, colp = k % cols;
        int cx = gx0 + colp * CELL_W, cy = gy0 + row * CELL_H;
        if (lx >= cx && lx < cx + CELL_W && ly >= cy && ly < cy + CELL_H) return k;
    }
    return -1;
}

/* ===================================================================
 * Status-name helpers
 * =================================================================== */

static void set_status_module(int idx)
{
    if (idx < 0 || idx >= module_count) { status_name[0] = '\0'; return; }
    strncpy(status_name, modules[idx].name, sizeof(status_name) - 1);
    status_name[sizeof(status_name) - 1] = '\0';
}

static void set_status_path(const char *path)
{
    int idx = module_by_path(path);
    if (idx >= 0) { set_status_module(idx); return; }
    const char *tail = path;
    for (const char *p = path; *p; p++) if (*p == '/') tail = p + 1;
    strncpy(status_name, tail, sizeof(status_name) - 1);
    status_name[sizeof(status_name) - 1] = '\0';
}

/* ===================================================================
 * Launching
 * =================================================================== */

static bool resolve_reference(const char *ref_path, char *out, int out_sz)
{
    int fd = dobfs_Open(ref_path, FS_READ);
    if (fd < 0) return false;
    char buf[200];
    int n = dobfs_Read(fd, buf, sizeof(buf) - 1);
    dobfs_Close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    char *p = buf;
    while (*p)
    {
        char *eol = p; while (*eol && *eol != '\n' && *eol != '\r') eol++;
        *eol = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#')
        {
            strncpy(out, p, out_sz - 1); out[out_sz - 1] = '\0';
            int e = (int)strlen(out);
            while (e > 0 && (out[e-1] == ' ' || out[e-1] == '\t')) out[--e] = '\0';
            return out[0] != '\0';
        }
        p = eol + 1;
    }
    return false;
}

static void launch_module(int idx)
{
    if (idx < 0 || idx >= module_count) return;
    module_entry_t *m = &modules[idx];
    const char *path = m->mdl_path[0] ? m->mdl_path : m->mdl_name;
    if (m->is_driver) dobui_SpawnDriver(path, NULL);
    else              spawn_file(path, NULL);
}

static void launch_path(const char *path)
{
    char target[160];
    const char *eff = path;
    if (str_ends_with(path, ".reference"))
    {
        if (!resolve_reference(path, target, sizeof(target)))
        { dobpopup_Error("Collegamento", "Collegamento non valido."); return; }
        eff = target;
    }
    int idx = module_by_path(eff);
    if (idx >= 0) { launch_module(idx); return; }
    if (str_ends_with(eff, ".mdl") || str_ends_with(eff, ".MDL")) spawn_file(eff, NULL);
    else dobpopup_Warning("Apri", "Impossibile aprire questo elemento.");
}

/* ===================================================================
 * Dynamic panel
 *
 * event_panel delivers an index into the CURRENT command list. Because
 * the list changes (the driver toggle relabels, library vs all differ),
 * we keep a parallel array of action ids built alongside the labels, so
 * an index maps to a stable action regardless of list shape.
 * =================================================================== */

typedef enum {
    ACT_OPEN, ACT_ADD_LIB, ACT_NEW_FOLDER, ACT_RENAME, ACT_REMOVE,
    ACT_MAKE_REF, ACT_UNINSTALL, ACT_TOGGLE_DRV, ACT_REFRESH
} panel_action_t;

static panel_action_t panel_actions[16];
static int panel_action_count = 0;

static void rebuild_panel(void)
{
    char menu[256];
    int n = 0;
    panel_action_count = 0;

    #define ADD(label, act) do { \
        int _l = (int)strlen(label); \
        if (n + _l + 1 < (int)sizeof(menu)) { \
            if (n) menu[n++] = '\n'; \
            memcpy(menu + n, (label), _l); n += _l; menu[n] = '\0'; \
            panel_actions[panel_action_count++] = (act); } } while (0)

    ADD("Apri", ACT_OPEN);
    if (active_tab == TAB_ALL)
        ADD("Aggiungi alla libreria", ACT_ADD_LIB);
    ADD("Nuova cartella", ACT_NEW_FOLDER);
    if (active_tab == TAB_LIBRARY)
    {
        ADD("Rinomina cartella", ACT_RENAME);
        ADD("Rimuovi dalla libreria", ACT_REMOVE);
    }
    if (active_tab == TAB_ALL)
        ADD("Crea collegamento", ACT_MAKE_REF);
    if (active_tab == TAB_ALL)
        ADD("Disinstalla", ACT_UNINSTALL);
    ADD(show_drivers ? "Nascondi driver" : "Mostra driver", ACT_TOGGLE_DRV);
    ADD("Aggiorna", ACT_REFRESH);

    #undef ADD
    dobui_set_panel(menu);
}

/* ===================================================================
 * Drag lifecycle
 * =================================================================== */

static void reset_drag(void)
{
    dragging = false; drag_armed = false; drag_kind = DRAG_NONE;
    drag_mod_idx = drag_item_idx = drag_fold_idx = drag_kid_idx = -1;
    drag_label[0] = '\0';
}

static void arm_drag_from_all(int lx, int ly, int mod_idx)
{
    drag_armed = true; press_x = lx; press_y = ly;
    drag_kind = DRAG_MODULE; drag_mod_idx = mod_idx;
    strncpy(drag_label, modules[mod_idx].name, sizeof(drag_label) - 1);
    drag_label[sizeof(drag_label) - 1] = '\0';
}

static void arm_drag_from_lib(int lx, int ly, lib_hit_t h)
{
    drag_armed = true; press_x = lx; press_y = ly;
    lib_sel_item = lib_sel_folder = -1;
    if (h.kind == LH_ITEM)
    {
        drag_kind = DRAG_LIB_ITEM; drag_item_idx = h.idx; lib_sel_item = h.idx;
        int idx = module_by_path(lib_items[h.idx].path);
        strncpy(drag_label, idx >= 0 ? modules[idx].name : lib_items[h.idx].path, sizeof(drag_label) - 1);
        set_status_path(lib_items[h.idx].path);
    }
    else if (h.kind == LH_FOLDER)
    {
        drag_kind = DRAG_LIB_FOLDER; drag_fold_idx = h.idx; lib_sel_folder = h.idx;
        strncpy(drag_label, lib_folders[h.idx].name, sizeof(drag_label) - 1);
        strncpy(status_name, lib_folders[h.idx].name, sizeof(status_name) - 1);
        status_name[sizeof(status_name)-1] = '\0';
    }
    drag_label[sizeof(drag_label) - 1] = '\0';
}

static void handle_drop(int lx, int ly)
{
    int cols = lib_cols();

    if (overlay_open && drag_kind == DRAG_MODULE)
    {
        int oh = overlay_hit(lx, ly);
        if (oh != -3)
        { folder_add_kid(overlay_folder, modules[drag_mod_idx].mdl_path); library_save(); return; }
    }

    if (active_tab == TAB_LIBRARY)
    {
        lib_hit_t h = lib_hit(lx, ly);

        if (drag_kind == DRAG_MODULE)
        {
            const char *p = modules[drag_mod_idx].mdl_path;
            if (h.kind == LH_FOLDER) folder_add_kid(h.idx, p);
            else if (h.kind == LH_ITEM)
            {
                char other[160];
                strncpy(other, lib_items[h.idx].path, sizeof(other) - 1); other[sizeof(other)-1]='\0';
                int gx = lib_items[h.idx].gx, gy = lib_items[h.idx].gy;
                library_remove_item(h.idx);
                int nf = library_new_folder("Nuova cartella", cols);
                if (nf >= 0)
                {
                    lib_folders[nf].gx = gx; lib_folders[nf].gy = gy;
                    folder_add_kid(nf, other); folder_add_kid(nf, p);
                }
            }
            else
            {
                int gx = h.gx, gy = h.gy;
                if (h.kind == LH_NONE) first_free_cell(cols, &gx, &gy);
                else if (cell_occupied(gx, gy, -1, -1)) first_free_cell(cols, &gx, &gy);
                library_add_item(p, gx, gy);
            }
            library_save();
        }
        else if (drag_kind == DRAG_LIB_ITEM)
        {
            if (h.kind == LH_ITEM && h.idx != drag_item_idx)
            {
                /* Drop an item onto another item → make a folder with
                 * both (Android-style). Remove the higher index first
                 * so the lower index stays valid. */
                char dragged[160], target[160];
                strncpy(dragged, lib_items[drag_item_idx].path, sizeof(dragged)-1); dragged[sizeof(dragged)-1]='\0';
                strncpy(target,  lib_items[h.idx].path,         sizeof(target)-1);  target[sizeof(target)-1]='\0';
                int gx = lib_items[h.idx].gx, gy = lib_items[h.idx].gy;
                int a = drag_item_idx, b = h.idx;
                if (a < b) { int t = a; a = b; b = t; }   /* a = higher */
                library_remove_item(a);
                library_remove_item(b);
                int nf = library_new_folder("Nuova cartella", cols);
                if (nf >= 0)
                {
                    lib_folders[nf].gx = gx; lib_folders[nf].gy = gy;
                    folder_add_kid(nf, target);
                    folder_add_kid(nf, dragged);
                }
                lib_sel_item = -1;
            }
            else if (h.kind == LH_EMPTY && !cell_occupied(h.gx, h.gy, -1, drag_item_idx))
            { lib_items[drag_item_idx].gx = h.gx; lib_items[drag_item_idx].gy = h.gy; }
            else if (h.kind == LH_FOLDER)
            {
                char p[160]; strncpy(p, lib_items[drag_item_idx].path, sizeof(p)-1); p[sizeof(p)-1]='\0';
                library_remove_item(drag_item_idx);
                folder_add_kid(h.idx, p);
                lib_sel_item = -1;
            }
            library_save();
        }
        else if (drag_kind == DRAG_LIB_FOLDER)
        {
            if (h.kind == LH_EMPTY && !cell_occupied(h.gx, h.gy, drag_fold_idx, -1))
            { lib_folders[drag_fold_idx].gx = h.gx; lib_folders[drag_fold_idx].gy = h.gy; library_save(); }
        }
        else if (drag_kind == DRAG_FOLDER_KID)
        {
            char p[160];
            strncpy(p, lib_folders[drag_fold_idx].kids[drag_kid_idx].path, sizeof(p)-1); p[sizeof(p)-1]='\0';
            folder_remove_kid(drag_fold_idx, drag_kid_idx);
            int gx = h.gx, gy = h.gy;
            if (h.kind != LH_EMPTY || cell_occupied(gx, gy, -1, -1)) first_free_cell(cols, &gx, &gy);
            library_add_item(p, gx, gy);
            library_save();
        }
    }
}

/* ===================================================================
 * Event handlers
 * =================================================================== */

void event_mouseclick(int x, int y, uint8_t buttons)
{
    (void)buttons;

    if (overlay_open)
    {
        int oh = overlay_hit(x, y);
        if (oh == -2 || oh == -3)
        { overlay_open = false; overlay_folder = -1; redraw(); return; }
        if (oh >= 0)
        {
            drag_armed = true; press_x = x; press_y = y;
            drag_kind = DRAG_FOLDER_KID; drag_kid_idx = oh; drag_fold_idx = overlay_folder;
            int idx = module_by_path(lib_folders[overlay_folder].kids[oh].path);
            strncpy(drag_label, idx >= 0 ? modules[idx].name :
                    lib_folders[overlay_folder].kids[oh].path, sizeof(drag_label) - 1);
            drag_label[sizeof(drag_label) - 1] = '\0';
            set_status_path(lib_folders[overlay_folder].kids[oh].path);
            redraw();
        }
        return;
    }

    /* Click inside the search strip (All tab) → focus/position the
     * search textbox. */
    if (active_tab == TAB_ALL && search_tb_ready &&
        y >= TAB_H && y < TAB_H + SEARCH_H)
    {
        dobtb_SetFocus(&search_tb, true);
        dobtb_OnClick(&search_tb, x, y);
        redraw();
        return;
    }
    /* Click anywhere else drops search focus (so typing doesn't keep
     * going to the box once the user moves on). */
    if (search_tb_ready && search_tb.focused && !(y >= TAB_H && y < TAB_H + SEARCH_H))
        dobtb_SetFocus(&search_tb, false);

    if (y < TAB_H)
    {
        tab_t nt = (x < tab_width()) ? TAB_LIBRARY : TAB_ALL;
        if (nt != active_tab)
        {
            active_tab = nt; selected_mod = -1; lib_sel_item = lib_sel_folder = -1;
            status_name[0] = '\0';
            reset_drag(); rebuild_panel(); redraw();
        }
        return;
    }

    if (active_tab == TAB_ALL)
    {
        int mi = all_hit(x, y);
        selected_mod = mi;
        set_status_module(mi);
        if (mi >= 0) arm_drag_from_all(x, y, mi);
        redraw();
    }
    else
    {
        lib_hit_t h = lib_hit(x, y);
        if (h.kind == LH_ITEM || h.kind == LH_FOLDER) arm_drag_from_lib(x, y, h);
        else { lib_sel_item = lib_sel_folder = -1; status_name[0] = '\0'; }
        redraw();
    }
}

void event_mousemove(int x, int y, uint8_t buttons)
{
    (void)buttons;
    /* Drag inside the focused search box extends its selection; it never arms
     * the module-icon drag (that click path returns early), so handle it first. */
    if (search_tb_ready && search_tb.focused && dobtb_OnDrag(&search_tb, x, y)) { redraw(); return; }
    drag_cur_x = x; drag_cur_y = y;
    if (drag_armed && !dragging)
    {
        int dx = x - press_x, dy = y - press_y;
        if (dx*dx + dy*dy >= DRAG_THRESHOLD * DRAG_THRESHOLD) dragging = true;
    }

    /* Hover-switch: while dragging a module from "Tutti", moving the
     * pointer onto the Libreria tab (left slot) opens it, so the user
     * can drop the icon straight into the library grid — drag the icon
     * literally onto the word "Libreria". */
    if (dragging && drag_kind == DRAG_MODULE && y < TAB_H &&
        x < tab_width() && active_tab != TAB_LIBRARY)
    {
        active_tab = TAB_LIBRARY;
        rebuild_panel();
    }

    if (dragging) redraw();
}

void event_mouserelease(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (search_tb_ready) dobtb_OnRelease(&search_tb);   /* end any in-flight text selection */
    if (dragging) { handle_drop(x, y); reset_drag(); rebuild_panel(); redraw(); return; }
    reset_drag();
}

void event_dblclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    reset_drag();

    if (overlay_open)
    {
        int oh = overlay_hit(x, y);
        if (oh >= 0) launch_path(lib_folders[overlay_folder].kids[oh].path);
        else if (oh == -2 || oh == -3) { overlay_open = false; overlay_folder = -1; redraw(); }
        return;
    }
    /* Double-click in the search strip selects the word (then the whole query). */
    if (active_tab == TAB_ALL && search_tb_ready && y >= TAB_H && y < TAB_H + SEARCH_H)
    {
        dobtb_SetFocus(&search_tb, true);
        if (dobtb_OnDblClick(&search_tb, x, y)) redraw();
        return;
    }
    if (y < TAB_H) return;

    if (active_tab == TAB_ALL)
    {
        int mi = all_hit(x, y);
        if (mi >= 0) launch_module(mi);
    }
    else
    {
        lib_hit_t h = lib_hit(x, y);
        if (h.kind == LH_ITEM) launch_path(lib_items[h.idx].path);
        else if (h.kind == LH_FOLDER) { overlay_open = true; overlay_folder = h.idx; redraw(); }
    }
}

void event_scroll(int delta)
{
    if (overlay_open) return;
    if (active_tab == TAB_ALL)
    {
        all_scroll += delta * 24;
        if (all_scroll < 0) all_scroll = 0;
        int maxs = all_content_height() - content_h(); if (maxs < 0) maxs = 0;
        if (all_scroll > maxs) all_scroll = maxs;
    }
    else
    {
        lib_scroll += delta * 24;
        if (lib_scroll < 0) lib_scroll = 0;
        int maxs = lib_content_height() - content_h(); if (maxs < 0) maxs = 0;
        if (lib_scroll > maxs) lib_scroll = maxs;
    }
    redraw();
}

void event_resize(int w, int h) { g_w = w; g_h = h; redraw(); }

/* Move the All-tab selection by (dcol, drow) through the flow grid,
 * skipping separators. Keeps selection within bounds. */
static void all_nav(int dcol, int drow)
{
    int cols = all_cols();
    /* Build the list of selectable module indices in flow order, and
     * note the grid (row,col) of each as laid out by draw_all_tab. */
    /* Find current selected position in flow. */
    int cur_flow = -1;
    for (int i = 0; i < flow_count; i++)
        if (!flow[i].is_sep && flow[i].mod_idx == selected_mod) { cur_flow = i; break; }

    if (cur_flow < 0)
    {
        /* Nothing selected → pick the first module. */
        for (int i = 0; i < flow_count; i++)
            if (!flow[i].is_sep) { selected_mod = flow[i].mod_idx; set_status_module(selected_mod); return; }
        return;
    }

    /* Compute (row,col) coordinates per the same packing as draw,
     * locating the current selection's row/col. */
    int row = 0, col = 0;
    int cur_row = 0, cur_col = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep) { if (col != 0) { row++; col = 0; } row++; continue; }
        if (i == cur_flow) { cur_row = row; cur_col = col; }
        col++; if (col >= cols) { row++; col = 0; }
    }

    int want_row = cur_row + drow;
    int want_col = cur_col + dcol;
    if (want_col < 0) want_col = 0;
    if (want_col >= cols) want_col = cols - 1;

    /* Second pass: find the module slot closest to (want_row, want_col);
     * if exact cell is empty, fall back to nearest in that row, then to
     * the original selection. */
    int best = selected_mod, best_exact = -1, best_row_any = -1;
    row = 0; col = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep) { if (col != 0) { row++; col = 0; } row++; continue; }
        if (row == want_row)
        {
            if (col == want_col) best_exact = flow[i].mod_idx;
            if (best_row_any < 0 || col <= want_col) best_row_any = flow[i].mod_idx;
        }
        col++; if (col >= cols) { row++; col = 0; }
    }
    if (best_exact >= 0) best = best_exact;
    else if (best_row_any >= 0) best = best_row_any;

    selected_mod = best;
    set_status_module(selected_mod);

    /* Scroll to keep selection visible. */
    /* Recompute selected cell's y and adjust all_scroll. */
    int top = content_top();
    int yy = top - all_scroll, cc = 0;
    for (int i = 0; i < flow_count; i++)
    {
        if (flow[i].is_sep) { if (cc != 0) { yy += CELL_H; cc = 0; } yy += SEP_H; continue; }
        if (flow[i].mod_idx == selected_mod)
        {
            if (yy < top) all_scroll -= (top - yy);
            else if (yy + CELL_H > content_bottom()) all_scroll += (yy + CELL_H - content_bottom());
            if (all_scroll < 0) all_scroll = 0;
            break;
        }
        cc++; if (cc >= cols) { yy += CELL_H; cc = 0; }
    }
}

/* Move the Library selection. The library is a free grid; we move to
 * the nearest occupied cell in the requested direction. */
static void lib_nav(int dcol, int drow)
{
    int cur_gx = 0, cur_gy = 0; bool have_cur = false;
    if (lib_sel_item >= 0)   { cur_gx = lib_items[lib_sel_item].gx; cur_gy = lib_items[lib_sel_item].gy; have_cur = true; }
    else if (lib_sel_folder >= 0) { cur_gx = lib_folders[lib_sel_folder].gx; cur_gy = lib_folders[lib_sel_folder].gy; have_cur = true; }

    if (!have_cur)
    {
        /* select the first thing (lowest gy,gx) */
        if (lib_folder_count > 0) { lib_sel_folder = 0; }
        else if (lib_item_count > 0) { lib_sel_item = 0; }
        return;
    }

    int want_gx = cur_gx + dcol, want_gy = cur_gy + drow;
    if (want_gx < 0) want_gx = 0;
    if (want_gy < 0) want_gy = 0;

    /* Find an occupied cell at (want_gx,want_gy); else nearest in row. */
    int pick_item = -1, pick_folder = -1;
    for (int f = 0; f < lib_folder_count; f++)
        if (lib_folders[f].gx == want_gx && lib_folders[f].gy == want_gy) pick_folder = f;
    for (int i = 0; i < lib_item_count; i++)
        if (lib_items[i].gx == want_gx && lib_items[i].gy == want_gy) pick_item = i;

    if (pick_folder < 0 && pick_item < 0)
    {
        /* nearest occupant on the target row (by |gx-want_gx|) */
        int bestd = 1 << 30;
        for (int f = 0; f < lib_folder_count; f++)
            if (lib_folders[f].gy == want_gy)
            { int d = lib_folders[f].gx - want_gx; if (d<0)d=-d; if (d<bestd){bestd=d;pick_folder=f;pick_item=-1;} }
        for (int i = 0; i < lib_item_count; i++)
            if (lib_items[i].gy == want_gy)
            { int d = lib_items[i].gx - want_gx; if (d<0)d=-d; if (d<bestd){bestd=d;pick_item=i;pick_folder=-1;} }
    }

    if (pick_folder >= 0) { lib_sel_folder = pick_folder; lib_sel_item = -1;
        strncpy(status_name, lib_folders[pick_folder].name, sizeof(status_name)-1); status_name[sizeof(status_name)-1]='\0'; }
    else if (pick_item >= 0) { lib_sel_item = pick_item; lib_sel_folder = -1; set_status_path(lib_items[pick_item].path); }
}

/* Keyboard: search text on the All tab; arrow/Enter navigation on
 * both tabs. Arrows always navigate even while the search box has
 * focus (so the user can type then arrow into the results). */
void event_key(uint8_t key)
{
    /* Overlay open: Enter opens the highlighted child? Keep simple —
     * Esc closes the overlay. */
    if (overlay_open)
    {
        if (key == 27) { overlay_open = false; overlay_folder = -1; redraw(); }
        return;
    }

    /* Navigation keys (arrows / Enter) handled first, for both tabs. */
    if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT)
    {
        int dcol = (key == KEY_LEFT) ? -1 : (key == KEY_RIGHT) ? 1 : 0;
        int drow = (key == KEY_UP)   ? -1 : (key == KEY_DOWN)  ? 1 : 0;
        if (active_tab == TAB_ALL) all_nav(dcol, drow);
        else                       lib_nav(dcol, drow);
        redraw();
        return;
    }
    if (key == '\n' || key == '\r')          /* Enter = open */
    {
        if (active_tab == TAB_ALL)
        { if (selected_mod >= 0) launch_module(selected_mod); }
        else
        {
            if (lib_sel_folder >= 0) { overlay_open = true; overlay_folder = lib_sel_folder; redraw(); }
            else if (lib_sel_item >= 0) launch_path(lib_items[lib_sel_item].path);
        }
        return;
    }

    /* Otherwise, on the All tab, feed the search box. */
    if (active_tab == TAB_ALL && search_tb_ready)
    {
        bool changed = dobtb_OnKey(&search_tb, key);
        if (changed) { selected_mod = -1; all_scroll = 0; redraw(); }
    }
}

/* ===================================================================
 * Panel command implementations
 * =================================================================== */

static void cmd_open(void)
{
    if (active_tab == TAB_ALL)
    { if (selected_mod >= 0) launch_module(selected_mod); }
    else
    {
        if (lib_sel_folder >= 0) { overlay_open = true; overlay_folder = lib_sel_folder; redraw(); }
        else if (lib_sel_item >= 0) launch_path(lib_items[lib_sel_item].path);
    }
}

static void cmd_add_to_library(void)
{
    if (active_tab != TAB_ALL || selected_mod < 0)
    { dobpopup_Info("Libreria", "Seleziona un programma da \"Tutti i programmi\"."); return; }
    int cols = lib_cols(), gx, gy; first_free_cell(cols, &gx, &gy);
    if (library_add_item(modules[selected_mod].mdl_path, gx, gy))
    { library_save(); dobpopup_Info("Libreria", "Aggiunto alla libreria."); }
    else dobpopup_Info("Libreria", "Questo programma è già nella libreria.");
}

static void cmd_new_folder(void)
{
    char name[48];
    int r = dobpopup_InputBox("Nuova cartella", "Nome della cartella:", "Nuova cartella", name, sizeof(name));
    if (r != 0 || name[0] == '\0') return;
    int cols = lib_cols();
    int nf = library_new_folder(name, cols);
    if (nf < 0) { dobpopup_Error("Cartella", "Limite cartelle raggiunto."); return; }
    library_save();
    if (active_tab != TAB_LIBRARY) { active_tab = TAB_LIBRARY; rebuild_panel(); }
    redraw();
}

static void cmd_rename_folder(void)
{
    if (lib_sel_folder < 0)
    { dobpopup_Info("Rinomina", "Seleziona prima una cartella nella libreria."); return; }
    char name[48];
    strncpy(name, lib_folders[lib_sel_folder].name, sizeof(name) - 1); name[sizeof(name)-1]='\0';
    int r = dobpopup_InputBox("Rinomina cartella", "Nuovo nome:", name, name, sizeof(name));
    if (r != 0 || name[0] == '\0') return;
    strncpy(lib_folders[lib_sel_folder].name, name, sizeof(lib_folders[0].name) - 1);
    lib_folders[lib_sel_folder].name[sizeof(lib_folders[0].name) - 1] = '\0';
    library_save(); redraw();
}

static void cmd_remove_from_library(void)
{
    if (active_tab != TAB_LIBRARY)
    { dobpopup_Info("Rimuovi", "Passa alla scheda Libreria e seleziona un elemento."); return; }
    if (lib_sel_folder >= 0)
    {
        int c = dobpopup_YesNo("Rimuovi cartella",
            "Rimuovere la cartella dalla libreria?\n(I programmi restano installati.)");
        if (c != 0) return;
        library_delete_folder(lib_sel_folder); lib_sel_folder = -1; status_name[0]='\0';
        library_save(); redraw();
    }
    else if (lib_sel_item >= 0)
    { library_remove_item(lib_sel_item); lib_sel_item = -1; status_name[0]='\0'; library_save(); redraw(); }
    else dobpopup_Info("Rimuovi", "Nessun elemento selezionato.");
}

static void cmd_make_reference(void)
{
    if (active_tab != TAB_ALL || selected_mod < 0)
    { dobpopup_Info("Collegamento", "Seleziona un programma da \"Tutti i programmi\"."); return; }
    module_entry_t *m = &modules[selected_mod];

    char ref_path[200];
    snprintf(ref_path, sizeof(ref_path), "/DATA/Desktop/%s.reference", m->name);
    int fd = dobfs_Open(ref_path, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0) { dobpopup_Error("Collegamento", "Impossibile creare il collegamento."); return; }
    char body[260];
    snprintf(body, sizeof(body), "# MainDOB shortcut — target path follows\n%s\n", m->mdl_path);
    dobfs_Write(fd, body, (uint32_t)strlen(body));
    dobfs_Close(fd);

    char self_prog[160];
    if (dobconfig_GetAssoc("reference", self_prog, sizeof(self_prog)) != 0)
        dobconfig_SetAssoc("reference", "/SYSTEM/PROGRAMS/modules/modules.mdl");

    dobpopup_Info("Collegamento", "Collegamento creato sul Desktop.");
}

/* Hand the selected installed module off to DobInstaller for removal.
 * Only modules that came from a package (has_manifest) are uninstallable;
 * a system module scanned without a manifest is part of the base system
 * and has no ModuleFiles receipt. The bubble directory is dirname of
 * mdl_path — parse_manifest always builds mdl_path as <bubble>/<x>.mdl,
 * so its parent is exactly the folder DobInstaller expects via
 * --uninstall. The confirmation prompt and all error messaging live in
 * DobInstaller, keeping the UX symmetric with install. Fire-and-forget:
 * the list may be stale until the next Aggiorna. */
static void cmd_uninstall(void)
{
    if (active_tab != TAB_ALL || selected_mod < 0)
    { dobpopup_Info("Disinstalla", "Seleziona un programma da \"Tutti i programmi\"."); return; }

    module_entry_t *m = &modules[selected_mod];
    if (!m->has_manifest || m->mdl_path[0] == '\0')
    { dobpopup_Info("Disinstalla", "Questo modulo di sistema non è disinstallabile."); return; }

    /* bubble_dir = dirname(mdl_path) */
    char bubble_dir[160];
    strncpy(bubble_dir, m->mdl_path, sizeof(bubble_dir) - 1);
    bubble_dir[sizeof(bubble_dir) - 1] = '\0';
    char *last_slash = NULL;
    for (char *p = bubble_dir; *p; p++) if (*p == '/') last_slash = p;
    if (!last_slash || last_slash == bubble_dir)
    { dobpopup_Error("Disinstalla", "Percorso del modulo non valido."); return; }
    *last_slash = '\0';

    /* DobInstaller runs as a driver (via dobinterface) so it can rewrite
     * Startup_modules and associations; it reads <bubble>/ModuleFiles to
     * reverse the install and shows its own confirmation. */
    const char *argv[] = { "--uninstall", bubble_dir, NULL };
    if (dobui_SpawnDriver("/SYSTEM/PROGRAMS/DobInstaller/DobInstaller.mdl",
                          argv) < 0)
        dobpopup_Error("Disinstalla", "Impossibile avviare DobInstaller.");
}

static void cmd_toggle_drivers(void)
{
    show_drivers = !show_drivers;
    selected_mod = -1; status_name[0] = '\0';
    rebuild_panel();
    redraw();
}

static void cmd_refresh(void)
{
    load_modules(); library_load();
    selected_mod = -1; lib_sel_item = lib_sel_folder = -1; status_name[0]='\0';
    rebuild_panel(); redraw();
}

void event_panel(int cmd_idx)
{
    if (cmd_idx < 0 || cmd_idx >= panel_action_count) return;
    switch (panel_actions[cmd_idx])
    {
        case ACT_OPEN:        cmd_open();                break;
        case ACT_ADD_LIB:     cmd_add_to_library();      break;
        case ACT_NEW_FOLDER:  cmd_new_folder();          break;
        case ACT_RENAME:      cmd_rename_folder();       break;
        case ACT_REMOVE:      cmd_remove_from_library(); break;
        case ACT_MAKE_REF:    cmd_make_reference();      break;
        case ACT_UNINSTALL:   cmd_uninstall();           break;
        case ACT_TOGGLE_DRV:  cmd_toggle_drivers();      break;
        case ACT_REFRESH:     cmd_refresh();             break;
    }
}

void event_rightclick(int x, int y, uint8_t buttons)
{
    (void)buttons;
    if (overlay_open) return;
    if (y < TAB_H) return;

    if (active_tab == TAB_ALL)
    { int mi = all_hit(x, y); selected_mod = mi; set_status_module(mi); redraw(); return; }

    lib_hit_t h = lib_hit(x, y);
    lib_sel_item = lib_sel_folder = -1;
    if (h.kind == LH_FOLDER) { lib_sel_folder = h.idx;
        strncpy(status_name, lib_folders[h.idx].name, sizeof(status_name)-1); status_name[sizeof(status_name)-1]='\0'; }
    else if (h.kind == LH_ITEM) { lib_sel_item = h.idx; set_status_path(lib_items[h.idx].path); }
    else status_name[0] = '\0';
    redraw();
}

/* ===================================================================
 * Startup
 * =================================================================== */

void event_start(void)
{
    g_win = dobui_window();
    g_w = dobui_width();
    g_h = dobui_height();

    build_gears();
    load_modules();
    library_load();

    /* Search textbox: themed to the navy/yellow palette. Position is
     * refreshed each redraw (it tracks window width). */
    dobtb_Init(&search_tb, g_win, 8 + 7 * 8, TAB_H + 2, WIN_W - (8 + 7*8) - 12, SEARCH_H - 4);
    search_tb.col_bg           = COL_NAVY_DK;
    search_tb.col_text         = COL_YELLOW;
    search_tb.col_cursor       = COL_YELLOW;
    search_tb.col_border       = COL_SEP_LINE;
    search_tb.col_border_focus = COL_YELLOW;
    search_tb.col_sel_bg       = COL_TAB_ACT;
    search_tb.col_sel_text     = COL_YELLOW;
    search_tb_ready = true;

    selected_mod = -1; lib_sel_item = -1; lib_sel_folder = -1;
    status_name[0] = '\0';

    rebuild_panel();
    redraw();
}

int main(void)
{
    /* Panel is built dynamically in event_start/rebuild_panel; set a
     * minimal placeholder so the framework has something pre-run. */
    dobui_set_panel("Aggiorna");
    dobui_run("Gestione Moduli", WIN_W, WIN_H);
    return 0;
}
