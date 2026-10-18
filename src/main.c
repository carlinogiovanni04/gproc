#define GPROC_IMPLEMENTATION
#include "gcui.h"
#include "proc.h"

#include <float.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 1280
#define WIN_H 720
#define ROW_H 28.0f
#define COL_N 4

static const GCCOLOR C_PANEL = {36, 36, 42, 255};
static const GCCOLOR C_BODY  = {28, 28, 33, 255};
static const GCCOLOR C_TEXT  = {245, 245, 245, 255};
static const GCCOLOR C_DIM   = {180, 180, 190, 255};
static const GCCOLOR C_SEL   = {50, 120, 220, 255};

static const f32 col_x[COL_N] = {24.0f, 112.0f, 210.0f, 328.0f};
static const f32 col_w[COL_N] = {88.0f, 98.0f, 118.0f, 920.0f};

static const char *col_names[COL_N] = {"PID", "CPU%", "MEM", "NAME"};
static const i32   col_keys[COL_N]  = {PROC_SORT_PID, PROC_SORT_CPU, PROC_SORT_MEM, PROC_SORT_NAME};

typedef struct {
    GCARENA  *arena;
    GCWINDOW *window;
    GCUI     *ui;

    Proc    procs[PROC_MAX];
    i32     order[PROC_MAX];
    i32     nproc;
    SysInfo sys;

    char   *filter;
    GCUSIZE filter_cap;

    i32 sort_key;
    b32 sort_desc;
    i32 sel;

    f32 scroll;
    f32 refresh;
    f32 timer;

    char status[128];
    char line[256];
} App;

static App app; //NOTE(Giovanni): global for now

static void app_label(App *a, f32 x, f32 y, f32 w, f32 h, const char *text, GCCOLOR c)
{
    gc_ui_label(a->ui, (GCRECT){x, y, w, h}, text, c);
}

static void app_textf(App *a, f32 x, f32 y, f32 w, f32 h, GCCOLOR c, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(a->line, sizeof(a->line), fmt, args);
    va_end(args);
    gc_ui_label(a->ui, (GCRECT){x, y, w, h}, a->line, c);
}

static b32 app_button(App *a, f32 x, f32 y, f32 w, f32 h, const char *label)
{
    return gc_ui_button(a->ui, (GCRECT){x, y, w, h}, label);
}

static void app_clamp_scroll(App *a, f32 max)
{
    if (a->scroll > max) {
        a->scroll = max;
    }
    if (a->scroll < 0.0f) {
        a->scroll = 0.0f;
    }
}

static void app_resnapshot(App *a, f32 dt)
{
    a->timer += dt;
    if (a->timer < a->refresh) {
        return;
    }
    a->timer = 0.0f;
    if (!proc_snapshot(a->procs, &a->nproc, &a->sys)) {
        snprintf(a->status, sizeof(a->status), "snapshot failed");
        return;
    }
    proc_sort(a->procs, a->nproc, a->sort_key, a->sort_desc);
}

static i32 app_filter(App *a)
{
    i32 nshow = 0;
    for (i32 i = 0; i < a->nproc; i++) {
        if (proc_match(&a->procs[i], a->filter)) {
            a->order[nshow++] = i;
        }
    }
    return nshow;
}

static void app_pick_sort(App *a, i32 key)
{
    if (a->sort_key == key) {
        a->sort_desc = !a->sort_desc;
    } else {
        a->sort_key  = key;
        a->sort_desc = (key == PROC_SORT_CPU || key == PROC_SORT_MEM);
    }
    proc_sort(a->procs, a->nproc, a->sort_key, a->sort_desc);
}

static const char *app_sel_cmd(App *a)
{
    for (i32 i = 0; i < a->nproc; i++) {
        if (a->procs[i].pid == a->sel) {
            return a->procs[i].cmd;
        }
    }
    return "-";
}

static void app_signal_sel(App *a, i32 sig, const char *name)
{
    if (a->sel > 0 && proc_kill(a->sel, sig)) {
        snprintf(a->status, sizeof(a->status), "sent %s to %d", name, a->sel);
    } else {
        snprintf(a->status, sizeof(a->status), "%s failed for %d", name, a->sel);
    }
}

