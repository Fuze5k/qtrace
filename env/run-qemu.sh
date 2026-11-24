#!/bin/sh
# Boot a QNX 8.0 aarch64le image made by mkqnximage (--type=qemu --arch=aarch64le)
# on macOS (or Linux), headless. mkqnximage's own runimage needs a Linux bridge;
# this uses QEMU user networking with SSH forwarded to the host.
#
# Full emulation (TCG, Cortex-A57, GICv3), as in mkqnximage's runimage. HVF does not
# work on Apple Silicon: the M-series CPU reports no GIC system-register interface, so
# QNX 8.0's startup-qemu-virt picks memory-mapped GIC callouts and asserts on GICv3
# (no GICC), and HVF has no GICv2. Checked 2026-11-24 with QEMU 11.1.3, macOS 26.4.
#
#   env/run-qemu.sh <vm-dir> start      boot, wait for SSH (console: <vm-dir>/output/console.log)
#   env/run-qemu.sh <vm-dir> stop
#   env/run-qemu.sh <vm-dir> ssh [cmd]  run a command on the target as root
#   env/run-qemu.sh <vm-dir> scp <src...> <dst>   copy to the target (dst is a target path)
#
# Environment: QEMU_CPUS (default 4), QEMU_ACCEL (default tcg; kvm on an arm64 Linux host),
# QNX_SSH_PORT (default 2222). The image's SSH key is <vm-dir>/id_ed25519 (its .pub was
# passed to mkqnximage --ssh-ident).
#
# SPDX-License-Identifier: Apache-2.0
set -eu
[ $# -ge 2 ] || { sed -n '2,20p' "$0" >&2; exit 2; }
VM=$(cd "$1" && pwd); cmd=$2; shift 2
CPUS=${QEMU_CPUS:-4}
ACCEL=${QEMU_ACCEL:-tcg}
PORT=${QNX_SSH_PORT:-2222}
PID="$VM/output/qemu.pid"
SSH_OPTS="-i $VM/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"

running() { [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null; }

case "$cmd" in
start)
    if running; then echo "already running (pid $(cat "$PID"))"; exit 0; fi
    [ -f "$VM/output/ifs.bin" ] || { echo "no $VM/output/ifs.bin: run mkqnximage first" >&2; exit 1; }
    mac=$(sed -n "s/^OPT_MACADDR='\(.*\)'/\1/p" "$VM/local/options")
    ram=$(sed -n "s/^OPT_RAM='\(.*\)'/\1/p" "$VM/local/options")
    if [ "$ACCEL" = tcg ]; then cpu=cortex-a57; else cpu=host; fi
    rm -f "$VM/output/console.log"
    qemu-system-aarch64 \
        -machine virt,gic-version=3 -accel "$ACCEL" -cpu "$cpu" -smp "$CPUS" -m "${ram:-1G}" \
        -drive file="$VM/output/disk-qemu.vmdk",if=none,id=drv0 -device virtio-blk-device,drive=drv0 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$PORT"-:22 -device virtio-net-device,netdev=net0,mac="$mac" \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-device,rng=rng0 \
        -kernel "$VM/output/ifs.bin" \
        -display none -monitor none -serial file:"$VM/output/console.log" \
        -daemonize -pidfile "$PID"
    i=0
    until ssh $SSH_OPTS -p "$PORT" root@127.0.0.1 true 2>/dev/null; do
        i=$((i + 1))
        if [ $i -ge 60 ] || ! running; then
            echo "no SSH after ${i} tries; console:" >&2; tail -30 "$VM/output/console.log" >&2; exit 1
        fi
        sleep 2
    done
    echo "QNX is up: ssh on 127.0.0.1:$PORT (pid $(cat "$PID"))"
    ;;
stop)
    if running; then
        ssh $SSH_OPTS -p "$PORT" root@127.0.0.1 shutdown >/dev/null 2>&1 || true
        sleep 3
        running && kill "$(cat "$PID")" 2>/dev/null || true
    fi
    rm -f "$PID"
    ;;
ssh)
    exec ssh $SSH_OPTS -p "$PORT" root@127.0.0.1 "$@"
    ;;
scp)
    n=$#; dst=$(eval echo "\${$n}")
    files=""; i=1
    while [ $i -lt $n ]; do files="$files $(eval echo "\${$i}")"; i=$((i + 1)); done
    # shellcheck disable=SC2086
    exec scp -O $SSH_OPTS -P "$PORT" $files root@127.0.0.1:"$dst"
    ;;
*)
    echo "unknown command: $cmd" >&2; exit 2
    ;;
esac
