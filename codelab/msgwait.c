/*
 * msgwait - run a command on QNX OS 8.0 and print every message it sends:
 * the server that answered, how long the message waited for that server,
 * how long the server worked on it, and the priority the server ran at.
 *
 * The program of the codelab "What is my QNX program waiting for?"
 * (codelab/qnx-message-tracing.md). Run it as root on a target that boots
 * the instrumented kernel, procnto-smp-instr.
 *
 *   qcc -Vgcc_ntoaarch64le -o msgwait msgwait.c -ltraceparser
 *   msgwait command [arg ...]
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/trace.h>
#include <sys/traceparser.h>
#include <sys/wait.h>

#define KEV     "/tmp/msgwait.kev"
#define MAXCPU  64
#define MAXPROC 512
#define MAXMSG  1024

extern char **environ;

/* ---------------------------------------------------------------- capture */

/* tracelogger in daemon mode (-d1) only drains the kernel's trace buffers
 * into the file; this program decides what the kernel records. */
static pid_t start_tracelogger(void)
{
    char *argv[] = { "tracelogger", "-d1", "-w", "-c", "-f", KEV, NULL };
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int rc;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    rc = posix_spawnp(&pid, "tracelogger", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        fprintf(stderr, "msgwait: cannot start tracelogger: %s\n", strerror(rc));
        return -1;
    }
    return pid;
}

/* Thread states (who runs on which CPU), message passing (send, receive,
 * reply) and process events (names), all in wide mode, which adds the
 * thread priority to every THRUNNING event. */
static int arm_kernel(void)
{
    if (TraceEvent(_NTO_TRACE_DELALLCLASSES) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_THREAD) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_COMM) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_PROCESS) == -1 ||
        TraceEvent(_NTO_TRACE_SETALLCLASSESWIDE) == -1)
        return -1;
    return TraceEvent(_NTO_TRACE_START);
}

static void stop_tracelogger(pid_t tl)
{
    TraceEvent(_NTO_TRACE_STOP);   /* also flushes the last, partly filled buffer */
    usleep(300 * 1000);            /* give tracelogger time to write it out */
    kill(tl, SIGINT);
    waitpid(tl, NULL, 0);
}

/* ------------------------------------------------------------------ parse */

typedef struct { int pid, tid, prio; } runner_t;
typedef struct {
    int pid, traced;
    char name[64];
    unsigned msgs;
    double queued_us, served_us;   /* as a server of the traced processes */
} proc_info_t;
typedef struct {
    int srv, rcvid, pid, tid, srv_prio, received;
    unsigned t_send, t_recv;
} flight_t;

static uint64_t cycles_per_sec;
static runner_t running[MAXCPU];
static proc_info_t procs[MAXPROC];
static int nprocs;
static flight_t msgs[MAXMSG];
static int nmsgs;

static proc_info_t *proc(int pid)
{
    int i;
    for (i = 0; i < nprocs; i++)
        if (procs[i].pid == pid) return &procs[i];
    if (nprocs == MAXPROC) { fprintf(stderr, "msgwait: too many processes\n"); exit(1); }
    memset(&procs[nprocs], 0, sizeof procs[0]);
    procs[nprocs].pid = pid;
    snprintf(procs[nprocs].name, sizeof procs[0].name, "pid");
    return &procs[nprocs++];
}

static const char *name_of(int pid) { return proc(pid)->name; }

/* A client thread has at most one message in flight, because MsgSend()
 * blocks, so (pid, tid) finds it; a newer send replaces an older one that
 * never got a reply. Returns NULL for threads that are not traced. */
static flight_t *find_or_add(int pid, int tid)
{
    int i;
    if (!proc(pid)->traced) return NULL;
    for (i = 0; i < nmsgs; i++)
        if (msgs[i].pid == pid && msgs[i].tid == tid) break;
    if (i == nmsgs) {
        if (nmsgs == MAXMSG) return NULL;
        nmsgs++;
    }
    memset(&msgs[i], 0, sizeof msgs[i]);
    msgs[i].pid = pid;
    msgs[i].tid = tid;
    return &msgs[i];
}

