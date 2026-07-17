/* MainDOB Orologio -- tray clock + calendar, and (later) agenda app.
 *
 * Single .mdl, two launch modes selected by argv[0]:
 *
 *   (no args)   TRAY mode  -- a widget-only program living in the panel
 *                            tray. Shows HH:MM, the full date, and a
 *                            month-grid calendar. Always present from
 *                            boot (listed in Startup_modules). Hosts the
 *                            "open agenda" launcher (and, later, the
 *                            alarm).
 *   "--app"     APP mode   -- an on-demand window with the agenda UI:
 *                            two tabs (Calendario / Eventi). Step 2
 *                            builds the window, the tab bar, and the
 *                            Calendario view (navigable month grid).
 *                            The Eventi list and persistence come in
 *                            step 3; the alarm + tray<->app IPC in
 *                            step 4.
 *
 * Time source: gettime() -- the RTC-calibrated kernel wall clock. The
 * returned date is already civil-correct (leap years handled in the
 * kernel's civil.c), so the calendar needs only weekday math, done
 * here in userspace. No hardware access: a widget is an ordinary
 * sandboxed program.
 *
 * The calendar math (leap year, days-in-month, weekday, grid layout)
 * is shared between the tray widget and the app window: one set of
 * helpers, two render targets.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <DobInterface.h>
#include <DobPopup.h>
#include <DobFileSystem.h>
#include <DobSettings.h>
#include <dob/ipc.h>
#include <dob/registry.h>
#include <dob/types.h>
#include <dob/spawn.h>
#include <listview.h>

/* === GUI event codes (from dobinterface's protocol) === */
#define GUI_EVT_MOUSE           201      /* arg1=xy(packed) arg2=btn arg3=etype */
#define GUI_EVT_KEY             202      /* arg1=key arg2=key arg3=0 */
#define GUI_EVT_CLOSE_REQ       210
#define GUI_EVT_RESIZE          212
#define GUI_EVT_WIDGET_CLICK    220      /* arg1=relx arg2=rely arg3=etype */

/* Mouse event types (arg3 of GUI_EVT_MOUSE) */
#define ETYPE_PRESS             1
#define ETYPE_RELEASE           2

/* Path to our own .mdl, for relaunching in --app mode. */
#define CLOCK_MDL  "/SYSTEM/PROGRAMS/clock/clock.mdl"

/* =====================================================================
 * Agenda event model + persistence
 * =====================================================================
 *
 * Events live in a plain-text file in the program's OWN sandboxed
 * folder. MainDOB confines every program to its home dir
 * (/SYSTEM/PROGRAMS/clock/), and DobFileSystem allows a program to
 * write only there (plus /DATA/). Keeping the file in our home means
 * NO other program can tamper with it -- /DATA/ is world-writable by
 * design, our home is not. The tray applet and the spawned --app
 * window share this identity (spawn_file derives the child's home from
 * the .mdl basename), so both read and write the same file.
 *
 * Line format, one event per line:
 *
 *     YYYY-MM-DD HH:MM|title text
 *     YYYY-MM-DD HH:MM|title text|F
 *
 * The pipe separates the timestamp from the free-form title. An OPTIONAL
 * third field F (0 or 1) records whether the alarm already fired, so a
 * reboot doesn't replay reminders whose time has passed. Lines without
 * it (older files, hand-written entries) default fired=0 -- fully
 * backward compatible. Parsing is line-by-line and tolerant: a malformed
 * line is skipped, never fatal -- the same robustness the .kbl parser
 * uses. The title itself can't contain '|' (the add/edit flow rewrites
 * any '|' the user types to '/'), so splitting on the LAST '|' when a
 * trailing flag is present is unambiguous. */

#define AGENDA_FILE   "/SYSTEM/PROGRAMS/clock/agenda.dat"

/* Calendar helpers are defined further down (near the rendering code)
 * but apply_tz() above the file's midpoint needs them, so forward-
 * declare here. */
static bool is_leap_year(uint32_t y);
static int  days_in_month(int year, int month);
static int  weekday_mon0(int y, int m, int d);
#define MAX_EVENTS    128
#define TITLE_MAX     96
#define FBUF_CAP      16384

typedef struct
{
    int  year, month, day;     /* date */
    int  hour, minute;         /* time-of-day */
    bool fired;                 /* alarm already shown (tray, step 4) */
    char title[TITLE_MAX];
} event_t;

static event_t events[MAX_EVENTS];
static int      event_count = 0;

/* A sortable key: minutes since an arbitrary epoch (year*~).
 * Monotonic in chronological order, which is all we need for sorting
 * and for "has this moment passed?" comparisons. */
static long event_key(const event_t *e)
{
    /* ((year*12+month)*31+day)*1440 + hour*60 + minute.
     * Gaps (31/day, 12/month) are fine -- we only need ordering. */
    long d = (((long)e->year * 12 + e->month) * 31 + e->day);
    return d * 1440L + (long)e->hour * 60L + e->minute;
}

/* Insertion-sort events[] ascending by time. Small N, runs on every
 * load/add; simplicity over speed. */
static void events_sort(void)
{
    for (int i = 1; i < event_count; i++)
    {
        event_t key = events[i];
        long kk = event_key(&key);
        int j = i - 1;
        while (j >= 0 && event_key(&events[j]) > kk)
        {
            events[j + 1] = events[j];
            j--;
        }
        events[j + 1] = key;
    }
}

/* Parse one line "YYYY-MM-DD HH:MM|title" into *e. Returns true on a
 * well-formed line. Tolerant: rejects (skips) anything malformed. */
static bool parse_event_line(const char *line, event_t *e)
{
    /* Need at least "YYYY-MM-DD HH:MM|" = 17 chars before the title. */
    int y, mo, d, h, mi;
    /* Manual field scan -- the libc here has sscanf? Be conservative and
     * parse by hand to avoid depending on it. */
    const char *p = line;

    /* Skip leading spaces. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0') return false;

    /* YYYY */
    y = 0; int n = 0;
    while (*p >= '0' && *p <= '9') { y = y * 10 + (*p - '0'); p++; n++; }
    if (n != 4 || *p != '-') return false;
    p++;
    /* MM */
    mo = 0; n = 0;
    while (*p >= '0' && *p <= '9') { mo = mo * 10 + (*p - '0'); p++; n++; }
    if (n < 1 || n > 2 || *p != '-') return false;
    p++;
    /* DD */
    d = 0; n = 0;
    while (*p >= '0' && *p <= '9') { d = d * 10 + (*p - '0'); p++; n++; }
    if (n < 1 || n > 2 || *p != ' ') return false;
    p++;
    /* HH */
    h = 0; n = 0;
    while (*p >= '0' && *p <= '9') { h = h * 10 + (*p - '0'); p++; n++; }
    if (n < 1 || n > 2 || *p != ':') return false;
    p++;
    /* MM */
    mi = 0; n = 0;
    while (*p >= '0' && *p <= '9') { mi = mi * 10 + (*p - '0'); p++; n++; }
    if (n < 1 || n > 2 || *p != '|') return false;
    p++;

    /* Range sanity. */
    if (mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59)
        return false;

    e->year = y; e->month = mo; e->day = d;
    e->hour = h; e->minute = mi;
    e->fired = false;

    /* Title = rest of line, trimmed of trailing CR/LF. */
    int i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && i < TITLE_MAX - 1)
    {
        e->title[i] = p[i];
        i++;
    }
    e->title[i] = '\0';

    /* Optional trailing "|0" / "|1" fired flag. The title can't contain
     * '|' (sanitised on input), so a '|' two chars from the end with a
     * 0/1 after it is unambiguously the flag, not part of the title. */
    if (i >= 2 && e->title[i - 2] == '|' &&
        (e->title[i - 1] == '0' || e->title[i - 1] == '1'))
    {
        e->fired = (e->title[i - 1] == '1');
        e->title[i - 2] = '\0';   /* strip the flag from the title */
    }

    if (e->title[0] == '\0') return false;   /* empty title -> skip */
    return true;
}

/* Load events from AGENDA_FILE into events[]. Missing file = zero
 * events (not an error -- first run). Result is sorted. */
