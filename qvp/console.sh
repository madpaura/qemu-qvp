#!/bin/sh
# Terminal for one qvp core's console (UART).
#
#   qvp/console.sh <aux|mgmt|pf0|pf1|fcore0|0..4>
#   qvp/console.sh all          all five in a tmux session ("qvp-consoles")
#
# Start it before or after qvp/run.sh: it keeps reconnecting, so it can
# stay open across QEMU runs. Keystrokes go to the core; Ctrl-C quits.
set -e

SELF=$(cd "$(dirname "$0")" && pwd)/console.sh
RUNDIR=${QVP_RUNDIR:-/tmp/qvp-$(id -un)}
CORES="aux mgmt pf0 pf1 fcore0"

case "$1" in
0) name=aux ;;
1) name=mgmt ;;
2) name=pf0 ;;
3) name=pf1 ;;
4) name=fcore0 ;;
aux|mgmt|pf0|pf1|fcore0) name=$1 ;;
all)
    session=qvp-consoles
    if ! tmux has-session -t $session 2>/dev/null; then
        for c in $CORES; do
            cmd="QVP_RUNDIR='$RUNDIR' '$SELF' $c"
            if [ $c = aux ]; then
                pane=$(tmux new-session -d -s $session -n consoles \
                       -x 200 -y 50 -P -F '#{pane_id}' "$cmd")
            else
                pane=$(tmux split-window -t $session -P -F '#{pane_id}' "$cmd")
            fi
            tmux select-pane -t "$pane" -T "$c"
            tmux select-layout -t $session tiled >/dev/null
        done
        tmux set -t $session pane-border-status top
        tmux set -t $session pane-border-format ' #{pane_title} '
    fi
    if [ -n "$TMUX" ]; then
        exec tmux switch-client -t $session
    fi
    exec tmux attach -t $session
    ;;
*)
    echo "usage: $0 <aux|mgmt|pf0|pf1|fcore0|0..4|all>" >&2
    exit 1
    ;;
esac

sock=$RUNDIR/$name.sock

# Pass keystrokes through one by one; the firmware does the echo
if [ -t 0 ]; then
    saved=$(stty -g)
    trap 'stty "$saved"' EXIT
    trap 'exit 130' INT TERM
    stty -icanon -echo
fi

echo "qvp console $name: waiting for qvp on $sock"
while :; do
    if socat - UNIX-CONNECT:"$sock" 2>/dev/null; then
        printf '\n--- qvp console %s: disconnected, waiting ---\n' "$name"
    fi
    sleep 0.2
done
