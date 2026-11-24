/*
 * Read a QNX kernel event (.kev) file with libtraceparser and feed the engine.
 *
 * Every event we care about gets its own traceparser callback registration
 * whose user data says what the event is, so the engine never has to decode
 * the compact internal header format. The only header field used is the CPU.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/kercalls.h>
#include <sys/trace.h>
#include <sys/traceparser.h>
#include "qt_engine.h"
#include "qt_kev.h"

typedef struct { qt_cls_t cls; int id; int is64; } reg_t;

typedef struct {
    qt_engine_t *e;
    uint32_t msb, last_lsb;   /* timestamp extension state */
    int have_time;
} ctx_t;

static ctx_t ctx;

static uint64_t extend_ts(uint32_t lsb)
{
    if (ctx.have_time && lsb < ctx.last_lsb && ctx.last_lsb - lsb > 0x80000000u)
        ctx.msb++;                              /* the 32-bit counter wrapped */
    ctx.last_lsb = lsb;
    return ((uint64_t)ctx.msb << 32) | lsb;
}

static int on_event(struct traceparser_state *state, void *ud, unsigned header, unsigned time,
                    unsigned *d, unsigned n)
{
    const reg_t *r = ud;
    qt_ev_t ev;
    (void)state;
    ev.ts = extend_ts(time);
    ev.cpu = _NTO_TRACE_GETCPU(header);
    ev.cls = r->cls;
    ev.id = r->id;
    ev.is64 = r->is64;
    ev.n = n;
    ev.d = (const uint32_t *)d;
    qt_engine_feed(ctx.e, &ev);
    return 0;
}

static int on_time(struct traceparser_state *state, void *ud, unsigned header, unsigned time,
                   unsigned *d, unsigned n)
{
    (void)state; (void)ud; (void)header; (void)time;
    if (n >= 2) {               /* msbtime, lsbtime */
        ctx.msb = d[0];
        ctx.last_lsb = d[1];
        ctx.have_time = 1;
    }
    return 0;
}

/* Registrations are kept for the lifetime of the parse. */
static reg_t regs[4 * QT_K__COUNT + QT_E__COUNT];
static unsigned nregs;

static int reg(struct traceparser_state *tp, unsigned cls, unsigned event, qt_cls_t qc, int id, int is64)
{
    reg_t *r = &regs[nregs++];
    r->cls = qc; r->id = id; r->is64 = is64;
    return traceparser_cs(tp, r, on_event, cls, event);
}

static void register_all(struct traceparser_state *tp)
{
    nregs = 0;
#define K(name) \
    reg(tp, _NTO_TRACE_KERCALLENTER, __KER_##name, QT_CLS_KENTER, QT_K_##name, 0); \
    reg(tp, _NTO_TRACE_KERCALLENTER, __KER_##name | _NTO_TRACE_KERCALL64, QT_CLS_KENTER, QT_K_##name, 1); \
    reg(tp, _NTO_TRACE_KERCALLEXIT,  __KER_##name, QT_CLS_KEXIT, QT_K_##name, 0); \
    reg(tp, _NTO_TRACE_KERCALLEXIT,  __KER_##name | _NTO_TRACE_KERCALL64, QT_CLS_KEXIT, QT_K_##name, 1);
#include "qt_kcalls_qnx.h"
#undef K
#define E(cls, name) reg(tp, _NTO_TRACE_##cls, _NTO_TRACE_##name, QT_CLS_EVENT, QT_E_##name, 0);
    E(THREAD, THRUNNING) E(THREAD, THREADY) E(THREAD, THSEND) E(THREAD, THRECEIVE)
    E(THREAD, THREPLY) E(THREAD, THCREATE) E(THREAD, THDESTROY) E(THREAD, THDEAD)
    E(THREAD, THMUTEX) E(THREAD, THCONDVAR) E(THREAD, THSEM) E(THREAD, THNANOSLEEP)
    E(THREAD, THSTOPPED) E(THREAD, THINTR) E(THREAD, THJOIN) E(THREAD, THWAITPAGE)
    E(THREAD, THSIGSUSPEND) E(THREAD, THSIGWAITINFO)
    E(COMM, COMM_SMSG) E(COMM, COMM_RMSG) E(COMM, COMM_REPLY) E(COMM, COMM_ERROR)
    E(COMM, COMM_SPULSE) E(COMM, COMM_RPULSE) E(COMM, COMM_SIGNAL)
    E(PROCESS, PROCCREATE) E(PROCESS, PROCCREATE_NAME) E(PROCESS, PROCDESTROY)
    E(PROCESS, PROCTHREAD_NAME)
    E(SYSTEM, SYS_PATHMGR)
    E(CONTROL, CONTROLDROPPED)
#undef E
    traceparser_cs(tp, NULL, on_time, _NTO_TRACE_CONTROL, _NTO_TRACE_CONTROLTIME);
}

static const char *info_str(struct traceparser_state *tp, info_modes_t mode)
{
    const char *s = traceparser_get_info(tp, mode, NULL);
    return s ? s : "";
}

int qt_kev_parse(const char *path, qt_engine_t *e, qt_kev_info_t *info)
{
    struct traceparser_state *tp = traceparser_init(NULL);
    int rc;
    if (!tp) return -1;
    memset(&ctx, 0, sizeof ctx);
    ctx.e = e;
    register_all(tp);
    rc = traceparser(tp, NULL, path);
    if (info) {
        /* Header keyword values come back as strings (checked on SDP 8.0). */
        snprintf(info->node, sizeof info->node, "%s", info_str(tp, _TRACEPARSER_INFO_NODENAME));
        snprintf(info->release, sizeof info->release, "%s", info_str(tp, _TRACEPARSER_INFO_SYS_RELEASE));
        snprintf(info->machine, sizeof info->machine, "%s", info_str(tp, _TRACEPARSER_INFO_MACHINE));
        snprintf(info->date, sizeof info->date, "%s", info_str(tp, _TRACEPARSER_INFO_DATE));
        info->cpus = (unsigned)strtoul(info_str(tp, _TRACEPARSER_INFO_CPU_NUM), NULL, 0);
        info->cycles_per_sec = strtoull(info_str(tp, _TRACEPARSER_INFO_CYCLES_PER_SEC), NULL, 0);
    }
    traceparser_destroy(&tp);
    return rc;
}
