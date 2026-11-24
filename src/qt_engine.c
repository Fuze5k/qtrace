/*
 * qtrace engine: kernel trace events -> per-thread kernel-call records.
 *
 * How the trace is read (QNX SDP 8.0 System Analysis Toolkit):
 *  - Kernel-call entry/exit events carry no pid/tid. They belong to the thread
 *    that the last THRUNNING event put on that CPU, so the engine tracks the
 *    running thread per CPU.
 *  - A MsgSend() is followed, on the client's CPU, by COMM_SMSG (rcvid, server
 *    pid). The server's MsgReceive() logs COMM_RMSG with the same rcvid, and
 *    its MsgReply() logs COMM_REPLY (client tid, client pid). Joining the three
 *    gives, for every POSIX call, which server handled it, how long the client
 *    waited for the server to pick the message up and how long the server
 *    worked on it, and at which priority the server ran (priority
 *    inheritance makes it run at the client's priority).
 *  - SYS_PATHMGR (pid, tid, path) is logged when procnto resolves a path for
 *    open(), stat(), unlink() ... and names the path of the io_connect message
 *    that follows.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "qt_engine.h"
#include "qt_msg.h"

#define MAX_CPUS 64

/* ---------- small open-addressing hash map: uint64 key -> int value ---------- */

