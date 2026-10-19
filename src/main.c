#define GPROC_IMPLEMENTATION
#include "gcui.h"
#include "proc.h"

#include <float.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define MARGIN    16.0f
#define GAP       8.0f
#define HEADER_H  88.0f
#define TOOLBAR_H 48.0f
#define SORT_H    32.0f
#define FOOTER_H  56.0f
#define ROW_H     28.0f
#define COL_N     4

static const GCCOLOR C_PANEL = {36, 36, 42, 255};
static const GCCOLOR C_BODY  = {28, 28, 33, 255};
static const GCCOLOR C_TEXT  = {245, 245, 245, 255};
static const GCCOLOR C_DIM   = {180, 180, 190, 255};
static const GCCOLOR C_SEL   = {50, 120, 220, 255};

static const char *col_names[COL_N] = {"PID", "CPU%", "MEM", "NAME"};
static const i32   col_keys[COL_N]  = {PROC_SORT_PID, PROC_SORT_CPU, PROC_SORT_MEM, PROC_SORT_NAME};

static GCARENA  *arena;
static GCWINDOW *window;
static GCUI     *ui;

static Proc    procs[PROC_MAX];
static i32     order[PROC_MAX];
static i32     nproc;
static SysInfo sys;

static char   *filter;
static GCUSIZE filter_cap;

static i32        sort_key  = PROC_SORT_CPU;
static b32        sort_desc = true;
static i32        sel       = -1;
static GCUISCROLL scroll;
static f32        refresh = 0.5f;
static f32        timer   = 1e9f;

static char status[128];

static void pick_sort(i32 key)
{
    if (sort_key == key) {
        sort_desc = !sort_desc;
    } else {
        sort_key  = key;
        sort_desc = (key == PROC_SORT_CPU || key == PROC_SORT_MEM);
    }
    proc_sort(procs, nproc, sort_key, sort_desc);
}

static const char *sel_cmd(void)
{
    for (i32 i = 0; i < nproc; i++) {
        if (procs[i].pid == sel) {
            return procs[i].cmd;
        }
    }
    return "-";
}

static void signal_sel(i32 sig, const char *name)
{
    if (sel > 0 && proc_kill(sel, sig)) {
        snprintf(status, sizeof(status), "sent %s to %d", name, sel);
    } else {
        snprintf(status, sizeof(status), "%s failed for %d", name, sel);
    }
}

