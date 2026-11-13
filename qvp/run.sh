#!/bin/sh
# Run the qvp board with one ELF per core.
#
#   qvp/run.sh [--gdb] [--paused] [--no-wait] [--fw DIR] [-- extra qemu args]
#
#   --gdb     per-core gdb servers, core n on port $QVP_GDB_PORT + n
#             (default 6234: 6234 aux, 6235 mgmt, 6236 pf0, 6237 pf1,
#             6238 fcore0)
#   --paused  every core starts paused at its reset vector
#   --no-wait don't wait for the consoles to connect
#   --fw DIR  directory with core0.elf .. core4.elf (default qvp/fw/build)
#
# Every core's UART is its own console: a unix socket in $QVP_RUNDIR
# (default /tmp/qvp-$USER) that qvp/console.sh connects to, one terminal
# per core ("qvp/console.sh all" opens all five in tmux). QEMU waits for
# all five consoles before starting, so no output is lost. Everything a
# core prints is also logged to $QVP_RUNDIR/<core>.log.
#
# Monitor: socat -,raw,echo=0 unix-connect:$QVP_RUNDIR/monitor.sock
#          e.g. "qvp-core status", "qvp-core resume all", "quit"
set -e

TOP=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$TOP/build/qemu-system-arm}
FW=$TOP/qvp/fw/build
GDB_PORT=${QVP_GDB_PORT:-6234}
RUNDIR=${QVP_RUNDIR:-/tmp/qvp-$(id -un)}
MACHINE=qvp
WAIT=on

while [ $# -gt 0 ]; do
    case "$1" in
    --gdb)    MACHINE="$MACHINE,gdb-port=$GDB_PORT" ;;
    --paused) MACHINE="$MACHINE,start-paused=on" ;;
    --no-wait) WAIT=off ;;
    --fw)     FW=$2; shift ;;
    --)       shift; break ;;
    *)        echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done

mkdir -p "$RUNDIR"

# core n: its ELF and its console (n-th -serial)
ARGS=
n=0
for name in aux mgmt pf0 pf1 fcore0; do
    ARGS="$ARGS -device loader,file=$FW/core$n.elf,cpu-num=$n"
    ARGS="$ARGS -chardev socket,id=con-$name,path=$RUNDIR/$name.sock"
    ARGS="$ARGS,server=on,wait=$WAIT,logfile=$RUNDIR/$name.log"
    ARGS="$ARGS -serial chardev:con-$name"
    n=$((n + 1))
done

echo "qvp: consoles and logs in $RUNDIR, monitor on $RUNDIR/monitor.sock" >&2
if [ $WAIT = on ]; then
    echo "qvp: waiting for the consoles (qvp/console.sh all, or the" \
         "\"qvp: consoles\" task in VS Code)" >&2
fi
# shellcheck disable=SC2086
exec "$QEMU" -M "$MACHINE" -accel tcg,thread=multi -display none \
    -monitor "unix:$RUNDIR/monitor.sock,server=on,wait=off" \
    $ARGS "$@"
