/*
 * qvp stub firmware: console + interrupt routing check
 *
 * One ELF per core, built with -DCORE_ID=n. mgmt (core 1) boots, sets up
 * the UART and GIC distributor, releases the other cores via the PMU and
 * then checks that every interrupt lands on the core it was routed to:
 *   phase 1: mgmt sends an SGI to each core (GICD_SGIR target list)
 *   phase 2: SPI 48+n is targeted at core n (GICD_ITARGETSR) and pended
 *   phase 3: every core sends an SGI to the next one (ring)
 * Afterwards the UART RX interrupt (INTID 32, routed to mgmt) echoes
 * whatever is typed on the console.
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

static inline uint32_t irq_save(void)
{
    uint32_t flags;

    __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(flags) : "memory");
}

static inline uint32_t read_mpidr(void)
{
    uint32_t v;

    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(v));
    return v;
}

static void spin_lock(volatile uint32_t *lock)
{
    uint32_t tmp;

    __asm__ volatile(
        "1: ldrex   %0, [%1]\n"
        "   cmp     %0, #0\n"
        "   bne     1b\n"
        "   strex   %0, %2, [%1]\n"
        "   cmp     %0, #0\n"
        "   bne     1b\n"
        "   dmb\n"
        : "=&r"(tmp) : "r"(lock), "r"(1) : "cc", "memory");
}

static void spin_unlock(volatile uint32_t *lock)
{
    __asm__ volatile("dmb" ::: "memory");
    *lock = 0;
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

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
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

/* Print one line tagged with this core; lines from cores never interleave */
static void cprintf(const char *fmt, ...)
{
    uint32_t flags = irq_save();
    va_list ap;

    spin_lock(&SHARED->uart_lock);
    uart_puts("[");
    uart_puts(core_name[ME]);
    uart_puts("] ");
    va_start(ap, fmt);
    vprint(fmt, ap);
    va_end(ap);
    spin_unlock(&SHARED->uart_lock);
    irq_restore(flags);
}

/* GIC */

static void gic_cpu_init(void)
{
    GICD_ISENABLER(0) = 0x0000ffff;     /* SGIs, banked per core */
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
    cprintf("          SGIs from: aux mgmt pf0 pf1 fcore0 | SPI 48+n: 0 1 2 3 4\n");
    for (int r = 0; r < NUM_CORES; r++) {
        int row_ok = !SHARED->unexpected[r];

        for (int s = 0; s < NUM_CORES; s++) {
            /* one SGI from mgmt (phase 1), one from the previous core */
            uint32_t want_sgi = (s == MGMT) + (r == (s + 1) % NUM_CORES);
            uint32_t want_spi = (s == r);

            row_ok &= SHARED->sgi_hits[r][s] == want_sgi;
            row_ok &= SHARED->spi_hits[r][s] == want_spi;
        }
        cprintf("  %6s               %u   %u    %u   %u   %u     |"
                "           %u %u %u %u %u  %s\n", core_name[r],
                SHARED->sgi_hits[r][0], SHARED->sgi_hits[r][1],
                SHARED->sgi_hits[r][2], SHARED->sgi_hits[r][3],
                SHARED->sgi_hits[r][4],
                SHARED->spi_hits[r][0], SHARED->spi_hits[r][1],
                SHARED->spi_hits[r][2], SHARED->spi_hits[r][3],
                SHARED->spi_hits[r][4], row_ok ? "ok" : "WRONG");
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

    UART_CR = 0x301;                    /* UARTEN | TXE | RXE */
    UART_IMSC = UART_IMSC_RX;

    GICD_CTLR = 1;
    for (int n = 0; n < NUM_CORES; n++) {
        gic_route_spi(INTID_TEST_SPI + n, n);
    }
    gic_route_spi(INTID_UART, MGMT);
    gic_cpu_init();

    cprintf("qvp stub firmware, MPIDR.Aff0=%u\n", read_mpidr() & 0xff);
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
    cprintf("type on the console: UART RX (INTID %u) is routed to mgmt\n",
            INTID_UART);

    for (;;) {
        __asm__ volatile("wfi");
        while (rx_tail != rx_head) {
            char c = rx_buf[rx_tail++ % sizeof(rx_buf)];

            cprintf("rx '%c' (0x%02x), uart irqs: mgmt %u, others %u\n",
                    c >= ' ' && c < 127 ? c : '.', (unsigned)c,
                    SHARED->uart_rx_hits[MGMT],
                    SHARED->uart_rx_hits[0] + SHARED->uart_rx_hits[2] +
                    SHARED->uart_rx_hits[3] + SHARED->uart_rx_hits[4]);
        }
    }
}

static void other_main(void)
{
    uint32_t aff0 = read_mpidr() & 0xff;

    gic_cpu_init();
    cprintf("online, MPIDR.Aff0=%u%s\n", aff0,
            aff0 == ME ? "" : " (expected core id!)");
    SHARED->ready[ME] = 1;

    while (SHARED->phase < 3) {
        /* mgmt drives phases 1 and 2 */
    }
    send_sgi(SGI_RING, (ME + 1) % NUM_CORES);

    for (;;) {
        __asm__ volatile("wfi");
    }
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