static void events_load(void)
{
    event_count = 0;

    int fd = dobfs_Open(AGENDA_FILE, FS_READ);
    if (fd < 0)
        return;   /* no file yet */

    static char buf[FBUF_CAP];
    int total = 0, got;
    while (total < (int)sizeof(buf) - 1 &&
           (got = dobfs_Read(fd, buf + total,
                             (uint32_t)((int)sizeof(buf) - 1 - total))) > 0)
        total += got;
    dobfs_Close(fd);
    buf[total] = '\0';

    const char *p = buf;
    while (*p && event_count < MAX_EVENTS)
    {
        char line[160];
        int li = 0;
        while (*p && *p != '\n' && li < (int)sizeof(line) - 1)
            line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;

        event_t e;
        memset(&e, 0, sizeof(e));
        if (parse_event_line(line, &e))
            events[event_count++] = e;
    }

    events_sort();
}

/* Write events[] back to AGENDA_FILE (truncate + rewrite). Returns true
 * on success. Same write pattern the editor uses. */
static bool events_save(void)
{
    int fd = dobfs_Open(AGENDA_FILE, FS_WRITE | FS_CREATE | FS_TRUNC);
    if (fd < 0)
        return false;

    bool ok = true;
    for (int i = 0; i < event_count; i++)
    {
        char line[160];
        int len = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d|%s|%d\n",
                           events[i].year, events[i].month, events[i].day,
                           events[i].hour, events[i].minute, events[i].title,
                           events[i].fired ? 1 : 0);
        if (len <= 0) continue;
        int off = 0;
        while (off < len)
        {
            int w = dobfs_Write(fd, line + off, (uint32_t)(len - off));
            if (w <= 0) { ok = false; break; }
            off += w;
        }
        if (!ok) break;
    }
    dobfs_Close(fd);
    return ok;
}

/* Add an event to the in-memory list and persist. Returns false if the
 * list is full or the write failed. */
static bool events_add(int y, int mo, int d, int h, int mi, const char *title)
{
    if (event_count >= MAX_EVENTS)
        return false;
    event_t *e = &events[event_count];
    e->year = y; e->month = mo; e->day = d;
    e->hour = h; e->minute = mi;
    e->fired = false;
    strncpy(e->title, title, TITLE_MAX - 1);
    e->title[TITLE_MAX - 1] = '\0';
    event_count++;
    events_sort();
    return events_save();
}

/* Remove event at index i, persist. */
static bool events_remove(int idx)
{
    if (idx < 0 || idx >= event_count)
        return false;
    for (int i = idx; i < event_count - 1; i++)
        events[i] = events[i + 1];
    event_count--;
    return events_save();
}

/* Remove every fired (past) event in one pass, then persist. Returns
 * the number removed. Backing for the "Cancella avvenuti" button. */
static int events_clear_fired(void)
{
    int dst = 0;
    int removed = 0;
    for (int i = 0; i < event_count; i++)
    {
        if (events[i].fired)
        {
            removed++;
        }
        else
        {
            if (dst != i) events[dst] = events[i];
            dst++;
        }
    }
    event_count = dst;
    if (removed > 0)
        events_save();
    return removed;
}

/* True when the OS booted from the live CD: the filesystem is a
 * read-only RAM image, so events can't persist. The agenda warns about
 * this rather than silently failing to save. */
static bool is_live_mode(void)
{
    return syscall0(SYS_LIVE_QUERY) > 0;
}

/* === Timezone / DST ===
 *
 * The RTC (and thus gettime()) is UTC. To show local time we apply an
 * offset chosen by the user. Per MainDOB's settings model the values
 * live in settingsd: this program DECLARES them (with defaults) and
 * READS them; the user changes them in the DobSettings editor. We never
 * write them ourselves -- that's structurally the daemon's job.
 *
 * Two settings:
 *   clock.utc_offset : ENUM "UTC-12".."UTC+14" (whole-hour zones)
 *   clock.dst        : BOOL, adds +1h when on (manual -- the user ticks
 *                      it for summer time; computing EU/US DST dates in
 *                      the kernel-less userspace isn't worth the rules).
 *
 * Both the resident tray clock AND the on-demand app read these and
 * call apply_tz() right after gettime(), so the clock face, the
 * calendar's "today", and the alarm comparisons all use local time
 * consistently across the two processes. */

/* UTC-12 .. UTC+14, index 0..26 -> offset (idx - 12) hours. */
static const char *const TZ_OPTIONS[] = {
    "UTC-12","UTC-11","UTC-10","UTC-9","UTC-8","UTC-7","UTC-6","UTC-5",
    "UTC-4","UTC-3","UTC-2","UTC-1","UTC+0","UTC+1","UTC+2","UTC+3",
    "UTC+4","UTC+5","UTC+6","UTC+7","UTC+8","UTC+9","UTC+10","UTC+11",
    "UTC+12","UTC+13","UTC+14", 0
};
#define TZ_DEFAULT  "UTC+1"     /* sensible for the author's locale */

/* Declare the timezone settings (idempotent; safe every boot). Called
 * by both modes so the settings exist regardless of which ran first. */
static void declare_clock_settings(void)
{
    declareSetting("clock.utc_offset", SETTING_ENUM,
                   "Fuso orario", TZ_DEFAULT, TZ_OPTIONS);
    declareSetting("clock.dst", SETTING_BOOL,
                   "Ora legale (+1h)", "false", 0);
}

/* Current total offset in hours = base UTC offset + (DST ? 1 : 0).
 * The timezone offset changes only when the user edits it in DobSettings
 * and presses "Applica", at which point settingsd posts SETTINGS_RELOAD_CALL
 * to us. So we read it from settingsd exactly once at startup and again on
 * each reload notification, caching the result in g_tz_offset_hours. The
 * per-second tick then reads the cached value with no IPC and no disk
 * access — consistent with the rest of MainDOB, which is event-driven and
 * never polls. (Reading live on every call previously woke settingsd, and
 * through it the disk, once a second.) */
static int  g_tz_offset_hours = 1;   /* cached; refreshed on reload. +1 = TZ_DEFAULT */

/* Read the live timezone offset from settingsd. Called only from
 * tz_refresh_offset() — never from the tick path. */
static int tz_read_offset_from_settings(void)
{
    int off = 1;   /* fallback = TZ_DEFAULT (+1) if unreadable */
    const char *v = getSetting("clock.utc_offset");
    if (v)
    {
        /* Parse "UTC{+|-}N". Find the sign, then the number. */
        const char *p = v;
        while (*p && *p != '+' && *p != '-') p++;
        if (*p)
        {
            int sign = (*p == '-') ? -1 : 1;
            p++;
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            off = sign * n;
        }
    }
    char dbuf[8];
    if (settingField("clock.dst", 0, dbuf, sizeof(dbuf)) == 0)
    {
        if (strcmp(dbuf, "true") == 0)
            off += 1;
    }
    return off;
}

/* Refresh the cached offset from settingsd. Call at startup and whenever
 * settingsd notifies us (SETTINGS_RELOAD_CALL) that the user applied a
 * change. */
static void tz_refresh_offset(void)
{
    g_tz_offset_hours = tz_read_offset_from_settings();
}

/* Cached offset used by the per-second tick — no IPC, no disk. */
static int tz_total_offset_hours(void)
{
    return g_tz_offset_hours;
}

/* Apply the timezone offset to a gettime() result in place. Handles the
 * day rollover in both directions, advancing/retreating day, month and
 * year correctly (reusing days_in_month/is_leap_year). Whole-hour
 * offsets only, so minutes/seconds are untouched. */
static void apply_tz(uint32_t *t)
{
    int off = tz_total_offset_hours();
    if (off == 0) return;

    int year  = (int)t[0];
    int month = (int)t[1];
    int day   = (int)t[2];
    int hour  = (int)t[3] + off;

    /* Roll forward across midnight(s). */
    while (hour >= 24)
    {
        hour -= 24;
        day++;
        if (day > days_in_month(year, month))
        {
            day = 1;
            month++;
            if (month > 12) { month = 1; year++; }
        }
    }
    /* Roll backward across midnight(s). */
    while (hour < 0)
    {
        hour += 24;
        day--;
        if (day < 1)
        {
            month--;
            if (month < 1) { month = 12; year--; }
            day = days_in_month(year, month);
        }
    }

    t[0] = (uint32_t)year;
    t[1] = (uint32_t)month;
    t[2] = (uint32_t)day;
    t[3] = (uint32_t)hour;
}

