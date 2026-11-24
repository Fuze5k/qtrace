/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QT_KEV_H
#define QT_KEV_H

#include <stdint.h>
#include "qt_engine.h"

typedef struct {
    char node[64], release[32], machine[32], date[64];
    unsigned cpus;
    uint64_t cycles_per_sec;
} qt_kev_info_t;

/* Parse a .kev file with libtraceparser, feeding every event to `e`, and
 * return the trace header (node name, clock rate, ...) in `info`. */
int qt_kev_parse(const char *path, qt_engine_t *e, qt_kev_info_t *info);

#endif
