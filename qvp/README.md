# qvp board (internal)

Single-process QEMU model of the qvp SoC: 5 Cortex-R5F cores (aux, mgmt,
pf0, pf1, fcore0), each with private ITCM/DTCM/QCC and its own console
UART, one shared SoC map.
Board code: `hw/arm/qvp.c`. Internal only, not for upstream.

## Build and run

    ninja -C build qemu-system-arm      # once: ../configure --target-list=arm-softmmu
    make -C qvp/fw                      # stub firmware, qvp/fw/build/core0..4.elf
    qvp/console.sh all                  # terminal 1: one console per core (tmux)
    qvp/run.sh                          # terminal 2: the board
    qvp/run.sh --gdb                    # + gdb server per core, port 6234 + core
    qvp/run.sh --gdb --paused           # every core waits at its reset vector

Your own images: `qvp/run.sh --fw DIR` (expects `core0.elf` .. `core4.elf`),
or call QEMU directly with `-device loader,file=X.elf,cpu-num=N`.

Port 6234 is the default because the multi-process setup uses 1234-1238;
override with `QVP_GDB_PORT`.

## Consoles: one terminal per core

Every core has its own UART at 0xD12000 (PL011 placeholder, interrupt
PPI 16, which only reaches that core), so each core's log goes to its own
terminal:

    qvp/console.sh mgmt                 # one core per terminal (or 0..4) ...
    qvp/console.sh all                  # ... or all five as tmux panes

Consoles keep reconnecting, so leave them open across QEMU runs. Keys you
type go to that core only; Ctrl-C closes the console. `qvp/run.sh` waits
until all five consoles are connected so no output is lost; use
`--no-wait` to run without them. Each core's output is also written to
`/tmp/qvp-$USER/<core>.log` (overwritten each run), next to the sockets
and `monitor.sock`; `QVP_RUNDIR` moves that directory.

## Cores, ports, control

| core | name   | gdb port | released by          |
|------|--------|----------|----------------------|
| 0    | aux    | 6234     | PMU CORE_RELEASE bit0 |
| 1    | mgmt   | 6235     | boots from reset      |
| 2    | pf0    | 6236     | bit2                 |
| 3    | pf1    | 6237     | bit3                 |
| 4    | fcore0 | 6238     | bit4                 |

Monitor: `socat -,raw,echo=0 unix-connect:/tmp/qvp-$USER/monitor.sock`
(or `monitor ...` from any gdb):

    qvp-core status | start | hold | reset | pause | resume  [core|all]

A breakpoint, step or Ctrl-C in one core's gdb stops only that core.

## VS Code (Cortex-Debug extension)

- **qvp: debug all cores**: builds, opens the five console terminals (one
  split terminal group), starts QEMU with all cores paused and attaches
  one debug session per core. Continue each core from its own
  session (cores you don't continue stay paused).
- **qvp: \<core\>**: attach to one core of an already running
  `qvp/run.sh --gdb`.
- Tasks: `qvp: consoles` (or `qvp: console <core>`), `qvp: run`,
  `qvp: monitor`, `qvp: status`, `qvp: resume all cores`, `qvp: stop`.

## Stub firmware (`qvp/fw`)

Each core prints to its own console and loops one byte back through its
UART, which must interrupt that core. mgmt releases the other cores via
the PMU and checks interrupt routing through the GIC (SGIs from mgmt to
each core, SPI 48+n targeted at core n, an SGI ring between all cores,
the UART interrupts), printing a table and PASS/FAIL on its console.
Characters typed on a core's console interrupt that core and are echoed.

## Placeholders (replace when specs are available)

- UART: one PL011 per core at 0xD12000, PPI 16, stands in for elan_uart.
  On the real SoC it is one shared UART (SPI); per-core instances keep
  the logs apart, like the multi-process setup did.
- PMU core control at 0xD03000: CORE_RELEASE/HOLD/RESET at 0x0/0x4/0x8.
- GIC: stock GICv2 (dist 0x400000, cpu 0x401000), no elan_gic changes.
- Other elan_* peripherals are unimplemented stubs (`-d unimp` logs them).