/* gettime() + local-time correction in one call. Everywhere the clock
 * needs the wall time it uses this, never raw gettime(), so UTC never
 * leaks to the display or the alarm. Returns 0 on success like gettime. */
static int localtime_now(uint32_t *t)
{
    int r = gettime(t);
    if (r == 0)
        apply_tz(t);
    return r;
}

/* === Async timer fire code (see kernel SYS_TIMER_SET / unistd.h) === */
#define MSG_TIMER               70

/* App -> tray signal: "events file changed, reload it". Sent by the
 * --app window after it saves, received in the tray's normal receive
 * loop (same channel as timer/GUI events -- a plain async post, not a
 * notify, so the single-receive loop stays simple). The code sits well
 * clear of the GUI event range (200s) and the timer code (70). */
#define MSG_AGENDA_RELOAD       300

/* === Font metrics (shared 8x16 bitmap font) === */
#define FONT_W                  8
#define FONT_H                  16

/* === Widget geometry ===
 * Width matches the other tray panels (keymap is 210). Height sized to
 * fit: title + big time + date (two lines) + separator + a 7-column,
 * 6-row month grid with weekday headers. */
#define W                       210
#define H                       320
#define PAD                     12

/* Vertical layout cursors (y of each block's top) */
#define TITLE_Y                 12
#define TIME_Y                  36      /* big HH:MM */
#define DATE_Y                  (TIME_Y + 28)      /* "Dom 31 maggio" */
#define YEAR_Y                  (DATE_Y + FONT_H + 2)
#define SEP_Y                   (YEAR_Y + FONT_H + 8)
#define CAL_HDR_Y               (SEP_Y + 10)       /* weekday headers row */
#define CAL_GRID_Y              (CAL_HDR_Y + FONT_H + 4)

/* Calendar cell grid: 7 columns across the usable width. */
#include <dobui_theme.h>
#define CAL_COLS                7
#define CAL_ROWS                6
#define CAL_X                   PAD
#define CAL_W                   (W - 2 * PAD)          /* 186 px */
#define CELL_W                  (CAL_W / CAL_COLS)     /* 26 px  */
#define CELL_H                  20

/* "Apri agenda" launcher button, at the bottom of the tray panel.
 * Sits below the 6-row grid (grid bottom ~= CAL_GRID_Y + 6*CELL_H). */
#define BTN_H                   24
#define BTN_Y                   (H - BTN_H - 8)
#define BTN_X                   PAD
#define BTN_W                   (W - 2 * PAD)

/* === Colors (match the dark tray panel look) === */
#define COL_BG          0x00242430      /* panel background  */
#define COL_TIME        0x00FFFFFF      /* big clock         */
#define COL_DATE        0x00C8C8D2      /* date line         */
#define COL_DIM         0x00888899      /* weekday headers   */
#define COL_SEP         0x00444452      /* separator line    */
#define COL_DAY         0x00C8C8D2      /* normal day number */
#define COL_TODAY_BG    0x000066CC      /* today's cell fill */
#define COL_TODAY_FG    0x00FFFFFF      /* today's number    */
#define COL_WEEKEND     0x009098A8      /* Sat/Sun numbers   */
#define COL_BTN_BG      0x00384058      /* launcher button   */
#define COL_BTN_FG      0x00DCE4F0
#define COL_BTN_BORDER  0x00586078

/* =====================================================================
 * APP mode (--app): agenda window
 * ===================================================================== */

/* Window geometry. Wider than the tray panel: the calendar grid here
 * uses larger cells, and the Eventi tab (step 3) needs room for a list. */
#define APP_W           360
#define APP_H           420
#define APP_PAD         12

/* Tab bar at the top: two tabs, full width split in two. */
#define TAB_H           28
#define TAB_Y           0
#define TAB_W           (APP_W / 2)

/* Calendario tab layout */
#define ANAV_Y          (TAB_H + 14)            /* month-nav row (◄ Maggio 2026 ►) */
#define ANAV_BTN_W      28                      /* ◄ / ► hit width */
#define ACAL_HDR_Y      (ANAV_Y + FONT_H + 14)  /* weekday headers */
#define ACAL_X          APP_PAD
#define ACAL_W          (APP_W - 2 * APP_PAD)        /* 336 px */
#define ACELL_W         (ACAL_W / CAL_COLS)          /* 48 px  */
#define ACELL_H         44
#define ANEW_H          28                           /* "+ Nuovo evento" button */
#define ANEW_Y          (APP_H - ANEW_H - APP_PAD)
#define ANEW_X          APP_PAD
#define ANEW_W          (APP_W - 2 * APP_PAD)

/* Hour picker: 24 cells as 6 columns x 4 rows. */
#define HR_COLS         6
#define HR_ROWS         4
#define HR_CELL_W       (ACAL_W / HR_COLS)           /* 56 px */
#define HR_CELL_H       48

/* Minute picker: 60 cells as 10 columns x 6 rows. */
#define MIN_COLS        10
#define MIN_ROWS        6
#define MIN_CELL_W      (ACAL_W / MIN_COLS)          /* 33 px */
#define MIN_CELL_H      32

/* "‹ Indietro" back affordance, shown while picking hour/minute, in the
 * same row the month title uses on the day view. */
#define BACK_X          ACAL_X
#define BACK_Y          ANAV_Y
#define BACK_W          72

/* App colors: a lighter, "document" look distinct from the dark tray. */
#define ACOL_BG         DOBUI_INSET      /* window background     */
#define ACOL_TAB_ACT    DOBUI_RELIEF      /* active tab fill       */
#define ACOL_TAB_IN     DOBUI_SURFACE      /* inactive tab fill     */
#define ACOL_TAB_FG     DOBUI_TEXT_ALT      /* tab label             */
#define ACOL_TAB_FG_IN  DOBUI_DISABLED      /* inactive tab label    */
#define ACOL_TAB_LINE   DOBUI_RELIEF      /* tab bottom border     */
#define ACOL_TEXT       DOBUI_TEXT_ALT      /* primary text          */
#define ACOL_DIM        DOBUI_DISABLED      /* weekday headers, dim  */
#define ACOL_NAV        DOBUI_TEXT      /* ◄ ► arrows + month     */
#define ACOL_DAY        DOBUI_TEXT_ALT      /* day numbers           */
#define ACOL_WEEKEND    DOBUI_DISABLED      /* Sat/Sun day numbers   */
#define ACOL_TODAY_BG   DOBUI_RELIEF      /* today's cell fill     */
#define ACOL_TODAY_FG   DOBUI_TEXT_ALT
#define ACOL_CELL_LINE  DOBUI_DISABLED      /* faint cell separators */

/* Tab identifiers */
#define TAB_CALENDAR    0
#define TAB_EVENTS      1

/* Eventi tab layout: a list filling most of the content area, with a
 * Modifica / Elimina button row underneath. */
#define EV_LIST_X       APP_PAD
#define EV_LIST_Y       (TAB_H + 12)
#define EV_LIST_W       (APP_W - 2 * APP_PAD)
#define EV_BTN_H        28
/* Bottom row: Modifica | Elimina. */
#define EV_BTN_Y        (APP_H - EV_BTN_H - APP_PAD)
/* Row above it: full-width "Cancella avvenuti". */
#define EV_CLR_H        28
#define EV_CLR_Y        (EV_BTN_Y - EV_CLR_H - 8)
#define EV_CLR_X        APP_PAD
#define EV_CLR_W        (APP_W - 2 * APP_PAD)
/* List shrinks to sit above the clear-row. */
#define EV_LIST_H       (EV_CLR_Y - EV_LIST_Y - 10)
#define EV_BTN_GAP      10
#define EV_BTN_W        ((EV_LIST_W - EV_BTN_GAP) / 2)
#define EV_BTN1_X       APP_PAD                       /* Modifica */
#define EV_BTN2_X       (EV_BTN1_X + EV_BTN_W + EV_BTN_GAP)  /* Elimina */

/* Eventi button colors (on the light app background) */
#define ECOL_EBTN_BG    DOBUI_INSET
#define ECOL_EBTN_FG    DOBUI_TEXT_ALT
#define ECOL_EBTN_BORDER DOBUI_TEXT_ALT
#define ECOL_DBTN_BG    DOBUI_INSET      /* Elimina: reddish */
#define ECOL_DBTN_FG    DOBUI_DANGER
#define ECOL_DBTN_BORDER DOBUI_DANGER
#define ECOL_CLR_BG     DOBUI_INSET      /* Cancella avvenuti: muted amber */
#define ECOL_CLR_FG     DOBUI_INPUT
#define ECOL_CLR_BORDER DOBUI_INPUT

