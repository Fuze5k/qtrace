/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QT_REPORT_H
#define QT_REPORT_H

#include <stdio.h>
#include "qt_engine.h"

typedef struct {
    const char *cmd;        /* what was traced, e.g. "ls -l /tmp" */
    const char *node, *release, *machine, *date;
    unsigned    cpus;
} qt_report_meta_t;

int qt_report_write(const qt_engine_t *e, const qt_report_meta_t *m, FILE *f);

#endif
