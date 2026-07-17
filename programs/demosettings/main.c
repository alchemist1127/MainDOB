/* demosettings.mdl -- a worked example of the MainDOB settings API.
 *
 * This program owns a small `.setting` file and demonstrates the whole
 * round trip:
 *
 *   1. On startup it DECLARES its settings (declareSetting /
 *      declareSettingMulti).  The settings daemon creates
 *      demosettings.setting the first time, filling each field with
 *      its default; on later runs the call is idempotent -- the schema
 *      is refreshed and the user's stored values are kept.
 *
 *   2. It READS the values back (getSetting / settingField) and shows
 *      them in its window.
 *
 * A `.setting` file is never shipped on disk: it is created by the
 * daemon from a program's declarations.  So the way to "place a demo
 * .setting" is a demo program that declares one -- this file.  After
 * it has run once, "demosettings" appears in the DobSettings editor;
 * edit a value there, relaunch this program, and the new value shows.
 *
 * The settings declared cover every control type:
 *   - general.welcome          a string  (textbox)
 *   - general.start_maximized  a bool    (checkbox)
 *   - general.theme            an enum   (dropdown)
 *   - display.resolution       a COMPOSITE setting: one entry / one
 *                              rectangle with two fields (width,
 *                              height), each its own textbox.
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include <app.h>
#include <DobInterface.h>
#include <DobSettings.h>

#include <dob/ipc.h>
#include <dob/registry.h>

#define WIN_W   440
#define WIN_H   272

#include <dobui_theme.h>
#define COL_BG     DOBUI_INSET
#define COL_TITLE  DOBUI_TEXT
#define COL_LABEL  DOBUI_TEXT_ALT
#define COL_VALUE  DOBUI_INPUT
#define COL_HINT   DOBUI_DISABLED

static uint32_t win_id;

/* Schema constants for the declarations below. */
static const char *const THEMES[] = { "Light", "Dark", "Blue", 0 };

static const setting_field_t RES_FIELDS[] =
{
    { SETTING_STRING, "Width",  "1024", 0 },
    { SETTING_STRING, "Height", "768",  0 },
};

/* ------------------------------------------------------------------ */

/* Declare every setting this program owns.  Idempotent -- safe to call
 * on every startup. */
static void declare_my_settings(void)
{
    declareSetting("general.welcome", SETTING_STRING,
                   "Welcome message", "Hello, MainDOB!", 0);

    declareSetting("general.start_maximized", SETTING_BOOL,
                   "Start maximized", "false", 0);

    declareSetting("general.theme", SETTING_ENUM,
                   "Theme", "Light", THEMES);

    /* A controlling switch and a composite setting it governs:
     * "Preferred resolution" is greyed out in the editor while custom
     * resolution is off. */
    declareSetting("display.custom_resolution", SETTING_BOOL,
                   "Use custom resolution", "false", 0);

    /* One composite setting: a single rectangle holding two textboxes. */
    declareSettingMulti("display.resolution", "Preferred resolution",
                        RES_FIELDS, 2);

    /* The "false" option of display.custom_resolution invalidates
     * display.resolution.  Declared after both entries exist. */
    {
        static const char *const targets[] = { "display.resolution", 0 };
        declareSettingInvalidation("display.custom_resolution",
                                   "false", targets);
    }
}

/* Draw one "Label: value" line and advance *y. */
static void draw_line(int *y, const char *label, const char *value)
{
    int lx = 18;
    int vx = lx + 9 * 18;     /* value column, ~18 chars in */
    char shown[96];

    dobui_DrawText(win_id, lx, *y, label, COL_LABEL, COL_BG);

    strncpy(shown, (value && value[0]) ? value : "(non disponibile)",
            sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    dobui_DrawText(win_id, vx, *y, shown, COL_VALUE, COL_BG);

    *y += 24;
}

static void draw_all(void)
{
    char welcome[SETTINGS_MAX_VALUE_LEN];
    char maxd[32];
    char theme[64];
    char w[32], h[32], res[80];
    const char *p;
    int y = 18;

    dobui_FillRect(win_id, 0, 0, WIN_W, WIN_H, COL_BG);

    dobui_DrawText(win_id, 18, y,
                   "Demo impostazioni - valori correnti",
                   COL_TITLE, COL_BG);
    y += 32;

    /* getSetting() returns a buffer that the NEXT getSetting() call
     * reuses -- so each value is copied out before the next fetch. */
    p = getSetting("general.welcome");
    welcome[0] = '\0';
    if (p) { strncpy(welcome, p, sizeof(welcome) - 1);
             welcome[sizeof(welcome) - 1] = '\0'; }

    p = getSetting("general.start_maximized");
    maxd[0] = '\0';
    if (p) { strncpy(maxd, p, sizeof(maxd) - 1);
             maxd[sizeof(maxd) - 1] = '\0'; }

    p = getSetting("general.theme");
    theme[0] = '\0';
    if (p) { strncpy(theme, p, sizeof(theme) - 1);
             theme[sizeof(theme) - 1] = '\0'; }

    /* settingField() copies straight into our buffer -- no aliasing. */
    w[0] = h[0] = '\0';
    settingField("display.resolution", 0, w, sizeof(w));
    settingField("display.resolution", 1, h, sizeof(h));
    snprintf(res, sizeof(res), "%s x %s",
             w[0] ? w : "?", h[0] ? h : "?");

    draw_line(&y, "Welcome",      welcome);
    draw_line(&y, "Maximized",    maxd);
    draw_line(&y, "Theme",        theme);

    /* Honour the dependency: "display.resolution" only applies while
     * "display.custom_resolution" is on.  The editor greys the entry
     * out; a program must still check the controlling setting itself. */
    {
        char custom[32];
        custom[0] = '\0';
        p = getSetting("display.custom_resolution");
        if (p) { strncpy(custom, p, sizeof(custom) - 1);
                 custom[sizeof(custom) - 1] = '\0'; }
        if (strcmp(custom, "true") == 0)
            draw_line(&y, "Resolution", res);
        else
            draw_line(&y, "Resolution", "(predefinita)");
    }

    y += 14;
    dobui_DrawText(win_id, 18, y,
                   "Aprili in Impostazioni per modificarli,",
                   COL_HINT, COL_BG);
    y += 18;
    dobui_DrawText(win_id, 18, y,
                   "poi riavvia questo programma.",
                   COL_HINT, COL_BG);

    dobui_Invalidate(win_id);
}

/* ------------------------------------------------------------------ */

void event_start(void)
{
    win_id = dobui_window();

    /* Register under our own name so the DobSettings editor can reach
     * us with the reload call after the user presses Apply. */
    dob_registry_register("demosettings", dobui_port());

    declare_my_settings();
    draw_all();
}

/* The settings editor sends this after "Applica".  Re-reading and
 * redrawing here is all "actualising the settings" means for this
 * program -- another program might re-apply a theme, resize a window,
 * reopen a device, whatever it needs.  A fire-and-forget post: no
 * reply is owed. */
void event_request(dob_msg_t *msg)
{
    if (msg->code == SETTINGS_RELOAD_CALL)
        draw_all();
}

void event_close(void)
{
    dobui_quit();
}

int main(void)
{
    dobui_run("Demo Impostazioni", WIN_W, WIN_H);
    return 0;
}