/* === State === */
static uint32_t event_port;
static uint32_t widget_id;
static dobui_widget_fb_ctx_t wctx;

/* Last rendered minute fingerprint -- redraw only when it changes, so a
 * 1 Hz tick doesn't repaint 86400 times/day for an HH:MM display. -1
 * forces the first paint. */
static int last_fp = -1;

/* === Italian month / weekday names === */
static const char *const MONTHS[12] = {
    "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
    "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"
};

/* Short weekday names, Monday-first (European convention). */
static const char *const WD_SHORT[7] = {
    "Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom"
};
/* Full weekday names, Monday-first. */
static const char *const WD_FULL[7] = {
    "Lunedi", "Martedi", "Mercoledi", "Giovedi",
    "Venerdi", "Sabato", "Domenica"
};

/* === Calendar arithmetic === */

/* Gregorian leap-year predicate -- identical to the kernel's civil.c. */
static bool is_leap_year(uint32_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

/* Number of days in (year, month). month is 1..12. */
static int days_in_month(int year, int month)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 30;
    if (month == 2 && is_leap_year((uint32_t)year))
        return 29;
    return dim[month - 1];
}

/* Day of week for a Gregorian date, returned Monday-first: 0=Mon .. 6=Sun.
 *
 * Sakamoto's algorithm. It yields 0=Sunday..6=Saturday; we rotate to
 * Monday-first at the end. Valid for the full Gregorian range, well
 * beyond anything the RTC can produce. */
static int weekday_mon0(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int sun0 = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    /* sun0: 0=Sun..6=Sat  ->  mon0: 0=Mon..6=Sun */
    return (sun0 + 6) % 7;
}

/* === Drawing helpers === */

/* Draw text horizontally centered within [x, x+w) into target `id`
 * (a window or a widget surface). Monospace 8px font, so centering is
 * pure arithmetic -- no measurement needed. */
static void draw_centered(uint32_t id, int x, int w, int y, const char *s,
                          uint32_t fg, uint32_t bg)
{
    int tw = (int)strlen(s) * FONT_W;
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    dobui_DrawText(id, tx, y, s, fg, bg);
}

/* Render a month grid into target `id` at origin (ox, oy).
 * Shared by the tray widget and the app's Calendario tab.
 *   year, month : the month to draw (month 1..12)
 * highlight_day : day-of-month to highlight (today), or 0 for none
 *           bg  : background color to paint cells against
 *      cell_w/h : cell dimensions
 * Draws weekday headers (Lun..Dom) then up to 6 rows of day numbers. */
static void draw_month_grid(uint32_t id, int ox, int oy,
                            int year, int month, int highlight_day,
                            uint32_t bg, int cell_w, int cell_h)
{
    /* Weekday header row */
    for (int c = 0; c < CAL_COLS; c++)
        draw_centered(id, ox + c * cell_w, cell_w, oy,
                      WD_SHORT[c], COL_DIM, bg);

    int grid_y = oy + FONT_H + 4;
    int first_col = weekday_mon0(year, month, 1);
    int ndays = days_in_month(year, month);

    for (int dnum = 1; dnum <= ndays; dnum++)
    {
        int idx = first_col + (dnum - 1);
        int row = idx / CAL_COLS;
        int col = idx % CAL_COLS;
        if (row >= CAL_ROWS) break;

        int cx = ox + col * cell_w;
        int cy = grid_y + row * cell_h;
        int ty = cy + (cell_h - 2 - FONT_H) / 2;

        char dbuf[4];
        sprintf(dbuf, "%d", dnum);

        if (dnum == highlight_day)
        {
            dobui_FillRect(id, cx + 1, cy, cell_w - 2, cell_h - 2,
                           COL_TODAY_BG);
            draw_centered(id, cx, cell_w, ty, dbuf,
                          COL_TODAY_FG, COL_TODAY_BG);
        }
        else
        {
            uint32_t fg = (col >= 5) ? COL_WEEKEND : COL_DAY;
            draw_centered(id, cx, cell_w, ty, dbuf, fg, bg);
        }
    }
}

/* === Tray widget render === */

static void draw_widget(uint32_t *t)
{
    int year   = (int)t[0];
    int month  = (int)t[1];   /* 1..12 */
    int day    = (int)t[2];   /* 1..31 */
    int hour   = (int)t[3];
    int minute = (int)t[4];

    dobui_WidgetRestoreContext(&wctx);

    /* Background */
    dobui_FillRect(widget_id, 0, 0, W, H, COL_BG);

    /* Title */
    dobui_DrawText(widget_id, PAD, TITLE_Y, "Orologio", COL_DATE, COL_BG);

    /* HH:MM, centered and on its own line as the focal point. The shared
     * font has one fixed size (8x16), so "prominent" is placement, not a
     * larger glyph. */
    char hhmm[8];
    sprintf(hhmm, "%02d:%02d", hour, minute);
    draw_centered(widget_id, 0, W, TIME_Y, hhmm, COL_TIME, COL_BG);

    /* Date line: "Dom 31 maggio" then year on the next line. */
    int wd = weekday_mon0(year, month, day);
    char dateline[48];
    const char *mon = (month >= 1 && month <= 12) ? MONTHS[month - 1] : "?";
    sprintf(dateline, "%s %d %s", WD_SHORT[wd], day, mon);
    draw_centered(widget_id, 0, W, DATE_Y, dateline, COL_DATE, COL_BG);

    char yearline[8];
    sprintf(yearline, "%d", year);
    draw_centered(widget_id, 0, W, YEAR_Y, yearline, COL_DATE, COL_BG);

    /* Separator line */
    dobui_FillRect(widget_id, PAD, SEP_Y, W - 2 * PAD, 1, COL_SEP);

    /* Month grid (today highlighted). */
    draw_month_grid(widget_id, CAL_X, CAL_HDR_Y, year, month, day,
                    COL_BG, CELL_W, CELL_H);

    /* "Apri agenda" launcher button at the bottom. */
    dobui_FillRect(widget_id, BTN_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BG);
    dobui_DrawRect(widget_id, BTN_X, BTN_Y, BTN_W, BTN_H, COL_BTN_BORDER);
    draw_centered(widget_id, BTN_X, BTN_W, BTN_Y + (BTN_H - FONT_H) / 2,
                  "Apri agenda", COL_BTN_FG, COL_BTN_BG);

    dobui_WidgetInvalidate(widget_id);
}

/* Compute a fingerprint that changes once per displayed minute. Used to
 * skip redraws on the 59 ticks per minute where nothing visible moves.
 * Includes the day so a midnight rollover repaints the calendar. */
static int minute_fingerprint(uint32_t *t)
{
    /* (((year*12 + month)*31 + day)*24 + hour)*60 + minute, truncated
     * into an int -- collisions only across spans far longer than any
     * uptime, and a collision merely skips one harmless repaint. */
    int year = (int)t[0], month = (int)t[1], day = (int)t[2];
    int hour = (int)t[3], minute = (int)t[4];
    return ((((year * 12 + month) * 31 + day) * 24 + hour) * 60) + minute;
}

/* === Alarm (tray) ===
 *
 * No polling loop and no dedicated timer: the tray already wakes once a
 * second to refresh the clock display, so the alarm rides that same
 * tick. On a minute change we scan the (already time-sorted) event list
 * and fire any event whose moment has arrived and that hasn't fired yet.
 * Cost over the clock's own work: an integer compare per event. */

/* "now" as the same sortable key event_key() produces. */
static long now_key(uint32_t *t)
{
    long d = (((long)(int)t[0] * 12 + (int)t[1]) * 31 + (int)t[2]);
    return d * 1440L + (long)(int)t[3] * 60L + (int)t[4];
}

/* Fire any due, unfired events. Shows one popup per event. Persists the
 * fired flags so a reboot doesn't replay alarms whose time has passed.
 *
 * dobpopup_Info BLOCKS until dismissed -- while a popup is up the tray's
 * receive loop is paused and the repeating 1s timer simply queues ticks;
 * on dismissal one tick is consumed and the next gettime() resyncs the
 * clock. Multiple events due in the same scan are shown in order, so
 * none is lost behind the modal popup. An event is due when now >= its
 * moment, which also sweeps up anything missed while the box was open.
 *
 * Across a reboot: an event whose time has passed loads with fired=1
 * (persisted), so it does NOT replay. An event that came due while the
 * machine was OFF loads with fired=0 and fires once on the first scan
 * after boot -- a missed reminder resurfaces exactly once, then is
 * marked. */