static flight_t *find_by_rcvid(int srv, int rcvid)
{
    int i;
    for (i = 0; i < nmsgs; i++)
        if (msgs[i].srv == srv && msgs[i].rcvid == rcvid && !msgs[i].received) return &msgs[i];
    return NULL;
}

static flight_t *find_by_client(int pid, int tid)
{
    int i;
    for (i = 0; i < nmsgs; i++)
        if (msgs[i].pid == pid && msgs[i].tid == tid) return &msgs[i];
    return NULL;
}

static void drop(flight_t *m) { *m = msgs[--nmsgs]; }

static double us(unsigned from, unsigned to)
{
    return (double)(unsigned)(to - from) * 1e6 / (double)cycles_per_sec;
}

/* THRUNNING: pid, tid, priority, policy (wide mode). Message events do not
 * say which thread logged them: it is the one running on that CPU. */
static int on_running(struct traceparser_state *tp, void *ud, unsigned header, unsigned time,
                      unsigned *d, unsigned n)
{
    unsigned cpu = _NTO_TRACE_GETCPU(header);
    if (cpu < MAXCPU && n >= 3) {
        running[cpu].pid = (int)d[0];
        running[cpu].tid = (int)d[1];
        running[cpu].prio = (int)d[2];
    }
    return 0;
}

/* PROCCREATE: ppid, pid. PROCCREATE_NAME: ppid, pid, path. When tracing
 * starts the kernel emits PROCCREATE_NAME for every existing process too. */
static int on_process(struct traceparser_state *tp, void *ud, unsigned header, unsigned time,
                      unsigned *d, unsigned n)
{
    proc_info_t *p;
    if (n < 2) return 0;
    p = proc((int)d[1]);
    if (proc((int)d[0])->traced) p->traced = 1;            /* follow children */
    if (ud && n > 2) {                                     /* keep the basename */
        const char *s = (const char *)&d[2];
        size_t len = strnlen(s, (n - 2) * sizeof d[0]), i, from = 0;
        for (i = 0; i < len; i++)
            if (s[i] == '/') from = i + 1;
        snprintf(p->name, sizeof p->name, "%.*s", (int)(len - from), s + from);
    }
    return 0;
}

/* SND_MESSAGE (COMM_SMSG): rcvid, server pid. Logged on the client's CPU. */
static int on_send(struct traceparser_state *tp, void *ud, unsigned header, unsigned time,
                   unsigned *d, unsigned n)
{
    runner_t *c = &running[_NTO_TRACE_GETCPU(header) % MAXCPU];
    flight_t *m = n >= 2 ? find_or_add(c->pid, c->tid) : NULL;
    if (!m) return 0;
    m->rcvid = (int)d[0];
    m->srv = (int)d[1];
    m->t_send = time;
    return 0;
}

/* REC_MESSAGE (COMM_RMSG): rcvid, server pid. Logged on the server's CPU. */
static int on_receive(struct traceparser_state *tp, void *ud, unsigned header, unsigned time,
                      unsigned *d, unsigned n)
{
    runner_t *s = &running[_NTO_TRACE_GETCPU(header) % MAXCPU];
    flight_t *m = n >= 2 ? find_by_rcvid((int)d[1], (int)d[0]) : NULL;
    if (!m) return 0;
    m->received = 1;
    m->t_recv = time;
    m->srv_prio = s->pid == m->srv ? s->prio : -1;         /* the server thread runs here */
    return 0;
}

