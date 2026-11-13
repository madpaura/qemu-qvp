/*
 * qvp stub firmware: console + interrupt routing check
 *
 * One ELF per core, built with -DCORE_ID=n. mgmt (core 1) boots, sets up
 * the UART and GIC distributor, releases the other cores via the PMU and
 * then checks that every interrupt lands on the core it was routed to:
 *   phase 1: mgmt sends an SGI to each core (GICD_SGIR target list)
 *   phase 2: SPI 48+n is targeted at core n (GICD_ITARGETSR) and pended
 *   phase 3: every core sends an SGI to the next one (ring)
 * Every core has its own UART (its own console/terminal). Before coming
 * up each core loops one byte back through it, which must raise the UART
 * interrupt (PPI 16) on that core only. Afterwards each core echoes what
 * is typed on its console.
 *
 * Internal test firmware for -M qvp, not for upstream.
 */
#include <stdarg.h>
#include "qvp.h"

#ifndef CORE_ID
#error "build with -DCORE_ID=n"
#endif
#define ME      CORE_ID
#define MGMT    1
#define TIMEOUT 50000000u

static volatile char rx_buf[64];
static volatile uint32_t rx_head, rx_tail;

static inline uint32_t read_mpidr(void)
{
    uint32_t v;

    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
    return v;
}

/* Console */

static void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    while (UART_FR & UART_FR_TXFF) {
    }
    UART_DR = c;
}

static void put_num(uint32_t v, unsigned base, int width, char pad)
{
    char buf[12];
    int n = 0;

    do {
        buf[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width) {
        buf[n++] = pad;
    }
    while (n) {
        uart_putc(buf[--n]);
    }
}

static void vprint(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        char pad = ' ';
        int width = 0;

        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        switch (*fmt) {
        case 'u':
            put_num(va_arg(ap, unsigned), 10, width, pad);
            break;
        case 'x':
            put_num(va_arg(ap, unsigned), 16, width, pad);
            break;
        case 's': {                     /* left-aligned in width */
            const char *str = va_arg(ap, const char *);

            for (; *str; str++, width--) {
                uart_putc(*str);
            }
            while (width-- > 0) {
                uart_putc(' ');
            }
            break;
        }
        case 'c':
            uart_putc(va_arg(ap, int));
            break;
        default:
            uart_putc(*fmt);
            break;
        }
    }
}

/* The UART is this core's own, so no locking against other cores */
static void cprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprint(fmt, ap);
    va_end(ap);
}

/* GIC */

static void gic_cpu_init(void)
{
    /* SGIs and the UART PPI, banked per core */
    GICD_ISENABLER(0) = 0x0000ffff | (1u << INTID_UART);
    GICC_PMR = 0xf0;
    GICC_CTLR = 1;
    __asm__ volatile("cpsie i" ::: "memory");
}

static void gic_route_spi(unsigned intid, unsigned core)
{
    GICD_ITARGETSR(intid) = 1u << core;
    GICD_ISENABLER(intid / 32) = 1u << (intid % 32);
}

static void send_sgi(unsigned sgi, unsigned core)
{
    GICD_SGIR = (1u << (16 + core)) | sgi;
}

void irq_handler(void);
void irq_handler(void)
{
    uint32_t iar = GICC_IAR;
    uint32_t id = iar & 0x3ff;
    uint32_t src = (iar >> 10) & 7;

    if (id == 1023) {
        return;                         /* spurious */
    }
    if (id < 16) {
        SHARED->sgi_hits[ME][src]++;
    } else if (id >= INTID_TEST_SPI && id < INTID_TEST_SPI + NUM_CORES) {
        SHARED->spi_hits[ME][id - INTID_TEST_SPI]++;
    } else if (id == INTID_UART) {
        while (!(UART_FR & UART_FR_RXFE)) {
            rx_buf[rx_head++ % sizeof(rx_buf)] = UART_DR;
        }
        SHARED->uart_rx_hits[ME]++;
    } else {
        SHARED->unexpected[ME]++;
    }
    GICC_EOIR = iar;
}

static int wait_nonzero(volatile uint32_t *p);

/*
 * Loop one byte back through this core's UART: the RX interrupt must
 * arrive here. Leaves RX interrupts enabled for the console echo.
 */
static void uart_init_and_selftest(void)
{
    int ok;

    UART_CR = 0x301;                    /* UARTEN | TXE | RXE */
    UART_IMSC = UART_IMSC_RX;
    cprintf("UART loopback ");
    UART_CR = 0x301 | UART_CR_LBE;      /* only this byte loops back */
    uart_putc('*');
    ok = wait_nonzero(&SHARED->uart_rx_hits[ME]);
    UART_CR = 0x301;
    rx_tail = rx_head;                  /* drop the loopback byte */
    cprintf(ok ? " interrupt on this core\n" : " no interrupt\n");
}

static void console_echo(void)
{
    for (;;) {
        __asm__ volatile("wfi");
        while (rx_tail != rx_head) {
            char c = rx_buf[rx_tail++ % sizeof(rx_buf)];

            cprintf("rx '%c' (0x%02x), uart irqs on %s: %u\n",
                    c >= ' ' && c < 127 ? c : '.', (unsigned)c,
                    core_name[ME], SHARED->uart_rx_hits[ME]);
        }
    }
}

static int wait_nonzero(volatile uint32_t *p)
{
    for (uint32_t i = 0; i < TIMEOUT; i++) {
        if (*p) {
            return 1;
        }
    }
    return 0;
}

/* mgmt: boot, release the others and check the routing */

