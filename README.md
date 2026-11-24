# qtrace — strace for the QNX microkernel

`qtrace` shows every kernel call a QNX OS 8.0 process makes — and, because on QNX
almost every POSIX call is a **message to a server process**, it turns each
`MsgSend()` back into the operation it carries (`open`, `read`, `write`, `devctl`,
`close` …) and tells you **which server answered, how long the message waited
for that server, how long the server worked on it, and at which priority**.

```
# qtrace -f -c sh -c "ls /proc/boot | wc -l"
  0.011195 sh[671766:1] open("/", pipe) on coid 0x40000000 = redirect 0x800b0000 <66.0us>  -> procnto[1] queued 21.0us, served 20.0us, prio 10
  0.011266 sh[671766:1] ConnectAttach(pid=122889 (pipe), chid=1, index=0, flags=64) = 1 <5.0us>
  0.011274 sh[671766:1] open("/", pipe) on fd 1 = 0 <167.0us>  -> pipe[122889] queued 13.0us, served 125.0us, prio 10
  0.012330 sh[671766:1] fork(coid 0x40000000) = 671767 <2003.0us>  -> procnto[1] queued 12.0us, served 1948.0us, prio 10
  0.014108 ls[671767:1] spawn(coid 0x40000000) = 0 <227.0us>  -> procnto[1] queued 9.0us, served 193.0us, prio 10
  ...
  0.041596 wc[671768:1] read(fd 0, 4096 bytes) = 641 <14631.0us>  -> pipe[122889] queued 29.0us, served 14567.0us, prio 10
  0.056013 ls[671767:1] write(fd 1, 641 bytes) = 641 <232.0us>  -> pipe[122889] queued 19.0us, served 189.0us, prio 10
  ...
% time   total us    calls  errors    avg us    max us  server           operation
 30.65    39614.0        2       2   19807.0   39442.0  (kernel)         SignalSuspend
 14.51    18755.9      308     117      60.9     494.0  procnto          combine
 11.53    14908.0        2       0    7454.0   14631.0  pipe             read
 10.32    13343.1       89       0     149.9    1730.0  procnto          mmap
```
<sub>Real output on QNX 8.0 in QEMU (aarch64le), trimmed. Full log and HTML report:
[`examples/sh-pipeline.txt`](examples/sh-pipeline.txt),
[`examples/sh-pipeline.html`](examples/sh-pipeline.html).</sub>

Each line is one kernel call: time, process and thread, the call, its result
and how long it took. A `MsgSend()` is shown as the operation it carries, with
the server that answered: here `wc` spends 14.6 ms in a `read()` that the
`pipe` server holds until `ls` has written its output.

`qtrace -H report.html` also writes a self-contained HTML report — a timeline
with the client's calls on top, the server threads that handled them below and
the messages drawn as arrows between them — that opens in any browser, so a
trace taken on the target can be read on a Mac or a laptop with no QNX tools.

## Why

Linux developers reach for `strace` first. QNX has no equivalent: its answer is
the System Analysis Toolkit — `tracelogger` to record every event in the whole
system and `traceprinter` or the Momentics System Profiler to read the result.
That is powerful but heavy for the everyday question *"what is my program doing
and what is it waiting for?"*, and the raw trace speaks in kernel terms
(`MSG_SENDV coid:0x3 msg:0x00000102`) rather than in terms of your code
(`write(fd 3, 42 bytes) -> devb-virtio`).

On a microkernel the interesting part of a slow call is almost never the kernel
call itself — it is the server on the other end. `qtrace` makes that visible.

## How it works

1. **Capture.** `qtrace cmd` starts `tracelogger` in daemon mode, creates `cmd`
   held (`SPAWN_HOLD`), then configures the instrumented kernel with
   `TraceEvent()`: kernel-call events only for the traced process
   (`_NTO_TRACE_SETCLASSPID`), thread, message-passing and process events for
   everyone, path-manager events, wide mode. It releases the process, waits for
   it and stops the trace.
2. **Decode.** The `.kev` file is read with QNX's `libtraceparser`. The engine
   ([`src/qt_engine.c`](src/qt_engine.c)) tracks which thread runs on each CPU
   (kernel-call events carry no pid), pairs each `KER_CALL` with its
   `KER_EXIT`, and joins the message-passing events of both sides:
   `COMM_SMSG` on the client (receive id, server pid), `COMM_RMSG` when the
   server's `MsgReceive()` picks the message up, `COMM_REPLY` when it answers.