static void frame(void)
{
    gc_window_begin(window);
    gc_ui_begin(ui);

    f32 dt = gc_window_dt(window);
    f32 w  = gc_window_width(window);
    f32 h  = gc_window_height(window);
    char line[256];

    timer += dt;
    if (timer >= refresh) {
        timer = 0.0f;
        if (!proc_snapshot(procs, &nproc, &sys)) {
            snprintf(status, sizeof(status), "snapshot failed");
        } else {
            proc_sort(procs, nproc, sort_key, sort_desc);
        }
    }

    i32 nshow = 0;
    for (i32 i = 0; i < nproc; i++) {
        if (proc_match(&procs[i], filter)) {
            order[nshow++] = i;
        }
    }

    f32 rowX  = MARGIN;
    f32 rowW  = w - 2.0f * MARGIN;
    f32 pidW  = 90.0f;
    f32 cpuW  = 100.0f;
    f32 memW  = 120.0f;
    f32 nameW = rowW - pidW - cpuW - memW;
    if (nameW < 80.0f) {
        nameW = 80.0f;
    }
    f32 col_x[COL_N] = {rowX, rowX + pidW, rowX + pidW + cpuW, rowX + pidW + cpuW + memW};
    f32 col_w[COL_N] = {pidW, cpuW, memW, nameW};

    f32 headerY  = MARGIN;
    f32 toolbarY = headerY + HEADER_H + GAP;
    f32 sortY    = toolbarY + TOOLBAR_H + GAP;
    f32 footerY  = h - MARGIN - FOOTER_H;
    f32 bodyY    = sortY + SORT_H + GAP;
    f32 bodyH    = footerY - GAP - bodyY;
    if (bodyH < ROW_H) {
        bodyH = ROW_H;
    }

    gc_ui_panel(ui, (GCRECT){MARGIN, headerY, w - 2.0f * MARGIN, HEADER_H}, C_PANEL);
    gc_ui_progress(ui, (GCRECT){MARGIN + 16.0f, headerY + 14.0f, 300.0f, 16.0f},
                   sys.cpu_pct / 100.0f);
    snprintf(line, sizeof(line), "CPU %.1f%%", (f64)sys.cpu_pct);
    gc_ui_label(ui, (GCRECT){MARGIN + 332.0f, headerY + 8.0f, 200.0f, 28.0f}, line, C_TEXT);

    gc_ui_progress(ui, (GCRECT){MARGIN + 16.0f, headerY + 42.0f, 300.0f, 16.0f},
                   sys.mem_pct / 100.0f);
    u64 mem_used = sys.mem_total_kb - sys.mem_avail_kb;
    snprintf(line, sizeof(line), "MEM %.1f%% %llu/%llu MB", (f64)sys.mem_pct,
             (unsigned long long)mem_used / 1024ULL,
             (unsigned long long)sys.mem_total_kb / 1024ULL);
    gc_ui_label(ui, (GCRECT){MARGIN + 332.0f, headerY + 36.0f, 460.0f, 28.0f}, line, C_TEXT);

    i32 up = (i32)sys.uptime_s;
    snprintf(line, sizeof(line), "up %d:%02d:%02d - tasks %d/%d", up / 3600, (up % 3600) / 60,
             up % 60, nshow, nproc);
    gc_ui_label(ui, (GCRECT){w - MARGIN - 16.0f - 440.0f, headerY + 8.0f, 440.0f, 28.0f}, line,
                C_DIM);

    gc_ui_panel(ui, (GCRECT){MARGIN, toolbarY, w - 2.0f * MARGIN, TOOLBAR_H}, C_PANEL);
    f32 tx = MARGIN + 16.0f;
    gc_ui_label(ui, (GCRECT){tx, toolbarY + 10.0f, 70.0f, 28.0f}, "Filter", C_TEXT);
    gc_ui_textbox(ui, (GCRECT){tx + 68.0f, toolbarY + 6.0f, 300.0f, 36.0f}, &filter, &filter_cap,
                  arena);
    gc_ui_label(ui, (GCRECT){tx + 388.0f, toolbarY + 10.0f, 90.0f, 28.0f}, "Refresh", C_TEXT);
    gc_ui_slider_horizontal(ui, (GCRECT){tx + 478.0f, toolbarY + 12.0f, 160.0f, 24.0f}, 0.2f,
                            100.0f, &refresh);
    snprintf(line, sizeof(line), "%.1fs", (f64)refresh);
    gc_ui_label(ui, (GCRECT){tx + 646.0f, toolbarY + 10.0f, 80.0f, 28.0f}, line, C_DIM);
    if (gc_ui_button(ui, (GCRECT){tx + 734.0f, toolbarY + 10.0f, 100.0f, 28.0f}, "Max")) {
        refresh = 100.0f;
    }
    snprintf(line, sizeof(line), "shown %d", nshow);
    gc_ui_label(ui, (GCRECT){w - MARGIN - 16.0f - 120.0f, toolbarY + 10.0f, 120.0f, 28.0f}, line,
                C_DIM);

    for (i32 c = 0; c < COL_N; c++) {
        char bl[16];
        if (col_keys[c] == sort_key) {
            snprintf(bl, sizeof(bl), "%s %s", col_names[c], sort_desc ? "v" : "^");
        } else {
            snprintf(bl, sizeof(bl), "%s", col_names[c]);
        }
        if (gc_ui_button(ui, (GCRECT){col_x[c], sortY, col_w[c], SORT_H}, bl)) {
            pick_sort(col_keys[c]);
        }
    }

    gc_ui_panel(ui, (GCRECT){MARGIN, bodyY, w - 2.0f * MARGIN, bodyH}, C_BODY);

    i32 vis = (i32)(bodyH / ROW_H);
    if (gc_window_key_was_pressed(window, GC_KEY_PAGEDOWN)) {
        scroll.offset += (f32)vis * ROW_H;
    }
    if (gc_window_key_was_pressed(window, GC_KEY_PAGEUP)) {
        scroll.offset -= (f32)vis * ROW_H;
    }
    if (gc_window_key_was_pressed(window, GC_KEY_HOME)) {
        scroll.offset = 0.0f;
    }
    if (gc_window_key_was_pressed(window, GC_KEY_END)) {
        scroll.offset = FLT_MAX;
    }

    b32    has_bar = (f32)nshow * ROW_H > bodyH;
    f32    barW    = 10.0f;
    GCRECT bar     = {rowX + rowW - barW, bodyY, barW, bodyH};

    gc_ui_scroll_begin(ui, &scroll, (GCRECT){MARGIN, bodyY, w - 2.0f * MARGIN, bodyH},
                       (f32)nshow * ROW_H, ROW_H * 3.0f);

    i32 first = (i32)(scroll.offset / ROW_H);
    i32 last  = first + vis;
    if (last > nshow) {
        last = nshow;
    }

    for (i32 vi = first; vi < last; vi++) {
        Proc  *p      = &procs[order[vi]];
        f32    y      = bodyY + (f32)(vi - first) * ROW_H;
        GCRECT row    = {rowX, y, rowW, ROW_H};
        GCB32  inside = gc_rect_contains(row, ui->mouseX, ui->mouseY);
        if (has_bar && gc_rect_contains(bar, ui->mouseX, ui->mouseY)) {
            inside = GC_FALSE;
        }

        if (p->pid == sel) {
            gc_draw_fill(window, row, C_SEL);
        } else if (inside) {
            gc_draw_outline(window, row, C_DIM);
        }
        if (inside && ui->mousePressed) {
            sel = p->pid;
        }

        snprintf(line, sizeof(line), "%d", p->pid);
        gc_ui_label(ui, (GCRECT){col_x[0], y, col_w[0], ROW_H}, line, C_TEXT);
        snprintf(line, sizeof(line), "%.1f", (f64)p->cpu_pct);
        gc_ui_label(ui, (GCRECT){col_x[1], y, col_w[1], ROW_H}, line, C_TEXT);
        snprintf(line, sizeof(line), "%.1fM", (f64)p->rss_kb / 1024.0);
        gc_ui_label(ui, (GCRECT){col_x[2], y, col_w[2], ROW_H}, line, C_TEXT);
        gc_ui_label(ui, (GCRECT){col_x[3], y, col_w[3], ROW_H}, p->cmd, C_DIM);
    }

    gc_ui_scroll_end(ui, &scroll);

    gc_ui_panel(ui, (GCRECT){MARGIN, footerY, w - 2.0f * MARGIN, FOOTER_H}, C_PANEL);
    snprintf(line, sizeof(line), "sel %d %s", sel, sel_cmd());
    gc_ui_label(ui, (GCRECT){MARGIN + 16.0f, footerY + 14.0f, 520.0f, 28.0f}, line, C_TEXT);

    f32 killX = w - MARGIN - 16.0f - 120.0f;
    f32 termX = killX - GAP - 120.0f;
    gc_ui_label(ui, (GCRECT){termX - 268.0f, footerY + 14.0f, 256.0f, 28.0f}, status, C_DIM);
    if (gc_ui_button(ui, (GCRECT){termX, footerY + 8.0f, 120.0f, 40.0f}, "Term")) {
        signal_sel(SIGTERM, "TERM");
    }
    if (gc_ui_button(ui, (GCRECT){killX, footerY + 8.0f, 120.0f, 40.0f}, "Kill")) {
        signal_sel(SIGKILL, "KILL");
    }

    gc_ui_end(ui);
    gc_window_end(window);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(void)
#endif
{
#ifdef _WIN32
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;
#endif
    arena  = gc_arena_create(GC_MB(2));
    window = gc_window_create(arena, "gproc", 1280, 720, GC_FALSE);
    if (!window) {
        gc_arena_destroy(arena);
        return 1;
    }
    ui = gc_ui_create(arena, window);
    gc_ui_load_default_font(ui, 18.0f);

    filter = (char *)gc_arena_push(arena, 256);
    memset(filter, 0, 256);
    filter_cap = 256;

    snprintf(status, sizeof(status), "ready");

    while (!gc_window_should_quit(window)) {
        if (gc_window_key_was_pressed(window, GC_KEY_ESCAPE)) {
            break;
        }
        frame();
    }

    gc_ui_destroy(ui);
    gc_window_destroy(window);
    gc_arena_destroy(arena);
    return 0;
}