static void app_header(App *a, i32 nshow)
{
    gc_ui_panel(a->ui, (GCRECT){16.0f, 16.0f, 1248.0f, 88.0f}, C_PANEL);

    gc_ui_progress(a->ui, (GCRECT){32.0f, 30.0f, 300.0f, 16.0f}, a->sys.cpu_pct / 100.0f);
    app_textf(a, 340.0f, 24.0f, 200.0f, 28.0f, C_TEXT, "CPU %.1f%%", (f64)a->sys.cpu_pct);

    gc_ui_progress(a->ui, (GCRECT){32.0f, 58.0f, 300.0f, 16.0f}, a->sys.mem_pct / 100.0f);
    u64 mem_used = a->sys.mem_total_kb - a->sys.mem_avail_kb;
    app_textf(a, 340.0f, 52.0f, 420.0f, 28.0f, C_TEXT, "MEM %.1f%% %llu/%llu MB",
              (f64)a->sys.mem_pct, (unsigned long long)mem_used / 1024ULL,
              (unsigned long long)a->sys.mem_total_kb / 1024ULL);

    i32 up = (i32)a->sys.uptime_s;
    app_textf(a, 800.0f, 24.0f, 440.0f, 28.0f, C_DIM, "up %d:%02d:%02d - tasks %d/%d", up / 3600,
              (up % 3600) / 60, up % 60, nshow, a->nproc);
}

static void app_toolbar(App *a, i32 nshow)
{
    gc_ui_panel(a->ui, (GCRECT){16.0f, 112.0f, 1248.0f, 48.0f}, C_PANEL);

    app_label(a, 32.0f, 120.0f, 70.0f, 28.0f, "Filter", C_TEXT);
    gc_ui_textbox(a->ui, (GCRECT){100.0f, 118.0f, 300.0f, 36.0f}, &a->filter, &a->filter_cap,
                  a->arena);

    app_label(a, 430.0f, 120.0f, 90.0f, 28.0f, "Refresh", C_TEXT);
    gc_ui_slider(a->ui, (GCRECT){520.0f, 122.0f, 160.0f, 24.0f}, 0.2f, 100.0f, &a->refresh);

    if (app_button(a, 750.0f, 120.0f, 100.0f, 28.0f, "Max")) {
        a->refresh = 100.0f;
    }

    app_textf(a, 688.0f, 120.0f, 80.0f, 28.0f, C_DIM, "%.1fs", (f64)a->refresh);

    app_textf(a, 1140.0f, 120.0f, 100.0f, 28.0f, C_DIM, "shown %d", nshow);
}

static void app_sortbar(App *a)
{
    for (i32 c = 0; c < COL_N; c++) {
        char bl[16];
        if (col_keys[c] == a->sort_key) {
            snprintf(bl, sizeof(bl), "%s %s", col_names[c], a->sort_desc ? "v" : "^");
        } else {
            snprintf(bl, sizeof(bl), "%s", col_names[c]);
        }
        if (app_button(a, col_x[c], 168.0f, col_w[c], 32.0f, bl)) {
            app_pick_sort(a, col_keys[c]);
        }
    }
}

static void app_row(App *a, Proc *p, f32 y)
{
    GCRECT row    = {24.0f, y, 1232.0f, ROW_H};
    GCB32  inside = gc_rect_contains(row, a->ui->mouseX, a->ui->mouseY);

    if (p->pid == a->sel) {
        gc_draw_fill(a->window, row, C_SEL);
    } else if (inside) {
        gc_draw_outline(a->window, row, C_DIM);
    }
    if (inside && a->ui->mousePressed) {
        a->sel = p->pid;
    }

    app_textf(a, col_x[0], y, col_w[0], ROW_H, C_TEXT, "%d", p->pid);
    app_textf(a, col_x[1], y, col_w[1], ROW_H, C_TEXT, "%.1f", (f64)p->cpu_pct);
    app_textf(a, col_x[2], y, col_w[2], ROW_H, C_TEXT, "%.1fM", (f64)p->rss_kb / 1024.0);
    app_label(a, col_x[3], y, col_w[3], ROW_H, p->cmd, C_DIM);
}

