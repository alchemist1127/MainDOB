/* MainDOB Benchmark — System Performance Test Suite
 *
 * Tests core subsystem performance and displays results
 * in a GUI window. Panel command "Ripeti" reruns all tests.
 *
 * Tests:
 *   1. Syscall overhead (clock_ms round-trip)
 *   2. IPC round-trip latency (registry lookup)
 *   3. SHM create + unmap cycle
 *   4. Memory allocation (malloc + free)
 *   5. Framebuffer fill (FillRect throughput)
 *   6. Text rendering throughput
 *   7. Port create throughput
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <app.h>
#include <DobInterface.h>
#include <dob/registry.h>
#include <dob/types.h>

/* Window */
static uint32_t win_id;
static int win_w = 480, win_h = 440;

/* Colors */
#define COL_BG          0x001A1A2E
#define COL_HEADER      0x0016213E
#define COL_TEXT         0x00E0E0E0
#define COL_LABEL       0x0000AAFF
#define COL_VALUE       0x0000FF88
#define COL_UNIT        0x00888888
#define COL_BAR_BG      0x00333344
#define COL_BAR_FG      0x000066CC
#define COL_RUNNING     0x00FFAA00
#define COL_TITLE       0x00FFFFFF

/* Test results */
#define NUM_TESTS       7
#define FONT_H          16
#define FONT_W          8
#define ROW_H           22
#define BAR_H           10
#define BAR_W           120

typedef struct
{
    const char *name;
    uint32_t    ops;
    uint32_t    avg_us;
    bool        done;
} test_result_t;

static test_result_t results[NUM_TESTS];

/* Status line shown at the bottom of the window.  Stored in a
 * shared buffer so draw_all() can include it on every redraw --
 * required under cmdlist semantics, where Invalidate resets the
 * window contents to whatever was in the cmdbuf this frame. */
static char status_msg[128] = "";

static const char *test_names[NUM_TESTS] =
{
    "Syscall overhead",
    "IPC round-trip",
    "SHM create/unmap",
    "malloc/free",
    "FillRect 100x100",
    "DrawText 32 chars",
    "Port create/destroy",
};

static void reset_results(void)
{
    for (int i = 0; i < NUM_TESTS; i++)
    {
        results[i].name = test_names[i];
        results[i].ops = 0;
        results[i].avg_us = 0;
        results[i].done = false;
    }
}

/* ================================================================ */

static void uint_to_str(uint32_t val, char *buf, int buflen)
{
    if (buflen < 2) { buf[0] = '\0'; return; }
    char tmp[12];
    int pos = 0;
    if (val == 0) { tmp[pos++] = '0'; }
    else { while (val > 0 && pos < 11) { tmp[pos++] = '0' + (char)(val % 10); val /= 10; } }

    int out = 0;
    for (int i = pos - 1; i >= 0 && out < buflen - 1; i--)
    {
        buf[out++] = tmp[i];
        if (i > 0 && i % 3 == 0 && out < buflen - 1)
            buf[out++] = ',';
    }
    buf[out] = '\0';
}

/* Drawing */

static void draw_header(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, 32, COL_HEADER);
    dobui_DrawText(win_id, 12, 8, "MainDOB Benchmark", COL_TITLE, COL_HEADER);

    uint32_t ver[5];
    syscall1(SYS_GETVERSION, (int)ver);
    char verbuf[32];
    sprintf(verbuf, "v%u.%u.%u.%u.%u", ver[0], ver[1], ver[2], ver[3], ver[4]);
    int vx = win_w - ((int)strlen(verbuf) * FONT_W) - 12;
    dobui_DrawText(win_id, vx, 8, verbuf, COL_UNIT, COL_HEADER);
}

static void draw_result(int idx)
{
    int y = 40 + idx * (ROW_H + BAR_H + 8);
    test_result_t *r = &results[idx];

    dobui_FillRect(win_id, 0, y, win_w, ROW_H + BAR_H + 4, COL_BG);
    dobui_DrawText(win_id, 12, y + 2, r->name, COL_LABEL, COL_BG);

    if (!r->done)
    {
        dobui_DrawText(win_id, win_w - 100, y + 2, "running...", COL_RUNNING, COL_BG);
        return;
    }

    char val[24];
    uint_to_str(r->ops, val, sizeof(val));
    int vx = win_w - ((int)strlen(val) * FONT_W) - 80;
    dobui_DrawText(win_id, vx, y + 2, val, COL_VALUE, COL_BG);
    dobui_DrawText(win_id, win_w - 72, y + 2, "ops/s", COL_UNIT, COL_BG);

    int bar_y = y + ROW_H;
    dobui_FillRect(win_id, 12, bar_y, BAR_W, BAR_H, COL_BAR_BG);
    int fill = 0;
    if (r->avg_us > 0)
    {
        fill = (int)r->avg_us * BAR_W / 1000;
        if (fill > BAR_W) fill = BAR_W;
        if (fill < 1) fill = 1;
    }
    dobui_FillRect(win_id, 12, bar_y, fill, BAR_H, COL_BAR_FG);

    char lat[24];
    uint_to_str(r->avg_us, lat, sizeof(lat));
    int lx = 12 + BAR_W + 8;
    dobui_DrawText(win_id, lx, bar_y - 2, lat, COL_TEXT, COL_BG);
    int ux = lx + (int)strlen(lat) * FONT_W + 4;
    dobui_DrawText(win_id, ux, bar_y - 2, "us avg", COL_UNIT, COL_BG);
}

