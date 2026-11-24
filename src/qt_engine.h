/*
 * qtrace - strace-style tracing for the QNX microkernel.
 *
 * The engine turns a stream of QNX instrumented-kernel trace events into
 * per-process kernel-call records. Its main job is the part that is unique to
 * QNX: almost every POSIX call is a message sent to a server process, so a
 * MsgSend() is decoded back into the POSIX operation it carries (read, write,
 * open, devctl, ...) and joined with the server that received and answered it.
 *
 * The engine is plain C99 and knows nothing about the SDP headers: events are
 * fed in already classified (see qt_ev_t). The QNX adapter (qt_kev.c) maps
 * libtraceparser output onto this interface; the unit tests feed synthetic
 * events directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QT_ENGINE_H
#define QT_ENGINE_H

#include <stdint.h>
#include <stdio.h>
#include "qt_events_gen.h"

typedef enum {
    QT_CLS_KENTER,      /* _NTO_TRACE_KERCALLENTER, id is enum qt_kcall */
    QT_CLS_KEXIT,       /* _NTO_TRACE_KERCALLEXIT,  id is enum qt_kcall */
    QT_CLS_KINT,        /* _NTO_TRACE_KERCALLINT,   id is enum qt_kcall */
    QT_CLS_EVENT        /* every other class,        id is enum qt_evid  */
} qt_cls_t;

typedef struct {
    uint64_t        ts;     /* timestamp in clock cycles, extended to 64 bits */
    unsigned        cpu;
    qt_cls_t        cls;
    int             id;
    int             is64;   /* event was ORed with _NTO_TRACE_KERCALL64 */
    unsigned        n;      /* number of data words */
    const uint32_t *d;      /* data words, as libtraceparser delivers them */
} qt_ev_t;

/* One completed (or abandoned) kernel call of a traced thread. */
typedef struct {
    uint64_t t0, t1;            /* entry / exit timestamps (0 = not seen) */
    int      pid, tid;
    int      k;                 /* enum qt_kcall */
    int      prio;              /* caller priority at entry, -1 if unknown */
    uint32_t a[8];              /* first entry argument words (wide layout) */
    unsigned na;
    int64_t  ret;
    int      err;               /* errno if the call failed, else 0 */
    unsigned char done, intr, is64, pad;

    /* Message passing (MsgSend*, MsgSendPulse) */
    int      coid;
    uint32_t msg[3];            /* first 12 bytes of the message (wide mode) */
    unsigned nmsg;
    int      srv_pid, srv_tid, srv_prio;
    uint32_t rcvid;
    uint64_t t_recv, t_reply;   /* server MsgReceive() / MsgReply() times */
    int      parent;            /* index of the client call this server-side
                                   call was made on behalf of, -1 if none */
    char    *path;              /* path name for io_connect messages, or NULL */
} qt_call_t;

typedef struct {
    FILE       *out;            /* strace-like text output, NULL for none */
    int         root_pid;       /* trace this process; 0 = every process */
    int         follow;         /* also trace processes created by traced ones */
    int         servers;        /* also record calls made by servers while they
                                   handle a traced client's message (--deep) */
    int         verbose;        /* print every kernel call, not only the useful ones */
    uint64_t    cycles_per_sec; /* from the trace header; 0 = print cycles */
} qt_opts_t;

typedef struct qt_engine qt_engine_t;

qt_engine_t *qt_engine_new(const qt_opts_t *opts);
void         qt_engine_feed(qt_engine_t *e, const qt_ev_t *ev);
void         qt_engine_finish(qt_engine_t *e);     /* completes and prints the calls */
void         qt_engine_set_cycles_per_sec(qt_engine_t *e, uint64_t cps);
void         qt_engine_free(qt_engine_t *e);

/* Results, valid after qt_engine_finish(). */
unsigned          qt_engine_ncalls(const qt_engine_t *e);
const qt_call_t  *qt_engine_call(const qt_engine_t *e, unsigned i);
const char       *qt_engine_pname(const qt_engine_t *e, int pid);
const char       *qt_engine_tname(const qt_engine_t *e, int pid, int tid);
uint64_t          qt_engine_t0(const qt_engine_t *e);
double            qt_engine_us(const qt_engine_t *e, uint64_t cycles);
unsigned long     qt_engine_nevents(const qt_engine_t *e);
unsigned long     qt_engine_dropped(const qt_engine_t *e);

/* "-c" table: per server and operation counts and blocked time. */
void qt_engine_summary(const qt_engine_t *e, FILE *f);

/* Describe a call as text ("write(fd 1, 42 bytes)"); returns buf. */
const char *qt_call_describe(const qt_engine_t *e, const qt_call_t *c, char *buf, size_t len);
/* The call's result as text ("42", "-1 errno 2 (...)", "redirect 0x800b0000"); returns buf. */
const char *qt_call_result(const qt_call_t *c, char *buf, size_t len);

/* Name of a QNX I/O or system message type, NULL if unknown. */
const char *qt_msg_name(uint32_t msg0);
/* procnto's own messages (type below 0x100): e.g. "mmap", "munmap"; NULL if unknown. */
const char *qt_sysmsg_name(uint32_t msg0);
/* File type an io_connect asks for ("pipe", "socket", ...); NULL for a plain path lookup. */
const char *qt_ftype_name(uint32_t ftype);

#endif
