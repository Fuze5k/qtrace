/*
 * qtrace - strace for the QNX microkernel.
 *
 *   qtrace [options] command [arg ...]     trace a new process
 *   qtrace [options] -p pid [-s seconds]   trace a running process
 *   qtrace [options] [-p pid] -i file.kev  decode an existing kernel trace
 *
 * Capture uses the instrumented kernel that QNX OS 8.0 always runs: tracelogger
 * is started in daemon mode to drain the kernel buffers into a .kev file, and
 * qtrace itself configures the event filters with TraceEvent(), so that kernel
 * calls are logged only for the traced process while thread, message-passing
 * and process events are logged for everyone (they tell us which server
 * answered and how long it took). The new process is created held
 * (SPAWN_HOLD) so that tracing is armed before its first instruction.
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
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/trace.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include "qt_engine.h"
#include "qt_kev.h"
#include "qt_report.h"

extern char **environ;

static volatile sig_atomic_t interrupted;
static pid_t child = -1;

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
    if (child > 0) kill(child, SIGINT);
}

static void usage(FILE *f)
{
    fputs("usage: qtrace [-cfDv] [-o file] [-H report.html] [-k file.kev] command [arg ...]\n"
          "       qtrace [-cfDv] [-o file] [-H report.html] [-k file.kev] -p pid [-s seconds]\n"
          "       qtrace [-cfDv] [-o file] [-H report.html] [-p pid] -i file.kev\n"
          "\n"
          "Show every kernel call a QNX process makes. Messages to servers are shown as\n"
          "the POSIX operation they carry, with the server that handled them, how long\n"
          "the message waited and how long the server worked on it.\n"
          "\n"
          "  -c          print a summary: time per server and operation\n"
          "  -f          follow processes created by the traced process\n"
          "  -D          deep: also record what servers do while serving the traced process\n"
          "  -v          verbose: print the server-side calls recorded by -D\n"
          "  -o file     write the call log to file (default: stderr)\n"
          "  -H file     write a self-contained HTML report with a timeline\n"
          "  -k file     keep the raw kernel trace (.kev) in file\n"
          "  -i file     decode an existing .kev file instead of tracing\n"
          "  -p pid      trace this process (with -i: show only this process)\n"
          "  -s seconds  with -p: stop after this many seconds (default: Ctrl-C)\n"
          "  -b buffers  kernel trace buffers per CPU (default 32)\n", f);
}

static int trace_cmd(int cmd, int a1, int a2)
{
    int rc = TraceEvent(cmd, a1, a2);
    if (rc == -1 && errno != ENOSYS) return -1;
    return 0;
}

/* Kernel calls for one process only (unless follow/deep); thread, message,
 * process and path events for everyone; wide mode for message contents. */
static int arm_tracing(pid_t pid, int all_kercalls)
{
    static const int classes[] = { _NTO_TRACE_KERCALL, _NTO_TRACE_THREAD, _NTO_TRACE_COMM, _NTO_TRACE_SYSTEM };
    unsigned i;

    if (TraceEvent(_NTO_TRACE_DELALLCLASSES) == -1) return -1;
    for (i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        trace_cmd(_NTO_TRACE_CLRCLASSPID, classes[i], 0);
        trace_cmd(_NTO_TRACE_CLRCLASSTID, classes[i], 0);
    }
    if (TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_THREAD) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_COMM) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_PROCESS) == -1 ||
        TraceEvent(_NTO_TRACE_ADDEVENT, _NTO_TRACE_SYSTEM, _NTO_TRACE_SYS_PATHMGR) == -1 ||
        TraceEvent(_NTO_TRACE_ADDCLASS, _NTO_TRACE_KERCALL) == -1)
        return -1;
    if (!all_kercalls && TraceEvent(_NTO_TRACE_SETCLASSPID, _NTO_TRACE_KERCALL, pid) == -1)
        return -1;
    if (TraceEvent(_NTO_TRACE_SETALLCLASSESWIDE) == -1) return -1;
    return 0;
}