static void alarm_check(uint32_t *now)
{
    long nk = now_key(now);
    bool any_fired = false;

    for (int i = 0; i < event_count; i++)
    {
        if (events[i].fired)
            continue;
        if (event_key(&events[i]) <= nk)
        {
            char body[TITLE_MAX + 32];
            snprintf(body, sizeof(body), "%02d:%02d\n%s",
                     events[i].hour, events[i].minute, events[i].title);
            dobpopup_Info("Promemoria", body);
            events[i].fired = true;
            any_fired = true;
        }
    }

    /* Persist the newly-set fired flags once, after the scan, so they
     * survive a reboot (the line format carries a trailing 0/1 field). */
    if (any_fired)
        events_save();
}

/* === TRAY mode === */

static int run_tray(void)
{
    event_port = (uint32_t)port_create();

    /* Single instance: the applet is launched from Startup_modules. */
    if (dob_registry_find("clock"))
        _exit(0);

    /* The widget tray lives in dobinterface. */
    dob_registry_wait("dobinterface", 5000);
    dob_registry_register("clock", event_port);

    widget_id = dobui_CreateWidget(W, H, event_port);
    if (widget_id == 0)
        _exit(1);
    dobui_WidgetSaveContext(&wctx);

    /* Load the agenda so the alarm can fire even if the app is never
     * opened this session. */
    events_load();

    /* Declare our timezone settings so they appear in DobSettings and
     * have defaults even before the user touches them. */
    declare_clock_settings();

    /* Read the timezone offset once, now. From here the tick uses the
     * cached value; we only re-read on a settingsd reload notification. */
    tz_refresh_offset();

    /* First paint. */
    uint32_t t[7];
    if (localtime_now(t) == 0)
    {
        last_fp = minute_fingerprint(t);
        draw_widget(t);
        /* Initial sweep: fire anything already due (e.g. reminders that
         * came due while the machine was off). */
        alarm_check(t);
    }

    /* Tick once a second; repaint only when the minute changes. */
    timer_set(event_port, 1000, 1);

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(event_port, &msg);

        switch (msg.code)
        {
            case MSG_TIMER:
            {
                uint32_t now[7];
                if (localtime_now(now) != 0)
                    break;
                int fp = minute_fingerprint(now);
                if (fp != last_fp)
                {
                    last_fp = fp;
                    draw_widget(now);
                    /* Same tick the clock already woke for: check alarms.
                     * Cost is an integer compare per event. */
                    alarm_check(now);
                }
                break;
            }

            case MSG_AGENDA_RELOAD:
                /* The --app window saved a change. Reload the file so the
                 * alarm sees new/edited/removed events immediately. A
                 * fresh load re-reads persisted fired flags, so nothing
                 * already-fired replays. Re-sweep right away in case the
                 * edit added something already due. */
                events_load();
                {
                    uint32_t now[7];
                    if (localtime_now(now) == 0)
                        alarm_check(now);
                }
                break;

            case SETTINGS_RELOAD_CALL:
                /* The user changed our settings in DobSettings and pressed
                 * "Applica"; settingsd posts this. Re-read the timezone
                 * offset once, here, and repaint so the new zone shows
                 * immediately. This is why the tick itself never needs to
                 * poll settingsd. */
                tz_refresh_offset();
                {
                    uint32_t now[7];
                    if (localtime_now(now) == 0)
                    {
                        last_fp = minute_fingerprint(now);
                        draw_widget(now);
                    }
                }
                break;

            case GUI_EVT_WIDGET_CLICK:
            {
                /* arg1=relx, arg2=rely, arg3=etype (1=click). Only act on
                 * a click landing on the "Apri agenda" button. */
                if (msg.arg3 != 0 && msg.arg3 != 1)
                    break;
                int rx = (int)msg.arg1;
                int ry = (int)msg.arg2;
                if (rx >= BTN_X && rx < BTN_X + BTN_W &&
                    ry >= BTN_Y && ry < BTN_Y + BTN_H)
                {
                    /* Relaunch ourselves in app mode. spawn_file is async
                     * (disk I/O on a worker), so the tray loop never
                     * stalls. A second click while the app is already
                     * open just spawns another window -- acceptable for
                     * now; step 4 can single-instance it if desired. */
                    const char *av[] = { "--app", NULL };
                    spawn_file(CLOCK_MDL, av);
                }
                break;
            }

            case GUI_EVT_CLOSE_REQ:
                /* The tray applet is resident; honour an explicit close
                 * by tearing down cleanly. */
                dobui_DestroyWidget(widget_id);
                _exit(0);
                break;

            default:
                break;
        }
    }

    return 0;
}

/* =====================================================================
 * APP mode implementation
 * ===================================================================== */

/* App window state */
static uint32_t app_port;
static uint32_t app_win;
static int      app_tab     = TAB_CALENDAR;   /* current tab */
static int      view_year   = 2026;           /* month currently shown */
static int      view_month  = 5;              /* 1..12 */
static int      today_year  = 2026;           /* "today" for highlight */
static int      today_month = 5;
static int      today_day   = 31;

/* Eventi-view widgets + display buffers. The listview references
 * strings it does not own, so we keep a stable backing store: one
 * formatted line per event ("31/05 21:30  Titolo"), plus a pointer
 * array handed to the control. Rebuilt whenever the event list
 * changes. */
static dob_listview_t ev_list;
static char  ev_lines[MAX_EVENTS][TITLE_MAX + 24];
static const char *ev_ptrs[MAX_EVENTS];
static bool  ev_list_ready = false;           /* listview initialised */

/* Calendar tab sub-mode: the grid morphs through three pickers to add
 * an event entirely by clicking -- pick a day, then the grid becomes
 * the 24 hours, then the 60 minutes; only the title is typed (a single
 * popup at the end). */
enum { CAL_DAYS = 0, CAL_HOURS, CAL_MINUTES };
static int cal_mode = CAL_DAYS;
static int pick_day  = 0;      /* chosen day-of-month (1..31) */
static int pick_hour = 0;      /* chosen hour (0..23) */

/* Decode a packed mouse xy (low 16 = x, high 16 = y), sign-correct. */
static void unpack_xy(uint32_t xy, int *x, int *y)
{
    int16_t lx = (int16_t)(xy & 0xFFFF);
    int16_t ly = (int16_t)((xy >> 16) & 0xFFFF);
    *x = lx;
    *y = ly;
}

/* Advance the shown month by delta (-1 / +1), wrapping the year. */
static void app_shift_month(int delta)
{
    view_month += delta;
    while (view_month < 1)  { view_month += 12; view_year--; }
    while (view_month > 12) { view_month -= 12; view_year++; }
}

/* --- Tab bar --- */
static void draw_tabs(void)
{
    /* Two tabs side by side. The active one is lighter and flush with
     * the content area; the bottom border under the inactive tab marks
     * the page boundary. */
    const char *labels[2] = { "Calendario", "Eventi" };
    for (int i = 0; i < 2; i++)
    {
        int tx = i * TAB_W;
        bool active = (app_tab == i);
        uint32_t bg = active ? ACOL_TAB_ACT : ACOL_TAB_IN;
        uint32_t fg = active ? ACOL_TAB_FG : ACOL_TAB_FG_IN;
        dobui_FillRect(app_win, tx, TAB_Y, TAB_W, TAB_H, bg);
        draw_centered(app_win, tx, TAB_W, TAB_Y + (TAB_H - FONT_H) / 2,
                      labels[i], fg, bg);
    }
    /* Bottom border across the whole bar. */
    dobui_FillRect(app_win, 0, TAB_Y + TAB_H - 1, APP_W, 1, ACOL_TAB_LINE);
}

