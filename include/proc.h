#ifndef PROC_H
#define PROC_H

#include <stdint.h>
#include <stddef.h>

typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef size_t usize;

typedef float  f32;
typedef double f64;

typedef i32 b32;
#define true  1
#define false 0

#define header_implementation static inline

#define PROC_MAX      2048
#define PROC_NAME_MAX 32
#define PROC_CMD_MAX  128

#define PROC_SORT_PID  0
#define PROC_SORT_CPU  1
#define PROC_SORT_MEM  2
#define PROC_SORT_NAME 3

typedef struct {
    i32  pid;
    i32  ppid;
    char state;
    u64  cpu_time;
    f32  cpu_pct;
    u64  rss_kb;
    char name[PROC_NAME_MAX];
    char cmd[PROC_CMD_MAX];
} Proc;

typedef struct {
    f32 cpu_pct;
    u64 mem_total_kb;
    u64 mem_avail_kb;
    f32 mem_pct;
    f64 uptime_s;
} SysInfo;

header_implementation b32  proc_snapshot(Proc *procs, i32 *count, SysInfo *sys);
header_implementation void proc_sort(Proc *procs, i32 count, i32 key, b32 desc);
header_implementation b32  proc_match(const Proc *p, const char *filter);
header_implementation b32  proc_kill(i32 pid, i32 sig);

#ifdef GPROC_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

#if !defined(__linux__)
#error "gproc v1 is Linux-only (/proc)"
#endif

typedef struct {
    i32 pid;
    u64 t;
} Proc__Prev;

static i32 proc__key  = PROC_SORT_CPU;
static b32 proc__desc = true;

header_implementation u64 proc__cpu_total(u64 *idle_out)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
        return 0;
    }
    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    unsigned long v[10] = {0};
    int n = sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4) {
        return 0;
    }
    u64 total = 0;
    for (i32 i = 0; i < (i32)n; i++) {
        total += (u64)v[i];
    }
    *idle_out = (u64)v[3] + (u64)v[4];
    return total;
}

header_implementation b32 proc__is_pid(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    while (*s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        s++;
    }
    return true;
}

