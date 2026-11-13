#!/bin/sh
# Run the qvp board with one ELF per core. Console (UART) is on stdio.
#
#   qvp/run.sh [--gdb] [--paused] [--fw DIR] [-- extra qemu args]
#
#   --gdb     per-core gdb servers, core n on port $QVP_GDB_PORT + n
#             (default 6234: 6234 aux, 6235 mgmt, 6236 pf0, 6237 pf1,
#             6238 fcore0)
#   --paused  every core starts paused at its reset vector
#   --fw DIR  directory with core0.elf .. core4.elf (default qvp/fw/build)
#
# Monitor: socat -,raw,echo=0 unix-connect:$QVP_MONITOR
#          e.g. "qvp-core status", "qvp-core resume all", "quit"
set -e

TOP=$(cd "$(dirname "$0")/.." && pwd)
QEMU=${QEMU:-$TOP/build/qemu-system-arm}
FW=$TOP/qvp/fw/build
GDB_PORT=${QVP_GDB_PORT:-6234}
MONITOR=${QVP_MONITOR:-/tmp/qvp-$(id -un)-monitor.sock}
MACHINE=qvp

while [ $# -gt 0 ]; do
    case "$1" in
    --gdb)    MACHINE="$MACHINE,gdb-port=$GDB_PORT" ;;
    --paused) MACHINE="$MACHINE,start-paused=on" ;;
    --fw)     FW=$2; shift ;;
    --)       shift; break ;;
    *)        echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift
done

LOADERS=
for n in 0 1 2 3 4; do
    LOADERS="$LOADERS -device loader,file=$FW/core$n.elf,cpu-num=$n"
done

echo "qvp: monitor on $MONITOR" >&2
# shellcheck disable=SC2086
exec "$QEMU" -M "$MACHINE" -accel tcg,thread=multi \
    -display none -serial stdio \
    -monitor "unix:$MONITOR,server=on,wait=off" \
    $LOADERS "$@"