/* --- Calendario tab: day view (default) --- */
static void draw_cal_days(void)
{
    /* Month-nav row: ◄  Maggio 2026  ► */
    const char *mon = (view_month >= 1 && view_month <= 12)
                      ? MONTHS[view_month - 1] : "?";
    char title[48];
    char moncap[16];
    strncpy(moncap, mon, sizeof(moncap) - 1);
    moncap[sizeof(moncap) - 1] = '\0';
    if (moncap[0] >= 'a' && moncap[0] <= 'z')
        moncap[0] = (char)(moncap[0] - 'a' + 'A');
    sprintf(title, "%s %d", moncap, view_year);

    dobui_DrawText(app_win, ACAL_X, ANAV_Y, "<", ACOL_NAV, ACOL_BG);
    dobui_DrawText(app_win, APP_W - ACAL_X - FONT_W, ANAV_Y, ">",
                   ACOL_NAV, ACOL_BG);
    draw_centered(app_win, 0, APP_W, ANAV_Y, title, ACOL_NAV, ACOL_BG);

    /* Weekday headers Lun..Dom. */
    for (int c = 0; c < CAL_COLS; c++)
        draw_centered(app_win, ACAL_X + c * ACELL_W, ACELL_W, ACAL_HDR_Y,
                      WD_SHORT[c], ACOL_DIM, ACOL_BG);

    int hl = (view_year == today_year && view_month == today_month)
             ? today_day : 0;
    int grid_y = ACAL_HDR_Y + FONT_H + 6;
    int first_col = weekday_mon0(view_year, view_month, 1);
    int ndays = days_in_month(view_year, view_month);

    for (int dnum = 1; dnum <= ndays; dnum++)
    {
        int idx = first_col + (dnum - 1);
        int row = idx / CAL_COLS;
        int col = idx % CAL_COLS;
        if (row >= CAL_ROWS) break;

        int cx = ACAL_X + col * ACELL_W;
        int cy = grid_y + row * ACELL_H;
        int ty = cy + (ACELL_H - FONT_H) / 2;

        dobui_DrawRect(app_win, cx, cy, ACELL_W, ACELL_H, ACOL_CELL_LINE);

        char dbuf[4];
        sprintf(dbuf, "%d", dnum);

        if (dnum == hl)
        {
            dobui_FillRect(app_win, cx + 1, cy + 1,
                           ACELL_W - 2, ACELL_H - 2, ACOL_TODAY_BG);
            draw_centered(app_win, cx, ACELL_W, ty, dbuf,
                          ACOL_TODAY_FG, ACOL_TODAY_BG);
        }
        else
        {
            uint32_t fg = (col >= 5) ? ACOL_WEEKEND : ACOL_DAY;
            draw_centered(app_win, cx, ACELL_W, ty, dbuf, fg, ACOL_BG);
        }
    }

    /* Hint where the "+ Nuovo evento" button used to be: the primary
     * way to add an event is now clicking a day. */
    draw_centered(app_win, ANEW_X, ANEW_W, ANEW_Y + (ANEW_H - FONT_H) / 2,
                  "Clicca un giorno per aggiungere un evento",
                  ACOL_DIM, ACOL_BG);
}

/* --- Calendario tab: hour picker --- */
static void draw_cal_hours(void)
{
    /* Header: back affordance + what we're picking for. */
    dobui_DrawText(app_win, BACK_X, BACK_Y, "< Indietro", ACOL_NAV, ACOL_BG);
    char hdr[48];
    sprintf(hdr, "%d %s %d - Ora",
            pick_day,
            (view_month >= 1 && view_month <= 12) ? MONTHS[view_month - 1] : "?",
            view_year);
    draw_centered(app_win, 0, APP_W, ACAL_HDR_Y, hdr, ACOL_TEXT, ACOL_BG);

    int grid_y = ACAL_HDR_Y + FONT_H + 10;
    for (int h = 0; h < 24; h++)
    {
        int row = h / HR_COLS;
        int col = h % HR_COLS;
        int cx = ACAL_X + col * HR_CELL_W;
        int cy = grid_y + row * HR_CELL_H;
        int ty = cy + (HR_CELL_H - FONT_H) / 2;

        dobui_DrawRect(app_win, cx, cy, HR_CELL_W, HR_CELL_H, ACOL_CELL_LINE);
        char b[4];
        sprintf(b, "%02d", h);
        draw_centered(app_win, cx, HR_CELL_W, ty, b, ACOL_DAY, ACOL_BG);
    }
}

/* --- Calendario tab: minute picker --- */
static void draw_cal_minutes(void)
{
    dobui_DrawText(app_win, BACK_X, BACK_Y, "< Indietro", ACOL_NAV, ACOL_BG);
    char hdr[48];
    sprintf(hdr, "%d %s %d  %02d:__ - Minuti",
            pick_day,
            (view_month >= 1 && view_month <= 12) ? MONTHS[view_month - 1] : "?",
            view_year, pick_hour);
    draw_centered(app_win, 0, APP_W, ACAL_HDR_Y, hdr, ACOL_TEXT, ACOL_BG);

    int grid_y = ACAL_HDR_Y + FONT_H + 10;
    for (int m = 0; m < 60; m++)
    {
        int row = m / MIN_COLS;
        int col = m % MIN_COLS;
        int cx = ACAL_X + col * MIN_CELL_W;
        int cy = grid_y + row * MIN_CELL_H;
        int ty = cy + (MIN_CELL_H - FONT_H) / 2;

        dobui_DrawRect(app_win, cx, cy, MIN_CELL_W, MIN_CELL_H, ACOL_CELL_LINE);
        char b[4];
        sprintf(b, "%02d", m);
        /* Multiples of 5 a touch bolder via the normal day color;
         * others dimmed, so the grid is scannable. */
        uint32_t fg = (m % 5 == 0) ? ACOL_TEXT : ACOL_DIM;
        draw_centered(app_win, cx, MIN_CELL_W, ty, b, fg, ACOL_BG);
    }
}

/* --- Calendario tab dispatcher --- */
static void draw_tab_calendar(void)
{
    /* Clear the content area below the tab bar. */
    dobui_FillRect(app_win, 0, TAB_H, APP_W, APP_H - TAB_H, ACOL_BG);

    if (cal_mode == CAL_DAYS)
        draw_cal_days();
    else if (cal_mode == CAL_HOURS)
        draw_cal_hours();
    else
        draw_cal_minutes();
}

/* Tell the resident tray applet to reload the agenda file. Called by the
 * --app window after any change it persists, so the alarm picks up
 * new/edited/removed events without waiting for the user to reboot or
 * the tray to re-read on its own. Fire-and-forget: if the tray isn't
 * found (shouldn't happen -- it's resident) we simply skip; the next
 * tray restart reloads from disk anyway. */
static void notify_tray_reload(void)
{
    uint32_t tray = dob_registry_find("clock");
    if (!tray)
        return;
    dob_msg_t m;
    memset(&m, 0, sizeof(m));
    m.code = MSG_AGENDA_RELOAD;
    dob_ipc_post(tray, &m);
}

/* Final step of the click-picker: ask only the title (the one thing
 * that needs a keyboard), then store the event at the already-picked
 * day/hour/minute in the currently shown month/year. Returns true if an
 * event was added. */
static bool add_event_with_title(int minute)
{
    char titlebuf[TITLE_MAX];
    titlebuf[0] = '\0';
    char in[TITLE_MAX];
    if (dobpopup_InputBox("Nuovo evento", "Titolo:",
                          titlebuf, in, sizeof(in)) != 0)
        return false;   /* cancelled */
    for (char *q = in; *q; q++) if (*q == '|') *q = '/';
    if (in[0] == '\0')
    {
        dobpopup_Error("Titolo", "Il titolo non puo' essere vuoto.");
        return false;
    }
    return events_add(view_year, view_month, pick_day, pick_hour, minute, in);
}

/* Rebuild the display lines for the Eventi list from events[].
 * Fired (past) events get a "[fatto]" prefix so the user can tell at a
 * glance what's already happened from what's upcoming. The listview
 * control supports only a single text color, so the marker lives in the
 * string itself rather than per-row styling -- and crucially this keeps
 * the displayed order identical to events[], so the listview's selected
 * index maps straight onto events[] for Modifica/Elimina. */
static void ev_rebuild_lines(void)
{
    for (int i = 0; i < event_count; i++)
    {
        if (events[i].fired)
            snprintf(ev_lines[i], sizeof(ev_lines[i]),
                     "[fatto] %02d/%02d/%04d %02d:%02d  %s",
                     events[i].day, events[i].month, events[i].year,
                     events[i].hour, events[i].minute, events[i].title);
        else
            snprintf(ev_lines[i], sizeof(ev_lines[i]),
                     "%02d/%02d/%04d %02d:%02d  %s",
                     events[i].day, events[i].month, events[i].year,
                     events[i].hour, events[i].minute, events[i].title);
        ev_ptrs[i] = ev_lines[i];
    }
    if (ev_list_ready)
        doblv_SetItems(&ev_list, ev_ptrs, event_count);
}