static int check_routing(void)
{
    int ok = 1;

    cprintf("interrupt routing (rows = receiving core)\n");
    cprintf("          SGIs from: aux mgmt pf0 pf1 fcore0 | SPI 48+n: 0 1 2 3 4 | UART\n");
    for (int r = 0; r < NUM_CORES; r++) {
        int row_ok = !SHARED->unexpected[r];

        for (int s = 0; s < NUM_CORES; s++) {
            /* one SGI from mgmt (phase 1), one from the previous core */
            uint32_t want_sgi = (s == MGMT) + (r == (s + 1) % NUM_CORES);
            uint32_t want_spi = (s == r);

            row_ok &= SHARED->sgi_hits[r][s] == want_sgi;
            row_ok &= SHARED->spi_hits[r][s] == want_spi;
        }
        /* exactly one: its own loopback byte */
        row_ok &= SHARED->uart_rx_hits[r] == 1;
        cprintf("  %6s               %u   %u    %u   %u   %u     |"
                "           %u %u %u %u %u |   %u   %s\n", core_name[r],
                SHARED->sgi_hits[r][0], SHARED->sgi_hits[r][1],
                SHARED->sgi_hits[r][2], SHARED->sgi_hits[r][3],
                SHARED->sgi_hits[r][4],
                SHARED->spi_hits[r][0], SHARED->spi_hits[r][1],
                SHARED->spi_hits[r][2], SHARED->spi_hits[r][3],
                SHARED->spi_hits[r][4], SHARED->uart_rx_hits[r],
                row_ok ? "ok" : "WRONG");
        ok &= row_ok;
    }
    return ok;
}

static void mgmt_main(void)
{
    volatile uint32_t *p = (volatile uint32_t *)SHARED;
    int ok = 1;

    for (unsigned i = 0; i < sizeof(*SHARED) / 4; i++) {
        p[i] = 0;
    }

    GICD_CTLR = 1;
    for (int n = 0; n < NUM_CORES; n++) {
        gic_route_spi(INTID_TEST_SPI + n, n);
    }
    gic_cpu_init();

    cprintf("qvp stub firmware, mgmt console, MPIDR.Aff0=%u\n",
            read_mpidr() & 0xff);
    uart_init_and_selftest();
    cprintf("releasing aux, pf0, pf1, fcore0 via PMU CORE_RELEASE\n");
    SHARED->ready[ME] = 1;
    PMU_CORE_RELEASE = 0x1d;

    for (int n = 0; n < NUM_CORES; n++) {
        if (!wait_nonzero(&SHARED->ready[n])) {
            cprintf("core %s did not come up\n", core_name[n]);
            ok = 0;
        }
    }

    cprintf("phase 1: SGI %u from mgmt to every core\n", SGI_FROM_MGMT);
    SHARED->phase = 1;
    for (int n = 0; n < NUM_CORES; n++) {
        send_sgi(SGI_FROM_MGMT, n);
        wait_nonzero(&SHARED->sgi_hits[n][MGMT]);
    }

    cprintf("phase 2: SPI %u+n targeted at core n\n", INTID_TEST_SPI);
    SHARED->phase = 2;
    for (int n = 0; n < NUM_CORES; n++) {
        unsigned intid = INTID_TEST_SPI + n;

        GICD_ISPENDR(intid / 32) = 1u << (intid % 32);
        wait_nonzero(&SHARED->spi_hits[n][n]);
    }

    cprintf("phase 3: SGI %u ring, core n -> core n+1\n", SGI_RING);
    SHARED->phase = 3;
    send_sgi(SGI_RING, (ME + 1) % NUM_CORES);
    for (int n = 0; n < NUM_CORES; n++) {
        wait_nonzero(&SHARED->sgi_hits[(n + 1) % NUM_CORES][n]);
    }
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        /* let any misrouted extra interrupt show up */
    }

    ok &= check_routing();
    cprintf("interrupt routing: %s\n", ok ? "PASS" : "FAIL");
    cprintf("type here: this console's RX interrupt goes to mgmt only\n");
    console_echo();
}

static void other_main(void)
{
    uint32_t aff0 = read_mpidr() & 0xff;

    gic_cpu_init();
    cprintf("qvp stub firmware, %s console, MPIDR.Aff0=%u%s\n",
            core_name[ME], aff0, aff0 == ME ? "" : " (expected core id!)");
    uart_init_and_selftest();
    cprintf("online, waiting for mgmt's interrupt tests\n");
    SHARED->ready[ME] = 1;

    while (SHARED->phase < 3) {
        /* mgmt drives phases 1 and 2 */
    }
    send_sgi(SGI_RING, (ME + 1) % NUM_CORES);
    while (!SHARED->sgi_hits[ME][(ME + NUM_CORES - 1) % NUM_CORES]) {
    }
    cprintf("received %u SGIs, %u SPIs; results on the mgmt console\n",
            SHARED->sgi_hits[ME][0] + SHARED->sgi_hits[ME][1] +
            SHARED->sgi_hits[ME][2] + SHARED->sgi_hits[ME][3] +
            SHARED->sgi_hits[ME][4],
            SHARED->spi_hits[ME][0] + SHARED->spi_hits[ME][1] +
            SHARED->spi_hits[ME][2] + SHARED->spi_hits[ME][3] +
            SHARED->spi_hits[ME][4]);
    cprintf("type here: this console's RX interrupt goes to %s only\n",
            core_name[ME]);
    console_echo();
}

int main(void);
int main(void)
{
    if (ME == MGMT) {
        mgmt_main();
    } else {
        other_main();
    }
    return 0;
}