/* REPLY_MESSAGE (COMM_REPLY), MSG_ERROR (COMM_ERROR): client tid, client pid. */
static int on_reply(struct traceparser_state *tp, void *ud, unsigned header, unsigned time,
                    unsigned *d, unsigned n)
{
    flight_t *m = n >= 2 ? find_by_client((int)d[1], (int)d[0]) : NULL;
    proc_info_t *srv;
    double queued, served;
    if (!m || !m->received) return 0;
    queued = us(m->t_send, m->t_recv);
    served = us(m->t_recv, time);
    printf("%12s[%d:%d] -> %-12s[%d]  queued %9.1f us  served %9.1f us  server prio %d%s\n",
           name_of(m->pid), m->pid, m->tid, name_of(m->srv), m->srv, queued, served,
           m->srv_prio, ud ? "  (MsgError)" : "");
    srv = proc(m->srv);
    srv->msgs++;
    srv->queued_us += queued;
    srv->served_us += served;
    drop(m);
    return 0;
}

static int by_total(const void *a, const void *b)
{
    const proc_info_t *x = a, *y = b;
    double tx = x->queued_us + x->served_us, ty = y->queued_us + y->served_us;
    return (tx < ty) - (tx > ty);
}

static int report(pid_t child)
{
    struct traceparser_state *tp = traceparser_init(NULL);
    int i;

    if (!tp) return -1;
    proc(child)->traced = 1;
    traceparser_cs(tp, NULL, on_running, _NTO_TRACE_THREAD, _NTO_TRACE_THRUNNING);
    traceparser_cs(tp, NULL, on_process, _NTO_TRACE_PROCESS, _NTO_TRACE_PROCCREATE);
    traceparser_cs(tp, "name", on_process, _NTO_TRACE_PROCESS, _NTO_TRACE_PROCCREATE_NAME);
    traceparser_cs(tp, NULL, on_send, _NTO_TRACE_COMM, _NTO_TRACE_COMM_SMSG);
    traceparser_cs(tp, NULL, on_receive, _NTO_TRACE_COMM, _NTO_TRACE_COMM_RMSG);
    traceparser_cs(tp, NULL, on_reply, _NTO_TRACE_COMM, _NTO_TRACE_COMM_REPLY);
    traceparser_cs(tp, "error", on_reply, _NTO_TRACE_COMM, _NTO_TRACE_COMM_ERROR);
    if (traceparser(tp, NULL, KEV) != 0) {
        fprintf(stderr, "msgwait: cannot parse %s: %s\n", KEV, strerror(errno));
        traceparser_destroy(&tp);
        return -1;
    }
    traceparser_destroy(&tp);

    qsort(procs, nprocs, sizeof procs[0], by_total);
    printf("\n%-16s %8s %14s %14s\n", "server", "messages", "queued us", "served us");
    for (i = 0; i < nprocs && procs[i].msgs; i++)
        printf("%-16s %8u %14.1f %14.1f\n", procs[i].name, procs[i].msgs,
               procs[i].queued_us, procs[i].served_us);
    return 0;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    struct inheritance inh;
    pid_t tl, child;
    int i, status = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: msgwait command [arg ...]\n");
        return 2;
    }
    cycles_per_sec = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    if ((tl = start_tracelogger()) < 0) return 1;

    /* Create the command stopped before its first instruction, so that
     * tracing is on before it runs. */
    memset(&inh, 0, sizeof inh);
    inh.flags = SPAWN_HOLD;
    child = spawnp(argv[1], 0, NULL, &inh, &argv[1], environ);
    if (child == -1) {
        fprintf(stderr, "msgwait: %s: %s\n", argv[1], strerror(errno));
        stop_tracelogger(tl);
        return 127;
    }

    /* tracelogger needs a moment to set up the kernel's buffers. */
    for (i = 0; i < 30 && arm_kernel() == -1; i++)
        usleep(100 * 1000);
    if (i == 30) {
        fprintf(stderr, "msgwait: TraceEvent: %s (root? instrumented kernel?)\n", strerror(errno));
        kill(child, SIGKILL);
        stop_tracelogger(tl);
        return 1;
    }

    kill(child, SIGCONT);                                   /* let it run */
    while (waitpid(child, &status, 0) == -1 && errno == EINTR)
        ;
    stop_tracelogger(tl);
    return report(child) == 0 ? WEXITSTATUS(status) : 1;
}