/* Prompt the user for an event's date/time/title via input dialogs and
 * add (or, if edit_idx >= 0, replace) it. Returns true if the list
 * changed. Date prefilled from the currently shown month + day 1, or
 * from the existing event when editing. */
static bool ev_prompt_and_store(int edit_idx)
{
    char datebuf[16], timebuf[8], titlebuf[TITLE_MAX];

    if (edit_idx >= 0 && edit_idx < event_count)
    {
        snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d",
                 events[edit_idx].year, events[edit_idx].month,
                 events[edit_idx].day);
        snprintf(timebuf, sizeof(timebuf), "%02d:%02d",
                 events[edit_idx].hour, events[edit_idx].minute);
        strncpy(titlebuf, events[edit_idx].title, sizeof(titlebuf) - 1);
        titlebuf[sizeof(titlebuf) - 1] = '\0';
    }
    else
    {
        snprintf(datebuf, sizeof(datebuf), "%04d-%02d-01",
                 view_year, view_month);
        strcpy(timebuf, "09:00");
        titlebuf[0] = '\0';
    }

    /* Date */
    char in[TITLE_MAX];
    if (dobpopup_InputBox("Data evento", "Data (AAAA-MM-GG):",
                          datebuf, in, sizeof(in)) != 0)
        return false;
    int y, mo, d;
    {
        /* reuse the strict-ish parse: build a fake line to validate */
        const char *p = in; int n;
        y = 0; n = 0; while (*p>='0'&&*p<='9'){y=y*10+(*p-'0');p++;n++;}
        if (n != 4 || *p != '-') { dobpopup_Error("Data", "Formato non valido."); return false; }
        p++;
        mo = 0; n = 0; while (*p>='0'&&*p<='9'){mo=mo*10+(*p-'0');p++;n++;}
        if (n < 1 || *p != '-') { dobpopup_Error("Data", "Formato non valido."); return false; }
        p++;
        d = 0; n = 0; while (*p>='0'&&*p<='9'){d=d*10+(*p-'0');p++;n++;}
        if (n < 1) { dobpopup_Error("Data", "Formato non valido."); return false; }
        if (mo<1||mo>12||d<1||d>days_in_month(y,mo))
        { dobpopup_Error("Data", "Data inesistente."); return false; }
    }

    /* Time */
    if (dobpopup_InputBox("Ora evento", "Ora (HH:MM):",
                          timebuf, in, sizeof(in)) != 0)
        return false;
    int h, mi;
    {
        const char *p = in; int n;
        h = 0; n = 0; while (*p>='0'&&*p<='9'){h=h*10+(*p-'0');p++;n++;}
        if (n < 1 || *p != ':') { dobpopup_Error("Ora", "Formato non valido."); return false; }
        p++;
        mi = 0; n = 0; while (*p>='0'&&*p<='9'){mi=mi*10+(*p-'0');p++;n++;}
        if (n < 1) { dobpopup_Error("Ora", "Formato non valido."); return false; }
        if (h<0||h>23||mi<0||mi>59) { dobpopup_Error("Ora", "Ora non valida."); return false; }
    }

    /* Title */
    if (dobpopup_InputBox("Titolo evento", "Descrizione:",
                          titlebuf, in, sizeof(in)) != 0)
        return false;
    /* Strip a stray '|' so it can't corrupt the on-disk line format. */
    for (char *q = in; *q; q++) if (*q == '|') *q = '/';
    if (in[0] == '\0') { dobpopup_Error("Titolo", "Il titolo non puo' essere vuoto."); return false; }

    if (edit_idx >= 0 && edit_idx < event_count)
    {
        /* Edit = remove old + add new (keeps the list sorted). */
        events_remove(edit_idx);
    }
    return events_add(y, mo, d, h, mi, in);
}

/* --- Eventi tab --- */
static void draw_tab_events(void)
{
    dobui_FillRect(app_win, 0, TAB_H, APP_W, APP_H - TAB_H, ACOL_BG);

    if (event_count == 0)
    {
        draw_centered(app_win, 0, APP_W, TAB_H + 40,
                      "Nessun evento", ACOL_DIM, ACOL_BG);
        draw_centered(app_win, 0, APP_W, TAB_H + 40 + FONT_H + 6,
                      "Aggiungine uno dal Calendario", ACOL_DIM, ACOL_BG);
    }
    else
    {
        doblv_Draw(&ev_list);
    }

    /* "Cancella avvenuti" row (full width), above Modifica/Elimina. */
    dobui_FillRect(app_win, EV_CLR_X, EV_CLR_Y, EV_CLR_W, EV_CLR_H,
                   ECOL_CLR_BG);
    dobui_DrawRect(app_win, EV_CLR_X, EV_CLR_Y, EV_CLR_W, EV_CLR_H,
                   ECOL_CLR_BORDER);
    draw_centered(app_win, EV_CLR_X, EV_CLR_W,
                  EV_CLR_Y + (EV_CLR_H - FONT_H) / 2,
                  "Cancella avvenuti", ECOL_CLR_FG, ECOL_CLR_BG);

    /* Modifica / Elimina button row. */
    dobui_FillRect(app_win, EV_BTN1_X, EV_BTN_Y, EV_BTN_W, EV_BTN_H,
                   ECOL_EBTN_BG);
    dobui_DrawRect(app_win, EV_BTN1_X, EV_BTN_Y, EV_BTN_W, EV_BTN_H,
                   ECOL_EBTN_BORDER);
    draw_centered(app_win, EV_BTN1_X, EV_BTN_W,
                  EV_BTN_Y + (EV_BTN_H - FONT_H) / 2,
                  "Modifica", ECOL_EBTN_FG, ECOL_EBTN_BG);

    dobui_FillRect(app_win, EV_BTN2_X, EV_BTN_Y, EV_BTN_W, EV_BTN_H,
                   ECOL_DBTN_BG);
    dobui_DrawRect(app_win, EV_BTN2_X, EV_BTN_Y, EV_BTN_W, EV_BTN_H,
                   ECOL_DBTN_BORDER);
    draw_centered(app_win, EV_BTN2_X, EV_BTN_W,
                  EV_BTN_Y + (EV_BTN_H - FONT_H) / 2,
                  "Elimina", ECOL_DBTN_FG, ECOL_DBTN_BG);
}

/* Full repaint of the app window. */
static void draw_app(void)
{
    dobui_FillRect(app_win, 0, 0, APP_W, APP_H, ACOL_BG);
    draw_tabs();
    if (app_tab == TAB_CALENDAR)
        draw_tab_calendar();
    else
        draw_tab_events();
    dobui_Invalidate(app_win);
}