static pid_t start_tracelogger(const char *kev, unsigned nbuf, int verbose)
{
    char nb[16];
    char *argv[] = { "tracelogger", "-d1", "-w", "-c", "-k", nb, "-f", (char *)kev, NULL };
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int rc;

    snprintf(nb, sizeof nb, "%u", nbuf);
    posix_spawn_file_actions_init(&fa);
    if (!verbose) {
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    rc = posix_spawnp(&pid, "tracelogger", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        fprintf(stderr, "qtrace: cannot start tracelogger: %s\n", strerror(rc));
        return -1;
    }
    return pid;
}

static void stop_tracelogger(pid_t tl)
{
    int st;
    TraceEvent(_NTO_TRACE_STOP);          /* also flushes the last buffer */
    usleep(300 * 1000);                   /* let tracelogger write it out */
    kill(tl, SIGINT);
    waitpid(tl, &st, 0);
}

/* Start tracing; tracelogger needs a moment to allocate the kernel buffers. */
static int start_tracing(pid_t pid, int all_kercalls)
{
    int tries;
    for (tries = 0; tries < 30; tries++) {
        usleep(100 * 1000);
        if (arm_tracing(pid, all_kercalls) == 0 && TraceEvent(_NTO_TRACE_START) != -1)
            return 0;
    }
    fprintf(stderr, "qtrace: TraceEvent() failed: %s (are you root?)\n", strerror(errno));
    return -1;
}

static char *join_args(char **argv)
{
    size_t len = 1;
    char **a, *s;
    for (a = argv; *a; a++) len += strlen(*a) + 1;
    s = malloc(len);
    if (!s) return NULL;
    s[0] = 0;
    for (a = argv; *a; a++) {
        if (a != argv) strcat(s, " ");
        strcat(s, *a);
    }
    return s;
}

int main(int argc, char **argv)
{
    qt_opts_t o;
    qt_engine_t *e;
    qt_kev_info_t info;
    qt_report_meta_t meta;
    struct utsname un;
    const char *outpath = NULL, *html = NULL, *keep = NULL, *input = NULL;
    char kev[64], date[64], cmdline[64];
    char *cmd = NULL;
    int opt, summary = 0, follow = 0, deep = 0, verbose = 0, seconds = 0, status = 0, rc = 0;
    unsigned nbuf = 32;
    pid_t pid = 0, tl = -1;
    time_t now;

    while ((opt = getopt(argc, argv, "+cfDvo:H:k:i:p:s:b:h")) != -1) {
        switch (opt) {
        case 'c': summary = 1; break;
        case 'f': follow = 1; break;
        case 'D': deep = 1; break;
        case 'v': verbose = 1; break;
        case 'o': outpath = optarg; break;
        case 'H': html = optarg; break;
        case 'k': keep = optarg; break;
        case 'i': input = optarg; break;
        case 'p': pid = (pid_t)atoi(optarg); break;
        case 's': seconds = atoi(optarg); break;
        case 'b': nbuf = (unsigned)atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (!input && !pid && optind >= argc) { usage(stderr); return 2; }

    memset(&o, 0, sizeof o);
    o.out = stderr;
    o.follow = follow;
    o.servers = deep;
    o.verbose = verbose;
    if (outpath && !(o.out = fopen(outpath, "w"))) {
        fprintf(stderr, "qtrace: %s: %s\n", outpath, strerror(errno));
        return 1;
    }

    if (!input) {
        if (geteuid() != 0)
            fprintf(stderr, "qtrace: warning: tracing needs root (tracelogger, TraceEvent)\n");
        if (keep) snprintf(kev, sizeof kev, "%s", keep);
        else snprintf(kev, sizeof kev, "/tmp/qtrace.%d.kev", (int)getpid());
        input = kev;

        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        if ((tl = start_tracelogger(kev, nbuf, verbose)) < 0) return 1;

        if (!pid) {
            struct inheritance inh;
            memset(&inh, 0, sizeof inh);
            inh.flags = SPAWN_HOLD;           /* stopped before its first instruction */
            child = spawnp(argv[optind], 0, NULL, &inh, &argv[optind], environ);
            if (child == -1) {
                fprintf(stderr, "qtrace: %s: %s\n", argv[optind], strerror(errno));
                stop_tracelogger(tl);
                return 127;
            }
            pid = child;
            cmd = join_args(&argv[optind]);
        } else {
            snprintf(cmdline, sizeof cmdline, "pid %d", (int)pid);
            cmd = cmdline;
        }

        if (start_tracing(pid, follow || deep) != 0) {
            if (child > 0) kill(child, SIGKILL);
            stop_tracelogger(tl);
            return 1;
        }
        if (child > 0) {
            kill(child, SIGCONT);
            while (waitpid(child, &status, 0) == -1 && errno == EINTR)
                ;
        } else {
            time_t until = seconds ? time(NULL) + seconds : 0;
            while (!interrupted && kill(pid, 0) == 0 && (!until || time(NULL) < until))
                usleep(100 * 1000);
        }
        stop_tracelogger(tl);
    } else if (!cmd) {
        cmd = (char *)input;
    }

    o.root_pid = (int)pid;
    if (!(e = qt_engine_new(&o))) return 1;
    memset(&info, 0, sizeof info);
    if (qt_kev_parse(input, e, &info) != 0) {
        fprintf(stderr, "qtrace: cannot parse %s: %s\n", input, strerror(errno));
        rc = 1;
    }
    /* On the traced machine the syspage has the exact clock rate. */
    qt_engine_set_cycles_per_sec(e, info.cycles_per_sec ? info.cycles_per_sec
                                                        : SYSPAGE_ENTRY(qtime)->cycles_per_sec);
    qt_engine_finish(e);
    if (qt_engine_dropped(e))
        fprintf(o.out, "qtrace: warning: the kernel dropped %lu events (try -b)\n", qt_engine_dropped(e));
    if (summary) qt_engine_summary(e, o.out);

    if (html) {
        FILE *hf = fopen(html, "w");
        uname(&un);
        now = time(NULL);
        strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", localtime(&now));
        meta.cmd = cmd ? cmd : "";
        meta.node = info.node[0] ? info.node : un.nodename;
        meta.release = info.release[0] ? info.release : un.release;
        meta.machine = info.machine[0] ? info.machine : un.machine;
        meta.date = info.date[0] ? info.date : date;
        meta.cpus = info.cpus ? info.cpus : _syspage_ptr->num_cpu;
        if (!hf || qt_report_write(e, &meta, hf) != 0 || fclose(hf) != 0) {
            fprintf(stderr, "qtrace: %s: %s\n", html, strerror(errno));
            rc = 1;
        } else {
            fprintf(stderr, "qtrace: report written to %s\n", html);
        }
    }
    if (!keep && tl > 0) unlink(kev);
    qt_engine_free(e);
    if (o.out != stderr) fclose(o.out);
    if (rc) return rc;
    if (child > 0) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return 0;
}