header_implementation b32 proc__contains_ci(const char *hay, const char *ndl)
{
    if (*ndl == '\0') {
        return true;
    }
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = ndl;
        while (*n && *h && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

header_implementation i32 proc__cmp(const void *a, const void *b)
{
    const Proc *x = (const Proc *)a;
    const Proc *y = (const Proc *)b;
    i32 r = 0;
    if (proc__key == PROC_SORT_CPU) {
        r = (x->cpu_pct > y->cpu_pct) - (x->cpu_pct < y->cpu_pct);
    } else if (proc__key == PROC_SORT_MEM) {
        r = (x->rss_kb > y->rss_kb) - (x->rss_kb < y->rss_kb);
    } else if (proc__key == PROC_SORT_NAME) {
        r = (i32)strcmp(x->name, y->name);
    } else {
        r = (x->pid > y->pid) - (x->pid < y->pid);
    }
    return proc__desc ? -r : r;
}

header_implementation b32 proc_snapshot(Proc *procs, i32 *count, SysInfo *sys)
{
    if (!procs || !count || !sys) {
        return false;
    }

    static u64 last_total = 0;
    static u64 last_idle = 0;
    static Proc__Prev prev[PROC_MAX];
    static i32 nprev = 0;

    u64 idle = 0;
    u64 total = proc__cpu_total(&idle);
    u64 dtotal = (total > last_total) ? total - last_total : 0;
    u64 didle = (idle > last_idle) ? idle - last_idle : 0;
    b32 first = (last_total == 0);

    sys->cpu_pct = 0.0f;
    if (!first && dtotal > 0) {
        f32 busy = (f32)(dtotal - didle) / (f32)dtotal;
        if (busy < 0.0f) {
            busy = 0.0f;
        }
        if (busy > 1.0f) {
            busy = 1.0f;
        }
        sys->cpu_pct = busy * 100.0f;
    }

    /* NOTE: libc file io here; gcui already links libc/SDL, so no raw-syscall layer. */
    sys->mem_total_kb = 0;
    sys->mem_avail_kb = 0;
    FILE *mf = fopen("/proc/meminfo", "r");
    if (mf) {
        char line[256];
        while (fgets(line, sizeof(line), mf)) {
            unsigned long kb = 0;
            if (sscanf(line, "MemTotal: %lu", &kb) == 1) {
                sys->mem_total_kb = (u64)kb;
            }
            if (sscanf(line, "MemAvailable: %lu", &kb) == 1) {
                sys->mem_avail_kb = (u64)kb;
            }
        }
        fclose(mf);
    }
    u64 used = (sys->mem_total_kb > sys->mem_avail_kb) ? sys->mem_total_kb - sys->mem_avail_kb : 0;
    sys->mem_pct = sys->mem_total_kb ? 100.0f * (f32)used / (f32)sys->mem_total_kb : 0.0f;

    sys->uptime_s = 0.0;
    FILE *uf = fopen("/proc/uptime", "r");
    if (uf) {
        double up = 0.0;
        if (fscanf(uf, "%lf", &up) == 1) {
            sys->uptime_s = up;
        }
        fclose(uf);
    }

    static long page_kb = 0;
    if (page_kb == 0) {
        long pg = sysconf(_SC_PAGESIZE);
        page_kb = (pg > 0) ? pg / 1024 : 4;
    }

    DIR *d = opendir("/proc");
    if (!d) {
        return false;
    }
    i32 n = 0;
    char path[64];
    char buf[640];
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < PROC_MAX) {
        if (!proc__is_pid(e->d_name)) {
            continue;
        }
        i32 pid = (i32)atoi(e->d_name);
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *sf = fopen(path, "r");
        if (!sf) {
            continue;
        }
        usize len = fread(buf, 1, sizeof(buf) - 1, sf);
        fclose(sf);
        if (len == 0) {
            continue;
        }
        buf[len] = '\0';
        char *lp = strchr(buf, '(');
        char *rp = strrchr(buf, ')');
        if (!lp || !rp || rp <= lp + 1) {
            continue;
        }
        Proc *p = &procs[n];
        usize cn = (usize)(rp - lp - 1);
        if (cn > (usize)(PROC_NAME_MAX - 1)) {
            cn = (usize)(PROC_NAME_MAX - 1);
        }
        memcpy(p->name, lp + 1, cn);
        p->name[cn] = '\0';
        char state = 0;
        int ppid = 0;
        unsigned long ut = 0;
        unsigned long st = 0;
        long rss_pg = 0;
        int m = sscanf(rp + 1, " %c %d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu %*ld %*ld %*d %*d %*d %*d %*llu %*lu %ld",
            &state, &ppid, &ut, &st, &rss_pg);
        if (m != 5) {
            continue;
        }
        p->pid = pid;
        p->ppid = (i32)ppid;
        p->state = state;
        p->cpu_time = (u64)ut + (u64)st;
        p->rss_kb = (rss_pg > 0) ? (u64)rss_pg * (u64)page_kb : 0;
        p->cpu_pct = 0.0f;
        if (!first && dtotal > 0) {
            for (i32 i = 0; i < nprev; i++) {
                if (prev[i].pid == pid) {
                    u64 dt = (p->cpu_time > prev[i].t) ? p->cpu_time - prev[i].t : 0;
                    p->cpu_pct = 100.0f * (f32)dt / (f32)dtotal;
                    break;
                }
            }
        }
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE *cf = fopen(path, "rb");
        if (!cf) {
            snprintf(p->cmd, sizeof(p->cmd), "%s", p->name);
        } else {
            usize cl = fread(p->cmd, 1, (usize)(PROC_CMD_MAX - 1), cf);
            fclose(cf);
            if (cl == 0) {
                snprintf(p->cmd, sizeof(p->cmd), "%s", p->name);
            } else {
                for (usize i = 0; i < cl; i++) {
                    if (p->cmd[i] == '\0') {
                        p->cmd[i] = ' ';
                    }
                }
                while (cl > 0 && p->cmd[cl - 1] == ' ') {
                    cl--;
                }
                p->cmd[cl] = '\0';
                if (p->cmd[0] == '\0') {
                    snprintf(p->cmd, sizeof(p->cmd), "%s", p->name);
                }
            }
        }
        n++;
    }
    closedir(d);

    for (i32 i = 0; i < n; i++) {
        prev[i].pid = procs[i].pid;
        prev[i].t = procs[i].cpu_time;
    }
    nprev = n;
    last_total = total;
    last_idle = idle;
    *count = n;
    return true;
}

header_implementation void proc_sort(Proc *procs, i32 count, i32 key, b32 desc)
{
    if (!procs || count < 2) {
        return;
    }
    if (key < PROC_SORT_PID || key > PROC_SORT_NAME) {
        key = PROC_SORT_PID;
    }
    proc__key = key;
    proc__desc = desc;
    qsort(procs, (usize)count, sizeof(Proc), proc__cmp);
}

header_implementation b32 proc_match(const Proc *p, const char *filter)
{
    if (!p) {
        return false;
    }
    if (!filter || *filter == '\0') {
        return true;
    }
    return proc__contains_ci(p->name, filter) || proc__contains_ci(p->cmd, filter);
}

header_implementation b32 proc_kill(i32 pid, i32 sig)
{
    if (pid <= 0 || sig <= 0) {
        return false;
    }
    return kill((pid_t)pid, sig) == 0;
}

#endif
#endif