/* Mouse-press dispatch for the app window. */
static void app_on_press(int x, int y)
{
    /* Tab bar hit? */
    if (y >= TAB_Y && y < TAB_Y + TAB_H)
    {
        int tab = (x < TAB_W) ? TAB_CALENDAR : TAB_EVENTS;
        if (tab != app_tab)
        {
            app_tab = tab;
            /* Leaving a half-finished picker behind would be confusing;
             * always return the calendar to its day view on a tab switch. */
            cal_mode = CAL_DAYS;
            draw_app();
        }
        return;
    }

    if (app_tab == TAB_CALENDAR)
    {
        if (cal_mode == CAL_DAYS)
        {
            /* Month-nav arrows (generous edge bands on the title row). */
            if (y >= ANAV_Y - 4 && y < ANAV_Y + FONT_H + 4)
            {
                if (x < ACAL_X + ANAV_BTN_W)         { app_shift_month(-1); draw_app(); return; }
                if (x > APP_W - ACAL_X - ANAV_BTN_W) { app_shift_month(+1); draw_app(); return; }
            }

            /* Day cell hit -> start the picker at that day. */
            int grid_y = ACAL_HDR_Y + FONT_H + 6;
            int first_col = weekday_mon0(view_year, view_month, 1);
            int ndays = days_in_month(view_year, view_month);
            for (int dnum = 1; dnum <= ndays; dnum++)
            {
                int idx = first_col + (dnum - 1);
                int row = idx / CAL_COLS, col = idx % CAL_COLS;
                if (row >= CAL_ROWS) break;
                int cx = ACAL_X + col * ACELL_W;
                int cy = grid_y + row * ACELL_H;
                if (x >= cx && x < cx + ACELL_W &&
                    y >= cy && y < cy + ACELL_H)
                {
                    pick_day = dnum;
                    cal_mode = CAL_HOURS;
                    draw_app();
                    return;
                }
            }
            return;
        }

        if (cal_mode == CAL_HOURS)
        {
            /* Back to day view. */
            if (y >= BACK_Y - 4 && y < BACK_Y + FONT_H + 4 &&
                x >= BACK_X && x < BACK_X + BACK_W)
            {
                cal_mode = CAL_DAYS;
                draw_app();
                return;
            }
            /* Hour cell hit -> pick hour, advance to minutes. */
            int grid_y = ACAL_HDR_Y + FONT_H + 10;
            for (int h = 0; h < 24; h++)
            {
                int row = h / HR_COLS, col = h % HR_COLS;
                int cx = ACAL_X + col * HR_CELL_W;
                int cy = grid_y + row * HR_CELL_H;
                if (x >= cx && x < cx + HR_CELL_W &&
                    y >= cy && y < cy + HR_CELL_H)
                {
                    pick_hour = h;
                    cal_mode = CAL_MINUTES;
                    draw_app();
                    return;
                }
            }
            return;
        }

        /* cal_mode == CAL_MINUTES */
        {
            /* Back to hour view. */
            if (y >= BACK_Y - 4 && y < BACK_Y + FONT_H + 4 &&
                x >= BACK_X && x < BACK_X + BACK_W)
            {
                cal_mode = CAL_HOURS;
                draw_app();
                return;
            }
            /* Minute cell hit -> pick minute, prompt title, store. */
            int grid_y = ACAL_HDR_Y + FONT_H + 10;
            for (int m = 0; m < 60; m++)
            {
                int row = m / MIN_COLS, col = m % MIN_COLS;
                int cx = ACAL_X + col * MIN_CELL_W;
                int cy = grid_y + row * MIN_CELL_H;
                if (x >= cx && x < cx + MIN_CELL_W &&
                    y >= cy && y < cy + MIN_CELL_H)
                {
                    bool added = add_event_with_title(m);
                    /* Picker done either way: back to the day grid. */
                    cal_mode = CAL_DAYS;
                    if (added)
                    {
                        ev_rebuild_lines();
                        notify_tray_reload();
                        app_tab = TAB_EVENTS;   /* show the result */
                    }
                    draw_app();
                    return;
                }
            }
            return;
        }
    }
    else /* TAB_EVENTS */
    {
        /* List item selection. */
        if (event_count > 0 && doblv_OnClick(&ev_list, x, y))
        {
            draw_app();
            return;
        }
        /* Cancella avvenuti */
        if (x >= EV_CLR_X && x < EV_CLR_X + EV_CLR_W &&
            y >= EV_CLR_Y && y < EV_CLR_Y + EV_CLR_H)
        {
            /* Count first so we can tell the user there's nothing to do
             * (rather than silently no-op'ing). */
            int npast = 0;
            for (int i = 0; i < event_count; i++)
                if (events[i].fired) npast++;
            if (npast == 0)
            {
                dobpopup_Info("Cancella avvenuti",
                              "Nessun evento avvenuto da cancellare.");
            }
            else if (dobpopup_YesNo("Cancella avvenuti",
                                    "Cancellare tutti gli eventi avvenuti?") == 0)
            {
                events_clear_fired();
                ev_rebuild_lines();
                notify_tray_reload();
            }
            draw_app();
            return;
        }
        /* Modifica */
        if (x >= EV_BTN1_X && x < EV_BTN1_X + EV_BTN_W &&
            y >= EV_BTN_Y && y < EV_BTN_Y + EV_BTN_H)
        {
            int sel = doblv_GetSelectedIndex(&ev_list);
            if (sel < 0)
            {
                dobpopup_Info("Modifica", "Seleziona prima un evento.");
            }
            else if (ev_prompt_and_store(sel))
            {
                ev_rebuild_lines();
                notify_tray_reload();
            }
            draw_app();
            return;
        }
        /* Elimina */
        if (x >= EV_BTN2_X && x < EV_BTN2_X + EV_BTN_W &&
            y >= EV_BTN_Y && y < EV_BTN_Y + EV_BTN_H)
        {
            int sel = doblv_GetSelectedIndex(&ev_list);
            if (sel < 0)
            {
                dobpopup_Info("Elimina", "Seleziona prima un evento.");
            }
            else if (dobpopup_YesNo("Elimina evento",
                                    "Eliminare l'evento selezionato?") == 0)
            {
                events_remove(sel);
                ev_rebuild_lines();
                notify_tray_reload();
            }
            draw_app();
            return;
        }
    }
}

static int run_app(void)
{
    app_port = (uint32_t)port_create();

    dob_registry_wait("dobinterface", 5000);

    app_win = dobui_CreateWindow(APP_W, APP_H, app_port, "Agenda");
    if (app_win == 0)
        _exit(1);

    /* Fixed-layout agenda window: the month grid and the Eventi list
     * are positioned against APP_W/APP_H, so resize/maximize would
     * only break the layout.  Set before the first draw. */
    dobui_SetWindowFlags(app_win, DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);

    /* Declare timezone settings (idempotent) so they exist even if the
     * app is the first of the two modes to run. */
    declare_clock_settings();
    tz_refresh_offset();   /* seed cached offset; tick reads cache thereafter */

    /* Seed the shown month from the local date, and remember today for
     * the highlight. */
    uint32_t t[7];
    if (localtime_now(t) == 0)
    {
        today_year  = view_year  = (int)t[0];
        today_month = view_month = (int)t[1];
        today_day   = (int)t[2];
    }

    /* Load persisted events and build the Eventi list control. */
    events_load();
    doblv_Init(&ev_list, app_win, EV_LIST_X, EV_LIST_Y, EV_LIST_W, EV_LIST_H);
    ev_list_ready = true;
    ev_rebuild_lines();

    draw_app();

    /* If we're on the live CD, warn once up front: anything the user
     * adds won't survive, because the filesystem is a read-only RAM
     * image. Better than letting them carefully build an agenda that
     * silently evaporates. */
    if (is_live_mode())
        dobpopup_Info("Sistema live",
                      "Sei in modalita' live (da CD).\n"
                      "Gli eventi NON verranno salvati.\n"
                      "Installa MainDOB su disco per conservarli.");

    for (;;)
    {
        dob_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        dob_ipc_receive(app_port, &msg);

        switch (msg.code)
        {
            case GUI_EVT_MOUSE:
            {
                /* arg1=xy, arg2=buttons, arg3=etype. Press=1, release=2,
                 * wheel up=4, wheel down=5, drag=6. */
                if (msg.arg3 == ETYPE_PRESS)
                {
                    int x, y;
                    unpack_xy(msg.arg1, &x, &y);
                    app_on_press(x, y);
                }
                else if ((msg.arg3 == 4 || msg.arg3 == 5) &&
                         app_tab == TAB_EVENTS && event_count > 0)
                {
                    /* Wheel scrolls the events list. */
                    if (doblv_OnScroll(&ev_list, msg.arg3 == 4 ? -1 : 1))
                        draw_app();
                }
                break;
            }

            case GUI_EVT_KEY:
                /* Arrow keys scroll/select within the list when on the
                 * Eventi tab; reserved for month nav on Calendario. */
                if (app_tab == TAB_EVENTS && event_count > 0)
                {
                    if (doblv_OnKey(&ev_list, (uint8_t)msg.arg1))
                        draw_app();
                }
                break;

            case GUI_EVT_CLOSE_REQ:
                dobui_DestroyWindow(app_win);
                _exit(0);
                break;

            default:
                break;
        }
    }

    return 0;
}

/* === Entry === */

int main(int argc, char **argv)
{
    /* Two modes, selected by argv[0]. Anything that isn't exactly
     * "--app" falls through to tray, so a stray/empty arg can never
     * leave the boot-launched applet unstarted. */
    if (argc >= 1 && argv && argv[0] && strcmp(argv[0], "--app") == 0)
        return run_app();

    return run_tray();
}