typedef struct { uint64_t *k; int *v; unsigned char *used; size_t cap, n; } map_t;

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static int map_get(const map_t *m, uint64_t key, int *val)
{
    size_t i;
    if (!m->cap) return 0;
    for (i = mix(key) & (m->cap - 1); m->used[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == key) { if (val) *val = m->v[i]; return 1; }
    return 0;
}

static void map_put(map_t *m, uint64_t key, int val);

static void map_grow(map_t *m)
{
    map_t n = { 0 };
    size_t i;
    n.cap = m->cap ? m->cap * 2 : 256;
    n.k = calloc(n.cap, sizeof *n.k);
    n.v = calloc(n.cap, sizeof *n.v);
    n.used = calloc(n.cap, 1);
    if (!n.k || !n.v || !n.used) { perror("qtrace"); exit(1); }
    for (i = 0; i < m->cap; i++)
        if (m->used[i]) map_put(&n, m->k[i], m->v[i]);
    free(m->k); free(m->v); free(m->used);
    *m = n;
}

static void map_put(map_t *m, uint64_t key, int val)
{
    size_t i;
    if ((m->n + 1) * 4 >= m->cap * 3) map_grow(m);
    for (i = mix(key) & (m->cap - 1); m->used[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == key) { m->v[i] = val; return; }
    m->used[i] = 1; m->k[i] = key; m->v[i] = val; m->n++;
}

static void map_del(map_t *m, uint64_t key)
{
    /* Deletion by tombstone-free backward shift. */
    size_t i, j, h;
    if (!m->cap) return;
    for (i = mix(key) & (m->cap - 1); m->used[i]; i = (i + 1) & (m->cap - 1))
        if (m->k[i] == key) break;
    if (!m->used[i]) return;
    m->used[i] = 0; m->n--;
    for (j = (i + 1) & (m->cap - 1); m->used[j]; j = (j + 1) & (m->cap - 1)) {
        h = mix(m->k[j]) & (m->cap - 1);
        if ((j > i && (h <= i || h > j)) || (j < i && (h <= i && h > j))) {
            m->k[i] = m->k[j]; m->v[i] = m->v[j]; m->used[i] = 1; m->used[j] = 0; i = j;
        }
    }
}

static void map_free(map_t *m) { free(m->k); free(m->v); free(m->used); memset(m, 0, sizeof *m); }

#define KEY2(a, b) (((uint64_t)(uint32_t)(a) << 32) | (uint32_t)(b))

/* ---------- engine state ---------- */

typedef struct { int pid, ppid, traced, alive; char name[48]; } proc_t;
typedef struct {
    int pid, tid, prio;
    int cur;            /* pending call (index into calls), -1 if none */
    int serving;        /* client call being served, -1 if none */
    char *path;         /* last path resolved by procnto for this thread */
    char name[32];
} thr_t;

struct qt_engine {
    qt_opts_t opts;
    proc_t *procs; size_t nprocs, capprocs;
    thr_t  *thrs;  size_t nthrs,  capthrs;
    qt_call_t *calls; size_t ncalls, capcalls;
    map_t pmap, tmap, conns, rcvids;
    int cpu_cur[MAX_CPUS];                 /* running thread index per CPU */
    const qt_kdesc_t *kd[QT_K__COUNT][2][2]; /* [kcall][exit][is64] */
    uint64_t t0;
    unsigned long nevents, dropped;
    int finished;
};

static proc_t *proc_get(qt_engine_t *e, int pid, int create)
{
    int i;
    if (map_get(&e->pmap, (uint64_t)(uint32_t)pid, &i)) return &e->procs[i];
    if (!create) return NULL;
    if (e->nprocs == e->capprocs) {
        e->capprocs = e->capprocs ? e->capprocs * 2 : 128;
        e->procs = realloc(e->procs, e->capprocs * sizeof *e->procs);
        if (!e->procs) { perror("qtrace"); exit(1); }
    }
    i = (int)e->nprocs++;
    memset(&e->procs[i], 0, sizeof e->procs[i]);
    e->procs[i].pid = pid;
    e->procs[i].alive = 1;
    e->procs[i].traced = e->opts.root_pid == 0 || pid == e->opts.root_pid;
    map_put(&e->pmap, (uint64_t)(uint32_t)pid, i);
    return &e->procs[i];
}

static int thr_index(qt_engine_t *e, int pid, int tid)
{
    int i;
    if (map_get(&e->tmap, KEY2(pid, tid), &i)) return i;
    if (e->nthrs == e->capthrs) {
        e->capthrs = e->capthrs ? e->capthrs * 2 : 256;
        e->thrs = realloc(e->thrs, e->capthrs * sizeof *e->thrs);
        if (!e->thrs) { perror("qtrace"); exit(1); }
    }
    i = (int)e->nthrs++;
    memset(&e->thrs[i], 0, sizeof e->thrs[i]);
    e->thrs[i].pid = pid; e->thrs[i].tid = tid;
    e->thrs[i].prio = -1; e->thrs[i].cur = -1; e->thrs[i].serving = -1;
    map_put(&e->tmap, KEY2(pid, tid), i);
    proc_get(e, pid, 1);
    return i;
}

static int traced(qt_engine_t *e, int pid)
{
    proc_t *p = proc_get(e, pid, 0);
    if (p) return p->traced;
    return e->opts.root_pid == 0 || pid == e->opts.root_pid;
}

static int new_call(qt_engine_t *e)
{
    qt_call_t *c;
    if (e->ncalls == e->capcalls) {
        e->capcalls = e->capcalls ? e->capcalls * 2 : 4096;
        e->calls = realloc(e->calls, e->capcalls * sizeof *e->calls);
        if (!e->calls) { perror("qtrace"); exit(1); }
    }
    c = &e->calls[e->ncalls];
    memset(c, 0, sizeof *c);
    c->prio = c->srv_pid = c->srv_tid = c->srv_prio = -1;
    c->coid = -1; c->parent = -1;
    return (int)e->ncalls++;
}

/* ---------- argument decoding ---------- */

static unsigned layout_words(const qt_arg_t *a, unsigned n)
{
    unsigned i, w = 0;
    for (i = 0; i < n; i++) w += a[i].words;
    return w;
}

/* Find argument `name` in the event's data. Wide layout is used when the event
 * is long enough to be wide, fast layout otherwise. */
static int kd_arg(const qt_kdesc_t *kd, const uint32_t *d, unsigned n,
                  const char *name, uint64_t *val)
{
    const qt_arg_t *a; unsigned na, i, off = 0;
    if (!kd) return 0;
    if (n >= layout_words(kd->wide, kd->nwide) || n > layout_words(kd->fast, kd->nfast)) {
        a = kd->wide; na = kd->nwide;
    } else {
        a = kd->fast; na = kd->nfast;
    }
    for (i = 0; i < na; i++) {
        if (strcmp(a[i].name, name) == 0) {
            if (off + a[i].words > n) return 0;
            *val = d[off];
            if (a[i].words == 2) *val |= (uint64_t)d[off + 1] << 32;
            return 1;
        }
        off += a[i].words;
    }
    return 0;
}

static int ev_arg(int id, const uint32_t *d, unsigned n, const char *name, uint64_t *val)
{
    const qt_edesc_t *ed = &qt_edesc[id];
    const qt_arg_t *a = n >= layout_words(ed->wide, ed->nwide) ? ed->wide : ed->fast;
    unsigned na = a == ed->wide ? ed->nwide : ed->nfast, i, off = 0;
    for (i = 0; i < na; i++) {
        if (strcmp(a[i].name, name) == 0) {
            if (off + a[i].words > n) return 0;
            *val = d[off];
            if (a[i].words == 2) *val |= (uint64_t)d[off + 1] << 32;
            return 1;
        }
        off += a[i].words;
    }
    return 0;
}

/* Strings in event data are packed little-endian into 32-bit words. */
static void ev_string(const uint32_t *d, unsigned n, unsigned from, char *buf, size_t len)
{
    size_t o = 0; unsigned i, b;
    for (i = from; i < n && o + 1 < len; i++)
        for (b = 0; b < 4 && o + 1 < len; b++) {
            char ch = (char)((d[i] >> (8 * b)) & 0xff);
            if (!ch) goto out;
            buf[o++] = ch;
        }
out:
    buf[o] = 0;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Process and thread names are for display only: cut a long one to fit. */
static void copy_name(char *dst, size_t size, const char *src)
{
    size_t len = 0;
    while (len + 1 < size && src[len]) len++;
    memcpy(dst, src, len);
    dst[len] = 0;
}

/* ---------- output ---------- */

static int is_msgsend(int k)
{
    return k == QT_K_MSG_SENDV || k == QT_K_MSG_SENDVNC;
}

static int is_pulse(int k)
{
    return k == QT_K_MSG_SEND_PULSE || k == QT_K_MSG_SEND_PULSEPTR;
}

double qt_engine_us(const qt_engine_t *e, uint64_t cycles)
{
    if (!e->opts.cycles_per_sec) return (double)cycles;
    return (double)cycles * 1e6 / (double)e->opts.cycles_per_sec;
}

static void fmt_fd(char *buf, size_t len, int coid)
{
    if ((uint32_t)coid >= QT_SIDE_CHANNEL)
        snprintf(buf, len, "coid 0x%x", (unsigned)coid);
    else
        snprintf(buf, len, "fd %d", coid);
}

#define QT_PROCNTO_PID 1

/* Name of the message a MsgSend() carries. Types below 0x100 are procnto's own
 * messages only when procnto gets them: a server with its own protocol may use
 * the same numbers. */
static const char *msg_name(const qt_call_t *c)
{
    if ((c->msg[0] & 0xffff) >= 0x100) return qt_msg_name(c->msg[0]);
    if (c->srv_pid == QT_PROCNTO_PID || (uint32_t)c->coid == QT_SIDE_CHANNEL) return qt_sysmsg_name(c->msg[0]);
    return NULL;
}

static int is_connect(const qt_call_t *c)
{
    return is_msgsend(c->k) && c->nmsg && (c->msg[0] & 0xffff) == QT_IO_CONNECT;
}

/* A connect that procnto's path manager answers with "ask these servers" returns a
 * status with _IO_CONNECT_RET_FLAG set, e.g. 0x800b0000 (RET_LINK | RET_UMASK |
 * RET_CHROOT), not a file descriptor. */
static int is_redirect(const qt_call_t *c)
{
    return is_connect(c) && !c->err && ((uint64_t)c->ret & 0x80000000u);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d) { perror("qtrace"); exit(1); }
    return memcpy(d, s, n);
}

const char *qt_call_result(const qt_call_t *c, char *buf, size_t len)
{
    if (!c->done) snprintf(buf, len, "? <unfinished>");
    else if (c->intr) snprintf(buf, len, "-1 EINTR");
    else if (c->err) snprintf(buf, len, "-1 errno %d (%s)", c->err, strerror(c->err));
    else if (is_redirect(c)) snprintf(buf, len, "redirect 0x%" PRIx64, (uint64_t)c->ret);
    else snprintf(buf, len, "%" PRId64, c->ret);
    return buf;
}

const char *qt_call_describe(const qt_engine_t *e, const qt_call_t *c, char *buf, size_t len)
{
    char fd[32], tmp[64];
    const char *what;
    unsigned i, nshown = 0;
    size_t o;

    if (is_msgsend(c->k) && c->nmsg) {
        uint32_t m0 = c->msg[0];
        uint16_t type = (uint16_t)(m0 & 0xffff);
        fmt_fd(fd, sizeof fd, c->coid);
        what = msg_name(c);
        if (type == QT_IO_CONNECT) {
            const char *ft = c->nmsg >= 2 ? qt_ftype_name(c->msg[1]) : NULL;
            snprintf(buf, len, "%s(\"%s\"%s%s) on %s", what, c->path ? c->path : "?",
                     ft ? ", " : "", ft ? ft : "", fd);
        } else if ((type == QT_IO_READ || type == QT_IO_WRITE) && c->nmsg >= 2) {
            snprintf(buf, len, "%s(%s, %" PRIu32 " bytes)", what, fd, c->msg[1]);
        } else if (type == QT_IO_DEVCTL && c->nmsg >= 2) {
            qt_dcmd_describe(c->msg[1], tmp, sizeof tmp);
            snprintf(buf, len, "devctl(%s, %s)", fd, tmp);
        } else if (type == QT_IO_LSEEK && c->nmsg >= 3) {
            static const char *const wh[] = { "SEEK_SET", "SEEK_CUR", "SEEK_END" };
            unsigned w = c->msg[1] & 0xffff;
            snprintf(buf, len, "lseek(%s, %" PRIu32 ", %s)", fd, c->msg[2], w < 3 ? wh[w] : "?");
        } else if (what && type < 0x100) {
            snprintf(buf, len, "%s()", what);   /* procnto's own: the server column says where */
        } else if (what) {
            snprintf(buf, len, "%s(%s)", what, fd);
        } else if (type < 0x100) {
            snprintf(buf, len, "MsgSend(%s, %smsg 0x%04x)", fd,
                     c->srv_pid == QT_PROCNTO_PID ? "procnto " : "", (unsigned)type);
        } else {
            snprintf(buf, len, "MsgSend(%s, msg 0x%04x/0x%04x)", fd, (unsigned)type, (unsigned)(m0 >> 16));
        }
        return buf;
    }
    if (is_pulse(c->k)) {
        uint64_t code = 0;
        fmt_fd(fd, sizeof fd, c->coid);
        kd_arg(e->kd[c->k][0][c->is64], c->a, c->na, "code", &code);
        snprintf(buf, len, "%s(%s, code %d)", qt_kcall_func[c->k], fd, (int)(int8_t)code);
        return buf;
    }

    /* Generic kernel call: name(arg=value, ...) from the event table. */
    o = (size_t)snprintf(buf, len, "%s(", qt_kcall_func[c->k]);
    {
        const qt_kdesc_t *kd = e->kd[c->k][0][c->is64];
        if (kd) {
            unsigned off = 0;
            for (i = 0; i < kd->nwide && off < c->na && nshown < 4 && o < len; i++) {
                const qt_arg_t *a = &kd->wide[i];
                uint64_t v = c->a[off];
                if (a->words == 2 && off + 1 < c->na) v |= (uint64_t)c->a[off + 1] << 32;
                off += a->words;
                if (!strcmp(a->name, "empty") || !strcmp(a->name, "pad")) continue;
                if (!strcmp(a->name, "pid") && v)
                    o += (size_t)snprintf(buf + o, len - o, "%spid=%d (%s)", nshown ? ", " : "",
                                          (int)v, qt_engine_pname(e, (int)v));
                else if (!strcmp(a->name, "coid") && v >= QT_SIDE_CHANNEL && v <= 0xffffffffu)
                    o += (size_t)snprintf(buf + o, len - o, "%scoid=0x%x", nshown ? ", " : "", (unsigned)v);
                else if (v < 0x10000 || strstr(a->name, "tid") || !strcmp(a->name, "coid"))
                    o += (size_t)snprintf(buf + o, len - o, "%s%s=%" PRId64, nshown ? ", " : "",
                                          a->name, a->words == 1 ? (int64_t)(int32_t)v : (int64_t)v);
                else
                    o += (size_t)snprintf(buf + o, len - o, "%s%s=0x%" PRIx64, nshown ? ", " : "", a->name, v);
                nshown++;
            }
        }
    }
    if (o < len) snprintf(buf + o, len - o, ")");
    return buf;
}

static void print_call(qt_engine_t *e, const qt_call_t *c)
{
    char desc[256], ret[48], srv[160] = "";
    FILE *f = e->opts.out;
    const char *pn;
    double rel;

    if (!f) return;
    if (c->parent >= 0 && !e->opts.verbose) return;
    pn = qt_engine_pname(e, c->pid);
    rel = qt_engine_us(e, (c->t0 ? c->t0 : c->t1) - e->t0) / 1e6;
    qt_call_describe(e, c, desc, sizeof desc);

    qt_call_result(c, ret, sizeof ret);

    if (c->srv_pid >= 0) {
        size_t o = (size_t)snprintf(srv, sizeof srv, "  -> %s[%d]", qt_engine_pname(e, c->srv_pid), c->srv_pid);
        if (c->t_recv && c->t_reply && c->t0 && c->t_recv >= c->t0 && c->t_reply >= c->t_recv)
            o += (size_t)snprintf(srv + o, sizeof srv - o, " queued %.1fus, served %.1fus",
                                  qt_engine_us(e, c->t_recv - c->t0), qt_engine_us(e, c->t_reply - c->t_recv));
        if (c->srv_prio >= 0 && o < sizeof srv)
            snprintf(srv + o, sizeof srv - o, ", prio %d", c->srv_prio);
    }

    fprintf(f, "%10.6f %s[%d:%d]%s %s = %s", rel, pn, c->pid, c->tid,
            c->parent >= 0 ? " (serving)" : "", desc, ret);
    if (c->t0 && c->t1)
        fprintf(f, " <%.1fus>", qt_engine_us(e, c->t1 - c->t0));
    fprintf(f, "%s\n", srv);
}

/* ---------- event handling ---------- */

static thr_t *cpu_thread(qt_engine_t *e, unsigned cpu)
{
    if (cpu >= MAX_CPUS || e->cpu_cur[cpu] < 0) return NULL;
    return &e->thrs[e->cpu_cur[cpu]];
}

static int records(qt_engine_t *e, thr_t *t)
{
    return traced(e, t->pid) || (e->opts.servers && t->serving >= 0);
}

static void finish_call(qt_engine_t *e, thr_t *t)
{
    qt_call_t *c = &e->calls[t->cur];
    t->cur = -1;
    if (c->rcvid && c->srv_pid >= 0) map_del(&e->rcvids, KEY2(c->srv_pid, c->rcvid));
}

static void on_kenter(qt_engine_t *e, const qt_ev_t *ev)
{
    thr_t *t = cpu_thread(e, ev->cpu);
    qt_call_t *c;
    const qt_kdesc_t *kd = e->kd[ev->id][0][ev->is64 ? 1 : 0];
    uint64_t v;
    unsigned i;
    int idx;

    if (!t || !records(e, t)) return;
    if (t->cur >= 0) finish_call(e, t);      /* exit was never logged */

    idx = new_call(e);
    c = &e->calls[idx];
    c->t0 = ev->ts; c->pid = t->pid; c->tid = t->tid; c->k = ev->id;
    c->prio = t->prio; c->is64 = (unsigned char)(ev->is64 != 0);
    c->parent = traced(e, t->pid) ? -1 : t->serving;
    c->na = ev->n < 8 ? ev->n : 8;
    for (i = 0; i < c->na; i++) c->a[i] = ev->d[i];
    t->cur = idx;

    if (is_msgsend(ev->id) || is_pulse(ev->id)) {
        if (kd_arg(kd, ev->d, ev->n, "coid", &v)) {
            int srv;
            c->coid = (int)v;
            if (map_get(&e->conns, KEY2(t->pid, c->coid), &srv)) c->srv_pid = srv;
        }
        for (i = 0; i < 3; i++) {
            char nm[8];
            snprintf(nm, sizeof nm, "msg[%u]", i);
            if (kd_arg(kd, ev->d, ev->n, nm, &v)) { c->msg[i] = (uint32_t)v; c->nmsg = i + 1; }
            else if (i == 0 && kd_arg(kd, ev->d, ev->n, "msg", &v)) { c->msg[0] = (uint32_t)v; c->nmsg = 1; }
        }
    }
    /* After procnto's path manager redirects a connect, the C library asks each
     * server on the list in turn - ConnectAttach(), the same io_connect message,
     * ConnectDetach() - so those connects carry the path procnto logged. A new
     * connect to the path manager or any other call ends the walk. */
    if (t->path) {
        if (is_connect(c) && (uint32_t)c->coid != QT_SIDE_CHANNEL)
            c->path = dup_str(t->path);
        else if (ev->id != QT_K_CONNECT_ATTACH && ev->id != QT_K_CONNECT_DETACH)
            free(t->path), t->path = NULL;
    }
}

static void on_kexit(qt_engine_t *e, const qt_ev_t *ev, int interrupted)
{
    thr_t *t = cpu_thread(e, ev->cpu);
    qt_call_t *c;
    const qt_kdesc_t *kd;
    int idx;

    if (!t || !records(e, t)) return;
    if (t->cur >= 0 && e->calls[t->cur].k == ev->id) {
        idx = t->cur;
    } else {
        if (t->cur >= 0) finish_call(e, t);
        idx = new_call(e);                   /* call began before the trace */
        e->calls[idx].pid = t->pid; e->calls[idx].tid = t->tid;
        e->calls[idx].k = ev->id; e->calls[idx].parent = traced(e, t->pid) ? -1 : t->serving;
        t->cur = idx;
    }
    c = &e->calls[idx];
    c->t1 = ev->ts;
    c->done = 1;
    if (interrupted) {
        c->intr = 1;
    } else if (ev->n >= 1) {
        kd = e->kd[ev->id][1][ev->is64 ? 1 : 0];
        if (!kd) kd = e->kd[ev->id][1][0];
        if (ev->d[0] == 0xffffffffu && ev->n >= 2) {
            /* Failed call: return value -1, then errno. */
            c->ret = -1;
            c->err = (int)((ev->is64 && ev->n >= 3 && ev->d[1] == 0xffffffffu) ? ev->d[2] : ev->d[1]);
        } else if (kd && kd->nwide && kd->wide[0].words == 2 && ev->n >= 2) {
            c->ret = (int64_t)((uint64_t)ev->d[0] | (uint64_t)ev->d[1] << 32);
        } else {
            c->ret = (int32_t)ev->d[0];
        }
        if (ev->id == QT_K_CONNECT_ATTACH && !c->err && c->na >= 3) {
            /* entry: empty, pid, chid, ... ; exit: coid */
            uint64_t pid;
            const qt_kdesc_t *ke = e->kd[QT_K_CONNECT_ATTACH][0][c->is64];
            if (kd_arg(ke, c->a, c->na, "pid", &pid) && pid)
                map_put(&e->conns, KEY2(c->pid, (int)c->ret), (int)pid);
        }
        if (ev->id == QT_K_CONNECT_DETACH && c->na >= 1)
            map_del(&e->conns, KEY2(c->pid, (int)c->a[0]));
    }
    finish_call(e, t);
}

static void on_thread(qt_engine_t *e, const qt_ev_t *ev)
{
    int pid, tid, ti;
    thr_t *t;
    if (ev->n < 2) return;
    pid = (int)ev->d[0]; tid = (int)ev->d[1];
    ti = thr_index(e, pid, tid);
    t = &e->thrs[ti];
    if (ev->n >= 3) t->prio = (int)ev->d[2];
    switch (ev->id) {
    case QT_E_THRUNNING:
        if (ev->cpu < MAX_CPUS) e->cpu_cur[ev->cpu] = ti;
        break;
    case QT_E_THDEAD:
    case QT_E_THDESTROY:
        if (t->cur >= 0) finish_call(e, t);
        break;
    default:
        break;
    }
}

static void on_comm(qt_engine_t *e, const qt_ev_t *ev)
{
    thr_t *t = cpu_thread(e, ev->cpu), *client;
    int idx, ti;
    uint64_t v;

    switch (ev->id) {
    case QT_E_COMM_SMSG:     /* rcvid, server pid; on the client's CPU */
    case QT_E_COMM_SPULSE:
        if (!t || t->cur < 0 || ev->n < 2) return;
        {
            qt_call_t *c = &e->calls[t->cur];
            if (!is_msgsend(c->k) && !is_pulse(c->k)) return;
            c->srv_pid = (int)ev->d[1];
            if (c->coid >= 0) map_put(&e->conns, KEY2(c->pid, c->coid), c->srv_pid);
            if (ev->id == QT_E_COMM_SMSG) {
                c->rcvid = ev->d[0];
                map_put(&e->rcvids, KEY2(c->srv_pid, c->rcvid), t->cur);
            }
        }
        break;
    case QT_E_COMM_RMSG:     /* rcvid, pid; on the server's CPU */
        if (ev->n < 2) return;
        if (!map_get(&e->rcvids, KEY2(ev->d[1], ev->d[0]), &idx) &&
            !(t && map_get(&e->rcvids, KEY2(t->pid, ev->d[0]), &idx)))
            return;
        e->calls[idx].t_recv = ev->ts;
        if (t) {
            e->calls[idx].srv_tid = t->tid;
            e->calls[idx].srv_prio = t->prio;
            t->serving = idx;
        }
        break;
    case QT_E_COMM_REPLY:    /* client tid, client pid; on the server's CPU */
    case QT_E_COMM_ERROR:
        if (!ev_arg(ev->id, ev->d, ev->n, "pid", &v) || ev->n < 2) return;
        if (!map_get(&e->tmap, KEY2((int)v, (int)ev->d[0]), &ti)) return;
        client = &e->thrs[ti];
        if (client->cur >= 0) {
            qt_call_t *c = &e->calls[client->cur];
            c->t_reply = ev->ts;
            if (t && t->serving == client->cur) t->serving = -1;
        }
        break;
    default:
        break;
    }
}

static void on_process(qt_engine_t *e, const qt_ev_t *ev)
{
    proc_t *p, *pp;
    char name[160];
    if (ev->n < 2) return;
    switch (ev->id) {
    case QT_E_PROCCREATE:
    case QT_E_PROCCREATE_NAME:
        proc_get(e, (int)ev->d[0], 1);
        p = proc_get(e, (int)ev->d[1], 1);      /* may move e->procs: look pp up after */
        pp = proc_get(e, (int)ev->d[0], 0);
        p->ppid = (int)ev->d[0];
        p->alive = 1;
        if (e->opts.follow && pp->traced) p->traced = 1;
        if (ev->id == QT_E_PROCCREATE_NAME) {
            ev_string(ev->d, ev->n, 2, name, sizeof name);
            copy_name(p->name, sizeof p->name, basename_of(name));
        } else if (!p->name[0]) {
            /* A fork() child, or the short-lived child that loads the new
             * image during exec() (on QNX 8.0 it opens and maps the program,
             * then goes away and the parent takes the program's name). */
            copy_name(p->name, sizeof p->name, pp->name);
        }
        break;
    case QT_E_PROCDESTROY:
        p = proc_get(e, (int)ev->d[1], 1);
        p->alive = 0;
        break;
    case QT_E_PROCTHREAD_NAME:
        ev_string(ev->d, ev->n, 2, name, sizeof name);
        copy_name(e->thrs[thr_index(e, (int)ev->d[0], (int)ev->d[1])].name,
                  sizeof e->thrs[0].name, name);
        break;
    default:
        break;
    }
}

static void on_system(qt_engine_t *e, const qt_ev_t *ev)
{
    char path[512];
    thr_t *t;
    int ti;
    if (ev->id != QT_E_SYS_PATHMGR || ev->n < 3) return;
    if (!map_get(&e->tmap, KEY2((int)ev->d[0], (int)ev->d[1]), &ti)) ti = thr_index(e, (int)ev->d[0], (int)ev->d[1]);
    t = &e->thrs[ti];
    if (!records(e, t)) return;
    ev_string(ev->d, ev->n, 2, path, sizeof path);
    free(t->path);
    t->path = malloc(strlen(path) + 2);
    if (!t->path) { perror("qtrace"); exit(1); }
    snprintf(t->path, strlen(path) + 2, "%s%s", path[0] == '/' ? "" : "/", path);
    /* procnto logs the path while it handles the client's io_connect message;
     * the same path is then sent to the servers that own it (see on_kenter). */
    if (t->cur >= 0) {
        qt_call_t *c = &e->calls[t->cur];
        if (is_connect(c) && !c->path) c->path = dup_str(t->path);
    }
}

/* ---------- public API ---------- */

qt_engine_t *qt_engine_new(const qt_opts_t *opts)
{
    qt_engine_t *e = calloc(1, sizeof *e);
    unsigned i;
    if (!e) return NULL;
    e->opts = *opts;
    for (i = 0; i < MAX_CPUS; i++) e->cpu_cur[i] = -1;
    for (i = 0; i < qt_nkdesc; i++) {
        const qt_kdesc_t *d = &qt_kdesc[i];
        if (!e->kd[d->k][d->exit][d->is64]) e->kd[d->k][d->exit][d->is64] = d;
    }
    for (i = 0; i < QT_K__COUNT; i++) {   /* fall back to the 32-bit layout */
        int x;
        for (x = 0; x < 2; x++) {
            if (!e->kd[i][x][1]) e->kd[i][x][1] = e->kd[i][x][0];
            if (!e->kd[i][x][0]) e->kd[i][x][0] = e->kd[i][x][1];
        }
    }
    if (opts->root_pid) proc_get(e, opts->root_pid, 1)->traced = 1;
    return e;
}

void qt_engine_feed(qt_engine_t *e, const qt_ev_t *ev)
{
    if (!e->nevents++) e->t0 = ev->ts;
    switch (ev->cls) {
    case QT_CLS_KENTER: if (ev->id >= 0 && ev->id < QT_K__COUNT) on_kenter(e, ev); break;
    case QT_CLS_KEXIT:  if (ev->id >= 0 && ev->id < QT_K__COUNT) on_kexit(e, ev, 0); break;
    case QT_CLS_KINT:   if (ev->id >= 0 && ev->id < QT_K__COUNT) on_kexit(e, ev, 1); break;
    case QT_CLS_EVENT:
        if (ev->id < 0 || ev->id >= QT_E__COUNT) break;
        if (!strcmp(qt_edesc[ev->id].cls, "THREAD")) on_thread(e, ev);
        else if (!strcmp(qt_edesc[ev->id].cls, "COMM")) on_comm(e, ev);
        else if (!strcmp(qt_edesc[ev->id].cls, "PROCESS")) on_process(e, ev);
        else if (!strcmp(qt_edesc[ev->id].cls, "SYSTEM")) on_system(e, ev);
        else if (ev->id == QT_E_CONTROLDROPPED && ev->n >= 1) e->dropped += ev->d[0];
        break;
    }
}

static const qt_engine_t *sort_e;

static int by_start(const void *a, const void *b)
{
    const qt_call_t *x = &sort_e->calls[*(const unsigned *)a], *y = &sort_e->calls[*(const unsigned *)b];
    uint64_t tx = x->t0 ? x->t0 : x->t1, ty = y->t0 ? y->t0 : y->t1;
    if (tx != ty) return tx < ty ? -1 : 1;
    return *(const unsigned *)a < *(const unsigned *)b ? -1 : 1;
}

void qt_engine_finish(qt_engine_t *e)
{
    size_t i;
    unsigned *order;
    if (e->finished) return;
    for (i = 0; i < e->nthrs; i++)
        if (e->thrs[i].cur >= 0) finish_call(e, &e->thrs[i]);
    e->finished = 1;
    if (!e->opts.out || !e->ncalls) return;
    /* Print in start order, like strace, now that every call is complete. */
    order = malloc(e->ncalls * sizeof *order);
    if (!order) { perror("qtrace"); exit(1); }
    for (i = 0; i < e->ncalls; i++) order[i] = (unsigned)i;
    sort_e = e;
    qsort(order, e->ncalls, sizeof *order, by_start);
    for (i = 0; i < e->ncalls; i++) print_call(e, &e->calls[order[i]]);
    free(order);
}

void qt_engine_set_cycles_per_sec(qt_engine_t *e, uint64_t cps)
{
    e->opts.cycles_per_sec = cps;
}

void qt_engine_free(qt_engine_t *e)
{
    size_t i;
    if (!e) return;
    for (i = 0; i < e->nthrs; i++) free(e->thrs[i].path);
    for (i = 0; i < e->ncalls; i++) free(e->calls[i].path);
    free(e->procs); free(e->thrs); free(e->calls);
    map_free(&e->pmap); map_free(&e->tmap); map_free(&e->conns); map_free(&e->rcvids);
    free(e);
}

unsigned qt_engine_ncalls(const qt_engine_t *e) { return (unsigned)e->ncalls; }
const qt_call_t *qt_engine_call(const qt_engine_t *e, unsigned i) { return &e->calls[i]; }
uint64_t qt_engine_t0(const qt_engine_t *e) { return e->t0; }
unsigned long qt_engine_nevents(const qt_engine_t *e) { return e->nevents; }
unsigned long qt_engine_dropped(const qt_engine_t *e) { return e->dropped; }

const char *qt_engine_pname(const qt_engine_t *e, int pid)
{
    int i;
    if (pid == 1) return "procnto";
    if (map_get(&e->pmap, (uint64_t)(uint32_t)pid, &i) && e->procs[i].name[0]) return e->procs[i].name;
    return "?";
}

const char *qt_engine_tname(const qt_engine_t *e, int pid, int tid)
{
    int i;
    if (map_get(&e->tmap, KEY2(pid, tid), &i)) return e->thrs[i].name;
    return "";
}

/* ---------- summary (-c) ---------- */

typedef struct { char srv[48], op[40]; unsigned long n, errs; double total, max; } srow_t;

static int srow_cmp(const void *a, const void *b)
{
    const srow_t *x = a, *y = b;
    return x->total < y->total ? 1 : x->total > y->total ? -1 : 0;
}

void qt_engine_summary(const qt_engine_t *e, FILE *f)
{
    srow_t *rows = NULL;
    size_t nrows = 0, cap = 0, i, j;
    double all = 0;

    for (i = 0; i < e->ncalls; i++) {
        const qt_call_t *c = &e->calls[i];
        char op[40], srv[48];
        double us;
        if (!c->done || !c->t0 || c->parent >= 0) continue;
        us = qt_engine_us(e, c->t1 - c->t0);
        if (is_msgsend(c->k) && c->nmsg) {
            const char *n = msg_name(c);
            if (n) snprintf(op, sizeof op, "%s", n);
            else snprintf(op, sizeof op, "msg 0x%04x", (unsigned)(c->msg[0] & 0xffff));
        } else {
            snprintf(op, sizeof op, "%s", qt_kcall_func[c->k]);
        }
        snprintf(srv, sizeof srv, "%s", c->srv_pid >= 0 ? qt_engine_pname(e, c->srv_pid) : "(kernel)");
        for (j = 0; j < nrows; j++)
            if (!strcmp(rows[j].op, op) && !strcmp(rows[j].srv, srv)) break;
        if (j == nrows) {
            if (nrows == cap) {
                cap = cap ? cap * 2 : 64;
                rows = realloc(rows, cap * sizeof *rows);
                if (!rows) { perror("qtrace"); exit(1); }
            }
            memset(&rows[j], 0, sizeof rows[j]);
            snprintf(rows[j].op, sizeof rows[j].op, "%s", op);
            snprintf(rows[j].srv, sizeof rows[j].srv, "%s", srv);
            nrows++;
        }
        rows[j].n++;
        if (c->err || c->intr) rows[j].errs++;
        rows[j].total += us;
        if (us > rows[j].max) rows[j].max = us;
        all += us;
    }
    qsort(rows, nrows, sizeof *rows, srow_cmp);
    fprintf(f, "%% time   total us    calls  errors    avg us    max us  server           operation\n");
    fprintf(f, "------ ---------- -------- ------- --------- ---------  ---------------- ----------------\n");
    for (i = 0; i < nrows; i++)
        fprintf(f, "%6.2f %10.1f %8lu %7lu %9.1f %9.1f  %-16s %s\n",
                all > 0 ? 100.0 * rows[i].total / all : 0.0, rows[i].total, rows[i].n, rows[i].errs,
                rows[i].total / (double)rows[i].n, rows[i].max, rows[i].srv, rows[i].op);
    fprintf(f, "------ ---------- -------- ------- --------- ---------  ---------------- ----------------\n");
    fprintf(f, "100.00 %10.1f                                       total time in kernel calls\n", all);
    free(rows);
}
