# qtrace - strace for the QNX microkernel
#
#   make test            unit tests of the decoder on this host (no SDP needed)
#   make qnx             cross-compile for QNX 8.0 (needs qcc: source qnxsdp-env.sh,
#                        or run through env/qnx on macOS / Apple Silicon)
#   make demo            the demo programs in demo/, for QNX (like make qnx)
#   make gen             regenerate the event tables (needs the SAT event table text)
#
# SPDX-License-Identifier: Apache-2.0

ARCH     ?= aarch64le
CC_HOST  ?= cc
CFLAGS_HOST ?= -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
QCC      ?= qcc
QCFLAGS  ?= -Vgcc_nto$(ARCH) -std=gnu11 -O2 -g -Wall -Wextra -Werror

CORE     = src/qt_engine.c src/qt_msg.c src/qt_events_gen.c src/qt_report.c src/qt_report_tpl.c
QNX_SRC  = $(CORE) src/qt_kev.c src/qtrace.c

.PHONY: all test qnx demo gen clean

all: test

build/test_engine: tests/test_engine.c $(CORE) src/*.h
	@mkdir -p build
	$(CC_HOST) $(CFLAGS_HOST) -o $@ tests/test_engine.c $(CORE)

test: build/test_engine
	./build/test_engine

src/qt_report_tpl.c: report/template.html tools/embed_report.py
	python3 -I tools/embed_report.py $< $@

qnx: build/$(ARCH)/qtrace

build/$(ARCH)/qtrace: $(QNX_SRC) src/*.h src/qt_kcalls_qnx.h
	@mkdir -p build/$(ARCH)
	$(QCC) $(QCFLAGS) -o $@ $(QNX_SRC) -ltraceparser

demo: build/$(ARCH)/kcalls

build/$(ARCH)/kcalls: demo/kcalls.c
	@mkdir -p build/$(ARCH)
	$(QCC) $(QCFLAGS) -o $@ demo/kcalls.c

SAT_TABLE ?= ../ref/sat/kercall_table_Events.txt
gen:
	python3 -I tools/gen_events.py $(SAT_TABLE) src

clean:
	rm -rf build