static void app_body(App *a, i32 nshow, f32 body_y, f32 body_h)
{
    gc_ui_panel(a->ui, (GCRECT){16.0f, body_y, 1248.0f, body_h}, C_BODY);

    i32 vis        = (i32)(body_h / ROW_H);
    i32 max_first  = (nshow > vis) ? nshow - vis : 0;
    f32 max_scroll = (f32)max_first * ROW_H;

    app_clamp_scroll(a, max_scroll);
    if (gc_window_key_was_pressed(a->window, GC_KEY_PAGEDOWN)) {
        a->scroll += (f32)vis * ROW_H;
    }
    if (gc_window_key_was_pressed(a->window, GC_KEY_PAGEUP)) {
        a->scroll -= (f32)vis * ROW_H;
    }
    if (gc_window_key_was_pressed(a->window, GC_KEY_HOME)) {
        a->scroll = 0.0f;
    }
    if (gc_window_key_was_pressed(a->window, GC_KEY_END)) {
        a->scroll = max_scroll;
    }

    app_clamp_scroll(a, max_scroll);

    i32 first = (i32)(a->scroll / ROW_H);
    if (first > max_first) {
        first = max_first;
    }

    i32 last = first + vis;
    if (last > nshow) {
        last = nshow;
    }

    for (i32 vi = first; vi < last; vi++) {
        app_row(a, &a->procs[a->order[vi]], body_y + (f32)(vi - first) * ROW_H);
    }

    gc_ui_slider(a->ui, (GCRECT){24.0f, 620.0f, 1232.0f, 20.0f}, 0.0f, max_scroll, &a->scroll);
}

static void app_footer(App *a)
{
    gc_ui_panel(a->ui, (GCRECT){16.0f, 648.0f, 1248.0f, 56.0f}, C_PANEL);

    app_textf(a, 32.0f, 658.0f, 700.0f, 28.0f, C_TEXT, "sel %d %s", a->sel, app_sel_cmd(a));

    if (app_button(a, 760.0f, 656.0f, 120.0f, 40.0f, "Term")) {
        app_signal_sel(a, SIGTERM, "TERM");
    }
    if (app_button(a, 888.0f, 656.0f, 120.0f, 40.0f, "Kill")) {
        app_signal_sel(a, SIGKILL, "KILL");
    }

    app_label(a, 1016.0f, 658.0f, 240.0f, 28.0f, a->status, C_DIM);
}

static b32 app_init(App *a)
{
    a->sort_key  = PROC_SORT_CPU;
    a->sort_desc = true;
    a->sel       = -1;
    a->refresh   = 0.5f;
    a->timer     = 1e9f;
    snprintf(a->status, sizeof(a->status), "ready");

    a->arena  = gc_arena_create(GC_MB(2));
    a->window = gc_window_create(a->arena, "gproc", WIN_W, WIN_H, GC_FALSE);
    if (!a->window) {
        gc_arena_destroy(a->arena);
        return false;
    }
    a->ui = gc_ui_create(a->arena, a->window);
    gc_ui_load_default_font(a->ui, 18.0f);

    a->filter = (char *)gc_arena_push(a->arena, 256);
    memset(a->filter, 0, 256);
    a->filter_cap = 256;
    return true;
}

static void app_frame(App *a)
{
    gc_window_begin(a->window);
    gc_ui_begin(a->ui);

    f32 dt = gc_window_dt(a->window);
    app_resnapshot(a, dt);
    i32 nshow = app_filter(a);

    app_header(a, nshow);
    app_toolbar(a, nshow);
    app_sortbar(a);
    app_body(a, nshow, 208.0f, 408.0f);
    app_footer(a);

    gc_ui_end(a->ui);
    gc_window_end(a->window);
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
#else
int main(void)
{
#endif
    if (!app_init(&app)) {
        return 1;
    }
    while (!gc_window_should_quit(app.window)) {
        if (gc_window_key_was_pressed(app.window, GC_KEY_ESCAPE)) {
            break;
        }
        app_frame(&app);
    }
    gc_ui_destroy(app.ui);
    gc_window_destroy(app.window);
    gc_arena_destroy(app.arena);
    return 0;
}
