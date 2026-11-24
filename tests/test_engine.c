/*
 * Engine test on a synthetic trace that follows the event sequences shown in
 * the QNX SDP 8.0 System Analysis Toolkit guide (traceprinter examples):
 * an open() that procnto resolves and a file system serves, a write() to
 * devc-pty that has to wait for the server, a failing open(), a close(),
 * a plain kernel call and a pulse. 1 cycle = 1 ns.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/qt_engine.h"
#include "../src/qt_msg.h"
#include "../src/qt_report.h"

static qt_engine_t *E;
static uint64_t T = 1000000;    /* start at 1 ms */

static void ev(unsigned cpu, qt_cls_t cls, int id, int is64, unsigned n, ...)
{
    uint32_t d[32];
    unsigned i;
    va_list ap;
    qt_ev_t e;
    va_start(ap, n);
    for (i = 0; i < n; i++) d[i] = va_arg(ap, uint32_t);
    va_end(ap);
    e.ts = T; e.cpu = cpu; e.cls = cls; e.id = id; e.is64 = is64; e.n = n; e.d = d;
    qt_engine_feed(E, &e);
    T += 1000;                  /* 1 us between events */
}

/* Pack a string into little-endian words after `pre` leading words. */
static void ev_str(unsigned cpu, int id, uint32_t a, uint32_t b, const char *s)
{
    uint32_t d[40] = { a, b };
    size_t i, len = strlen(s);
    qt_ev_t e;
    for (i = 0; i <= len; i++) d[2 + i / 4] |= (uint32_t)(unsigned char)s[i] << (8 * (i % 4));
    e.ts = T; e.cpu = cpu; e.cls = QT_CLS_EVENT; e.id = id; e.is64 = 0;
    e.n = (unsigned)(2 + (len + 4) / 4); e.d = d;
    qt_engine_feed(E, &e);
    T += 1000;
}

#define TH(cpu, ev_, pid, tid, prio) ev(cpu, QT_CLS_EVENT, QT_E_##ev_, 0, 4, pid, tid, prio, 2u)
#define SEND64(cpu, coid, m0, m1, m2) \
    ev(cpu, QT_CLS_KENTER, QT_K_MSG_SENDV, 1, 8, coid, 1u, 0u, 1u, 0u, m0, m1, m2)
#define SENDRET64(cpu, st) ev(cpu, QT_CLS_KEXIT, QT_K_MSG_SENDV, 1, 5, st, 0u, 0u, 0u, 0u)

enum { PROCNTO = 1, PTY = 159762, FS = 8195, APP = 4242, PULSER = 5000 };