static void draw_all(void)
{
    dobui_FillRect(win_id, 0, 0, win_w, win_h, COL_BG);
    draw_header();
    for (int i = 0; i < NUM_TESTS; i++)
        draw_result(i);
    /* Status line at the bottom -- always emitted (empty buffer
     * draws nothing visible, only the bg fill). */
    if (status_msg[0])
    {
        int y = win_h - 20;
        dobui_DrawText(win_id, 12, y + 2, status_msg, COL_UNIT, COL_BG);
    }
    dobui_Invalidate(win_id);
}

static void draw_status(const char *msg)
{
    /* Store the message and trigger a full redraw — incremental
     * status strip update would be wiped by the next Invalidate. */
    strncpy(status_msg, msg ? msg : "", sizeof(status_msg) - 1);
    status_msg[sizeof(status_msg) - 1] = '\0';
    draw_all();
}

/* Tests */

static void record(int idx, uint32_t iterations, uint32_t elapsed_ms)
{
    if (elapsed_ms == 0) elapsed_ms = 1;
    results[idx].ops = iterations * 1000 / elapsed_ms;
    results[idx].avg_us = elapsed_ms * 1000 / iterations;
    results[idx].done = true;
}

static void test_syscall(void)
{
    uint32_t n = 50000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++) clock_ms();
    record(0, n, clock_ms() - start);
}

static void test_ipc(void)
{
    uint32_t port = dob_registry_find("config");
    if (!port) { results[1].done = true; return; }
    uint32_t n = 5000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++) dob_registry_find("config");
    record(1, n, clock_ms() - start);
}

static void test_shm(void)
{
    uint32_t n = 2000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t vaddr = 0;
        int id = shm_create(4096, &vaddr);
        if (id >= 0) shm_unmap(id);
    }
    record(2, n, clock_ms() - start);
}

static void test_malloc(void)
{
    uint32_t n = 50000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++)
    {
        void *p = malloc(256);
        if (p) free(p);
    }
    record(3, n, clock_ms() - start);
}

static void test_fillrect(void)
{
    int sy = win_h - 100;
    if (sy < 0) sy = 0;
    uint32_t n = 5000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++)
        dobui_FillRect(win_id, 0, sy, 100, 100, COL_BG);
    record(4, n, clock_ms() - start);
    dobui_FillRect(win_id, 0, sy, 100, 100, COL_BG);
}

static void test_text(void)
{
    const char *str = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    int sy = win_h - 20;
    if (sy < 0) sy = 0;
    uint32_t n = 3000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++)
        dobui_DrawText(win_id, 0, sy, str, COL_TEXT, COL_BG);
    record(5, n, clock_ms() - start);
    dobui_FillRect(win_id, 0, sy, win_w, 20, COL_BG);
}

static void test_port(void)
{
    uint32_t n = 10000;
    uint32_t start = clock_ms();
    for (uint32_t i = 0; i < n; i++)
    {
        int p = port_create();
        if (p >= 0) port_destroy(p);
    }
    record(6, n, clock_ms() - start);
}

/* Run all tests */

typedef void (*test_fn)(void);

static test_fn tests[NUM_TESTS] =
{
    test_syscall, test_ipc, test_shm, test_malloc,
    test_fillrect, test_text, test_port,
};

static void run_all_tests(void)
{
    reset_results();
    draw_all();
    draw_status("In esecuzione...");
    sleep_ms(300);

    for (int i = 0; i < NUM_TESTS; i++)
    {
        draw_all();
        tests[i]();
        draw_all();
        sleep_ms(80);
    }

    draw_status("Completato. Usa 'Ripeti' per rieseguire.");
    /* draw_status chains into draw_all which already Invalidates —
     * a second Invalidate here would flush an empty cmdbuf and
     * blank the window. */
}

/* Event handlers */

void event_close(void)
{
    dobui_quit();
}

void event_panel(int cmd_idx)
{
    if (cmd_idx == 0)
        run_all_tests();
}

void event_resize(int w, int h)
{
    win_w = w;
    win_h = h;
    draw_all();
}

void event_start(void)
{
    win_id = dobui_window();
    /* Fixed-layout window — neither resize nor maximize serve any
     * purpose here. */
    dobui_SetWindowFlags(win_id, DOBUI_WIN_NORESIZE | DOBUI_WIN_NOMAXIMIZE);
    run_all_tests();
}

int main(void)
{
    /* Promote to realtime (priority 0). The kernel accepts this for
     * the benchmark process by name (see sys_set_priority's exemption).
     * Without this, the strict-priority scheduler caps benchmark at
     * normal/2 and the idle thread (prio 3) eats ~33% of every slice
     * boundary — measurements jitter upward by 30-40% and any
     * "synthetic noise" (mouse motion, window paint) bumps the
     * apparent throughput. At prio 0 nothing else preempts us, so
     * the numbers reflect the actual subsystem cost. */
    set_priority(0);

    dobui_set_panel("Ripeti");
    dobui_run("Benchmark", 480, 440);
    return 0;
}
