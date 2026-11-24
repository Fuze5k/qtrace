/*
 * Self-contained HTML report: the template from report/template.html with the
 * trace data inlined as JSON. No network access is needed to view it, so it
 * can be copied off the target and opened on any host.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qt_engine.h"
#include "qt_report.h"

extern const char qt_report_head[], qt_report_tail[];

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch == '<') fputs("\\u003c", f);          /* keep </script> out of the data */
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static double rel(const qt_engine_t *e, uint64_t t)
{
    return t ? qt_engine_us(e, t - qt_engine_t0(e)) : 0.0;
}

int qt_report_write(const qt_engine_t *e, const qt_report_meta_t *m, FILE *f)
{
    unsigned i, n = qt_engine_ncalls(e), first = 1, np = 0;
    char desc[256], res[48];
    int *pids;

    fputs(qt_report_head, f);
    fputs("{\"meta\":{\"cmd\":", f); json_str(f, m->cmd);
    fputs(",\"node\":", f); json_str(f, m->node);
    fputs(",\"release\":", f); json_str(f, m->release);
    fputs(",\"machine\":", f); json_str(f, m->machine);
    fputs(",\"date\":", f); json_str(f, m->date);
    fprintf(f, ",\"cpus\":%u,\"events\":%lu,\"dropped\":%lu},\"procs\":{",
            m->cpus, qt_engine_nevents(e), qt_engine_dropped(e));

    /* Names of every process that appears in a call, as client or server. */
    pids = malloc((2 * (size_t)n + 1) * sizeof *pids);
    if (!pids) return -1;
    for (i = 0; i < n; i++) {
        const qt_call_t *c = qt_engine_call(e, i);
        pids[np++] = c->pid;
        if (c->srv_pid >= 0) pids[np++] = c->srv_pid;
    }
    qsort(pids, np, sizeof *pids, int_cmp);
    for (i = 0; i < np; i++) {
        if (i && pids[i] == pids[i - 1]) continue;
        fprintf(f, "%s\"%d\":", first ? "" : ",", pids[i]);
        json_str(f, qt_engine_pname(e, pids[i]));
        first = 0;
    }
    free(pids);
    fputs("},\"calls\":[", f);
    for (i = 0; i < n; i++) {
        const qt_call_t *c = qt_engine_call(e, i);
        uint64_t start = c->t0 ? c->t0 : c->t1;
        double dur = c->t0 && c->t1 ? qt_engine_us(e, c->t1 - c->t0) : 0.0;
        qt_call_describe(e, c, desc, sizeof desc);
        fprintf(f, "%s[%.3f,%.3f,%d,%d,", i ? ",\n" : "", rel(e, start), dur, c->pid, c->tid);
        json_str(f, desc);
        fputc(',', f);
        json_str(f, qt_call_result(c, res, sizeof res));
        fprintf(f, ",%d,%d,%d,%.3f,%.3f,%d,%d,\"%s\"]",
                c->intr ? 4 /* EINTR */ : c->err, c->srv_pid, c->srv_tid,
                rel(e, c->t_recv), rel(e, c->t_reply), c->prio, c->srv_prio,
                c->parent >= 0 ? "s" : "c");
    }
    fputs("]}", f);
    fputs(qt_report_tail, f);
    return ferror(f) ? -1 : 0;
}