3. **Explain.** The first 12 bytes of every message (wide mode) give the
   resource-manager message type, subtype and first argument
   ([`src/qt_msg.c`](src/qt_msg.c)); messages to procnto itself (types below
   0x100: `mmap`, `munmap`, `fork`, `spawn`, `sysconf` ...) are named from the
   SDP headers. `SYS_PATHMGR` events give the path name of
   `open()`/`stat()`/`unlink()`. When procnto's path manager answers a connect
   with a redirect (`_IO_CONNECT_RET_LINK`), the C library asks each server on
   the list in turn; qtrace carries the path along that walk.
   `ConnectAttach()` maps connection ids to servers.

Argument names and layouts of the 84 documented kernel calls and 76 trace
events come from the event table of the *System Analysis Toolkit User's Guide*,
turned into C tables by [`tools/gen_events.py`](tools/gen_events.py). The 9 kernel
calls in `<sys/kercalls.h>` that the table does not list (`MsgReceivePulse`,
`ConnectDupFd`, `InterruptUnblock`, `InterruptCtl`, `PipeOpen`, `PipeCtl`,
`PipeClose` and two without a public wrapper) are shown by name and result.

## Evidence of QNX use

| What | Where |
|---|---|
| Configuring the instrumented kernel (`TraceEvent()` filters, wide mode, start/stop) | [`src/qtrace.c`](src/qtrace.c) `arm_tracing()`, `start_tracing()` |
| Starting the traced process held with QNX `spawnp()` + `SPAWN_HOLD` | [`src/qtrace.c`](src/qtrace.c) `main()` |
| Reading `.kev` files with `libtraceparser` callbacks, CPU from `_NTO_TRACE_GETCPU()` | [`src/qt_kev.c`](src/qt_kev.c) |
| Decoding QNX message passing (MsgSend/MsgReceive/MsgReply, rcvid, coid, side channels, priority inheritance) | [`src/qt_engine.c`](src/qt_engine.c) |
| Resource-manager message types checked at compile time against `<sys/iomsg.h>` | [`src/qt_msg.c`](src/qt_msg.c) |
| procnto's own messages (`_MEM_*`, `_PROC_*`, `_SYS_*`, `_PATH_*`) and `_FTYPE_*` checked against the SDP headers | [`src/qt_msg.c`](src/qt_msg.c) `qt_sysmsg_name()`, `qt_ftype_name()` |
| Every `__KER_*` kernel call of `<sys/kercalls.h>` registered with `traceparser_cs()` | [`src/qt_kev.c`](src/qt_kev.c) `register_all()`, [`src/qt_kcalls_qnx.h`](src/qt_kcalls_qnx.h) |
| Following procnto's path-manager redirect across servers | [`src/qt_engine.c`](src/qt_engine.c) `on_kenter()`, `on_system()` |
| Booting the `mkqnximage` QEMU image on a Mac | [`env/run-qemu.sh`](env/run-qemu.sh) |
| Real traces taken on QNX 8.0 (QEMU aarch64le) | [`examples/`](examples/), see "Checked on QNX 8.0" below |
| Clock rate from the system page (`SYSPAGE_ENTRY(qtime)`) | [`src/qtrace.c`](src/qtrace.c) |
| Cross-compiling with `qcc -Vgcc_ntoaarch64le` against SDP 8.0 | [`Makefile`](Makefile), [`env/qnx`](env/qnx) |

## Build and run

Targets **QNX SDP 8.0**, target **QEMU aarch64le** (image built by
`mkqnximage`), toolchain **qcc / gcc_ntoaarch64le** from the SDP 8.0 Linux host
tools.

```sh
# Linux host with the SDP installed
source ~/qnx800/qnxsdp-env.sh
make qnx                      # -> build/aarch64le/qtrace

# macOS (also Apple Silicon): the SDP's Linux host tools run in Docker
env/qnx make qnx
env/qnx make demo             # -> build/aarch64le/kcalls

# Unit tests of the decoder, on any host, no SDP needed
make test
```

### Run on QEMU (checked on an M3 Mac: QEMU 11.1, macOS 26.4)

Build a QNX 8.0 image with the SDP's `mkqnximage` (it needs the packages
`com.qnx.qnx800.target.qemuvirt`, `...host.mkqnximage` and the virtio drivers),
then boot it headless with SSH forwarded to `127.0.0.1:2222`:

```sh
mkdir -p vm && ssh-keygen -t ed25519 -N '' -f vm/id_ed25519
(cd vm && ../env/qnx mkqnximage --type=qemu --arch=aarch64le --cpu=4 --ram=1G \
    --hostname=qtrace --ssh-ident="$PWD/id_ed25519.pub" --noprompt --build)
env/run-qemu.sh vm start          # ~60 s to SSH
```

`run-qemu.sh` uses full emulation (TCG): QNX 8.0's QEMU startup does not boot
under Apple's Hypervisor framework (see the script's header). Copy `qtrace` and
`libtraceparser.so.1` (not in the default image) to the target and run as root:

```sh
env/run-qemu.sh vm ssh mkdir -p /data/qtrace
env/run-qemu.sh vm scp build/aarch64le/qtrace \
    ~/qnx800/target/qnx/aarch64le/usr/lib/libtraceparser.so.1 /data/qtrace/
env/run-qemu.sh vm ssh 'LD_LIBRARY_PATH=/data/qtrace /data/qtrace/qtrace -c -H /tmp/ls.html ls /proc/boot'
env/run-qemu.sh vm stop
```

## Checked on QNX 8.0

All runs on QNX 8.0.0 (`uname`: `QNX qtrace 8.0.0 ... QEMU_virt aarch64le`), 4
virtual CPUs, built by `env/qnx make qnx demo`:

- `qtrace -f -c -H sh-pipeline.html sh -c "ls /proc/boot | wc -l"`: exit 0;
  `sh`, `ls` and `wc` make 1476 kernel calls and 703 messages to 3 servers
  ([`examples/sh-pipeline.txt`](examples/sh-pipeline.txt),
  [`examples/sh-pipeline.html`](examples/sh-pipeline.html)).
- The same `.kev` files read with the SDP's `traceprinter` give the same
  arguments, errors and return values for every call compared: for example
  `CONNECT_ATTACH ... pid:45063 chid:3` (qtrace:
  `ConnectAttach(pid=45063 (devb-virtio), chid=3, ...)`) and
  `MSG_SENDV ret_val:-1, errno:2` (qtrace: `= -1 errno 2`).
- [`demo/kcalls.c`](demo/kcalls.c) makes six of the kernel calls the event
  table does not document. qtrace shows all six
  ([`examples/kcalls.txt`](examples/kcalls.txt)):

  ```
  0.005680 kcalls[704534:1] MsgReceivePulse() = 0 <22.0us>
  0.005745 kcalls[704534:1] InterruptUnblock() = -1 errno 3 (No such process) <36.0us>
  0.005829 kcalls[704534:1] InterruptCtl() = -1 errno 89 (Function not implemented) <9.0us>
  0.005881 kcalls[704534:1] PipeOpen() = -1 errno 14 (Bad address) <100.0us>
  0.006025 kcalls[704534:1] PipeCtl() = -1 errno 22 (Invalid argument) <19.0us>
  0.006082 kcalls[704534:1] PipeClose() = -1 errno 3 (No such process) <18.0us>
  ```

  `traceprinter` prints `MSG_RECEIVEPULSEV/24` but leaves out the other five
  (kernel calls 61, 62, 98, 99 and 100) altogether.

## Codelab

[`codelab/qnx-message-tracing.md`](codelab/qnx-message-tracing.md) is a step-by-step tutorial in the
format of the [QNX codelabs](https://qnx.github.io/codelabs/): *What is my QNX program waiting for?*
It builds [`codelab/msgwait.c`](codelab/msgwait.c), a 300-line tracer that shows how the send, receive and
reply events of the instrumented kernel fit together, using only `TraceEvent()`, `tracelogger` and
`libtraceparser` (`env/qnx make codelab`).

## Usage

```
qtrace [-cfDv] [-o file] [-H report.html] [-k file.kev] command [arg ...]
qtrace [-cfDv] [-o file] [-H report.html] [-k file.kev] -p pid [-s seconds]
qtrace [-cfDv] [-o file] [-H report.html] [-p pid] -i file.kev

  -c          print a summary: time per server and operation
  -f          follow processes created by the traced process
  -D          deep: also record what servers do while serving the traced process
  -v          verbose: print the server-side calls recorded by -D
  -o file     write the call log to file (default: stderr)
  -H file     write a self-contained HTML report with a timeline
  -k file     keep the raw kernel trace (.kev) in file
  -i file     decode an existing .kev file instead of tracing
  -p pid      trace this process (with -i: show only this process)
  -s seconds  with -p: stop after this many seconds (default: Ctrl-C)
  -b buffers  kernel trace buffers per CPU (default 32)
```

## Project layout

```
src/qtrace.c          command line, capture (QNX only)
src/qt_kev.c          libtraceparser adapter (QNX only)
src/qt_engine.c       portable decoder: events -> calls, servers, timing
src/qt_msg.c          resource-manager and procnto message names
src/qt_report.c       HTML report writer; template in report/template.html
src/qt_events_gen.*   generated kernel-call and event tables
tests/                decoder tests on synthetic traces (run on any host)
demo/kcalls.c         makes kernel calls the SAT event table leaves out
codelab/              the codelab and its program, msgwait.c
examples/             real qtrace output from QNX 8.0 in QEMU
env/                  Docker build environment for the SDP host tools; QEMU runner
tools/                generators for the tables and the embedded report
```

## Licence

Apache License 2.0 — see [LICENSE](LICENSE).