int main(void)
{
    qt_opts_t o;
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    const char *expect_path = "tests/expected_engine.txt";
    FILE *ef;
    char want[8192];
    size_t wl;

    memset(&o, 0, sizeof o);
    o.out = out; o.root_pid = APP; o.cycles_per_sec = 1000000000ULL;
    E = qt_engine_new(&o);

    /* Initial state dump */
    ev_str(0, QT_E_PROCCREATE_NAME, 0, PROCNTO, "proc/boot/procnto-smp-instr");
    ev_str(0, QT_E_PROCCREATE_NAME, 1, PTY, "proc/boot/devc-pty");
    ev_str(0, QT_E_PROCCREATE_NAME, 1, FS, "proc/boot/devb-virtio");
    ev_str(0, QT_E_PROCCREATE_NAME, 1, APP, "data/home/qnxuser/demo");
    TH(0, THRUNNING, APP, 1, 10);

    /* open("/etc/hosts"): resolve through procnto, attach, connect to devb */
    SEND64(0, 0x40000000u, (QT_IO_CONNECT_OPEN << 16) | QT_IO_CONNECT, 0, 0);
    ev(0, QT_CLS_EVENT, QT_E_COMM_SMSG, 0, 2, 0x11u, PROCNTO);
    TH(0, THREPLY, APP, 1, 10);
    TH(0, THRUNNING, PROCNTO, 7, 10);
    ev(0, QT_CLS_EVENT, QT_E_COMM_RMSG, 0, 2, 0x11u, PROCNTO);
    ev_str(0, QT_E_SYS_PATHMGR, APP, 1, "etc/hosts");
    ev(0, QT_CLS_EVENT, QT_E_COMM_REPLY, 0, 2, 1u, APP);
    TH(0, THREADY, APP, 1, 10);
    TH(0, THRUNNING, APP, 1, 10);
    SENDRET64(0, 0u);
    ev(0, QT_CLS_KENTER, QT_K_CONNECT_ATTACH, 0, 5, 0u, FS, 1u, 0u, 0u);
    ev(0, QT_CLS_KEXIT, QT_K_CONNECT_ATTACH, 0, 2, 3u, 0u);
    SEND64(0, 3u, (QT_IO_CONNECT_OPEN << 16) | QT_IO_CONNECT, 0, 0);
    ev(0, QT_CLS_EVENT, QT_E_COMM_SMSG, 0, 2, 0x22u, FS);
    TH(0, THREPLY, APP, 1, 10);
    TH(1, THRUNNING, FS, 2, 10);
    ev(1, QT_CLS_EVENT, QT_E_COMM_RMSG, 0, 2, 0x22u, FS);
    T += 20000;
    ev(1, QT_CLS_EVENT, QT_E_COMM_REPLY, 0, 2, 1u, APP);
    TH(1, THREADY, APP, 1, 10);
    TH(0, THRUNNING, APP, 1, 10);
    SENDRET64(0, 0u);

    /* write(1, 42 bytes) to devc-pty while the server is busy */
    SEND64(0, 1u, QT_IO_WRITE, 42u, 0u);
    ev(0, QT_CLS_EVENT, QT_E_COMM_SMSG, 0, 2, 0x4fu, PTY);
    TH(0, THSEND, APP, 1, 10);
    T += 5000;
    TH(1, THRUNNING, PTY, 1, 10);
    ev(1, QT_CLS_EVENT, QT_E_COMM_RMSG, 0, 2, 0x4fu, PTY);
    TH(1, THREPLY, APP, 1, 10);
    T += 30000;
    ev(1, QT_CLS_EVENT, QT_E_COMM_REPLY, 0, 2, 1u, APP);
    TH(1, THREADY, APP, 1, 10);
    TH(0, THRUNNING, APP, 1, 10);
    SENDRET64(0, 42u);

    /* a plain kernel call */
    ev(0, QT_CLS_KENTER, QT_K_CLOCK_TIME, 0, 3, 0u, 0u, 0u);
    ev(0, QT_CLS_KEXIT, QT_K_CLOCK_TIME, 0, 3, 0u, 1700000000u, 5u);

    /* open() of a missing file: procnto answers ENOENT */
    SEND64(0, 0x40000000u, (QT_IO_CONNECT_OPEN << 16) | QT_IO_CONNECT, 0, 0);
    ev(0, QT_CLS_EVENT, QT_E_COMM_SMSG, 0, 2, 0x12u, PROCNTO);
    TH(0, THREPLY, APP, 1, 10);
    TH(0, THRUNNING, PROCNTO, 7, 10);
    ev(0, QT_CLS_EVENT, QT_E_COMM_RMSG, 0, 2, 0x12u, PROCNTO);
    ev_str(0, QT_E_SYS_PATHMGR, APP, 1, "etc/nope");
    ev(0, QT_CLS_EVENT, QT_E_COMM_ERROR, 0, 2, 1u, APP);
    TH(0, THREADY, APP, 1, 10);
    TH(0, THRUNNING, APP, 1, 10);
    ev(0, QT_CLS_KEXIT, QT_K_MSG_SENDV, 1, 2, 0xffffffffu, 2u);

    /* an untraced process sends a pulse: must not appear */
    TH(2, THRUNNING, PULSER, 1, 20);
    ev(2, QT_CLS_KENTER, QT_K_MSG_SEND_PULSE, 0, 4, 2u, 20u, 5u, 0u);
    ev(2, QT_CLS_KEXIT, QT_K_MSG_SEND_PULSE, 0, 2, 0u, 0u);

    /* close(3) */
    SEND64(0, 3u, (4u << 16) | QT_IO_CLOSE, 0, 0);
    ev(0, QT_CLS_EVENT, QT_E_COMM_SMSG, 0, 2, 0x23u, FS);
    TH(0, THREPLY, APP, 1, 10);
    TH(1, THRUNNING, FS, 2, 10);
    ev(1, QT_CLS_EVENT, QT_E_COMM_RMSG, 0, 2, 0x23u, FS);
    ev(1, QT_CLS_EVENT, QT_E_COMM_REPLY, 0, 2, 1u, APP);
    TH(0, THRUNNING, APP, 1, 10);
    SENDRET64(0, 0u);
    ev(0, QT_CLS_KENTER, QT_K_CONNECT_DETACH, 0, 2, 3u, 0u);
    ev(0, QT_CLS_KEXIT, QT_K_CONNECT_DETACH, 0, 2, 0u, 0u);

    qt_engine_finish(E);
    fprintf(out, "--\n");
    qt_engine_summary(E, out);
    fclose(out);

    if (getenv("QT_UPDATE_EXPECTED")) {
        ef = fopen(expect_path, "w");
        if (!ef) { perror(expect_path); return 1; }
        fwrite(buf, 1, len, ef);
        fclose(ef);
        printf("updated %s\n", expect_path);
        return 0;
    }
    ef = fopen(expect_path, "r");
    if (!ef) { perror(expect_path); fputs(buf, stdout); return 1; }
    wl = fread(want, 1, sizeof want - 1, ef);
    want[wl] = 0;
    fclose(ef);
    if (wl != len || memcmp(want, buf, len) != 0) {
        printf("FAIL: output differs from %s\n--- got ---\n%s", expect_path, buf);
        return 1;
    }
    if (getenv("QT_REPORT")) {
        qt_report_meta_t m = { "demo (synthetic trace)", "qemu-a64", "8.0.0", "aarch64le", "test", 2 };
        FILE *rf = fopen(getenv("QT_REPORT"), "w");
        if (!rf || qt_report_write(E, &m, rf) != 0) { perror("report"); return 1; }
        fclose(rf);
    }
    printf("PASS test_engine (%u calls)\n", qt_engine_ncalls(E));
    qt_engine_free(E);
    free(buf);
    return 0;
}
