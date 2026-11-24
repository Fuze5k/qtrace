# qtrace — strace for the QNX microkernel

`qtrace` shows every kernel call a QNX OS 8.0 process makes — and, because on QNX
almost every POSIX call is a **message to a server process**, it turns each
`MsgSend()` back into the operation it carries (`open`, `read`, `write`, `devctl`,
`close` …) and tells you **which server answered, how long the message waited
for that server, how long the server worked on it, and at which priority**.

```
$ qtrace -c ls /tmp
  0.000005 ls[4242:1] open("/tmp") on coid 0x40000000 = 0 <9.0us>  -> procnto[1] queued 4.0us, served 2.0us, prio 10
  0.000015 ls[4242:1] ConnectAttach(pid=8195 (devb-virtio), chid=1, index=0, flags=0) = 3 <1.0us>
  0.000017 ls[4242:1] open("/tmp") on fd 3 = 0 <28.0us>  -> devb-virtio[8195] queued 4.0us, served 21.0us, prio 10
  0.000046 ls[4242:1] write(fd 1, 42 bytes) = 42 <44.0us>  -> devc-pty[159762] queued 9.0us, served 32.0us, prio 10
  ...
% time   total us    calls  errors    avg us    max us  server           operation
 44.00       44.0        1       0      44.0      44.0  devc-pty         write
 28.00       28.0        1       0      28.0      28.0  devb-virtio      open
```
<sub>(Output from the decoder's unit test; real QEMU output will replace it.)</sub>

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
   ([`src/qt_msg.c`](src/qt_msg.c)); `SYS_PATHMGR` events give the path name of
   `open()`/`stat()`/`unlink()`; `ConnectAttach()` maps connection ids to
   servers.

Argument names and layouts of all 85 kernel calls and 76 trace events come
from the event table of the *System Analysis Toolkit User's Guide*, turned into
C tables by [`tools/gen_events.py`](tools/gen_events.py).

## Evidence of QNX use

| What | Where |
|---|---|
| Configuring the instrumented kernel (`TraceEvent()` filters, wide mode, start/stop) | [`src/qtrace.c`](src/qtrace.c) `arm_tracing()`, `start_tracing()` |
| Starting the traced process held with QNX `spawnp()` + `SPAWN_HOLD` | [`src/qtrace.c`](src/qtrace.c) `main()` |
| Reading `.kev` files with `libtraceparser` callbacks, CPU from `_NTO_TRACE_GETCPU()` | [`src/qt_kev.c`](src/qt_kev.c) |
| Decoding QNX message passing (MsgSend/MsgReceive/MsgReply, rcvid, coid, side channels, priority inheritance) | [`src/qt_engine.c`](src/qt_engine.c) |
| Resource-manager message types checked at compile time against `<sys/iomsg.h>` | [`src/qt_msg.c`](src/qt_msg.c) |
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
src/qt_msg.c          resource-manager message names
src/qt_report.c       HTML report writer; template in report/template.html
src/qt_events_gen.*   generated kernel-call and event tables
tests/                decoder tests on synthetic traces (run on any host)
env/                  Docker build environment for the SDP host tools
tools/                generators for the tables and the embedded report
```

## Licence

Apache License 2.0 — see [LICENSE](LICENSE).
