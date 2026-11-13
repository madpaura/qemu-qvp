/*
 * qvp stub firmware: SoC map and register helpers
 *
 * Internal test firmware for -M qvp, not for upstream.
 */
#ifndef QVP_H
#define QVP_H

#include <stdint.h>

#define NUM_CORES       5

#define REG32(addr)     (*(volatile uint32_t *)(addr))
#define REG8(addr)      (*(volatile uint8_t *)(addr))

/* Per-core local view */
#define LOCAL_ITCM      0x00000000u
#define LOCAL_DTCM      0x00040000u

/* GICv2 */
#define GICD_BASE       0x00400000u
#define GICC_BASE       0x00401000u
#define GICD_CTLR       REG32(GICD_BASE + 0x000)
#define GICD_ISENABLER(n) REG32(GICD_BASE + 0x100 + 4 * (n))
#define GICD_ISPENDR(n) REG32(GICD_BASE + 0x200 + 4 * (n))
#define GICD_ITARGETSR(id) REG8(GICD_BASE + 0x800 + (id))
#define GICD_SGIR       REG32(GICD_BASE + 0xF00)
#define GICC_CTLR       REG32(GICC_BASE + 0x00)
#define GICC_PMR        REG32(GICC_BASE + 0x04)
#define GICC_IAR        REG32(GICC_BASE + 0x0C)
#define GICC_EOIR       REG32(GICC_BASE + 0x10)

/* INTIDs; must match QVP_PPI_* in hw/arm/qvp.c */
#define INTID_UART      16      /* PPI: each core's own UART */
#define INTID_TEST_SPI  48      /* 48 + n is routed to core n by the test */
#define SGI_FROM_MGMT   1
#define SGI_RING        2

/* PL011 stand-in for elan_uart; every core has its own at this address */
#define UART_BASE       0x00D12000u
#define UART_DR         REG32(UART_BASE + 0x00)
#define UART_FR         REG32(UART_BASE + 0x18)
#define UART_CR         REG32(UART_BASE + 0x30)
#define UART_IMSC       REG32(UART_BASE + 0x38)
#define UART_ICR        REG32(UART_BASE + 0x44)
#define UART_FR_RXFE    (1u << 4)
#define UART_FR_TXFF    (1u << 5)
#define UART_CR_LBE     (1u << 7)                 /* loopback */
#define UART_IMSC_RX    ((1u << 4) | (1u << 6))   /* RX + RX timeout */

/* PMU stub (placeholder layout, see hw/arm/qvp.c) */
#define PMU_CORE_RELEASE REG32(0x00D03000u)

/* State shared by all cores, in core_sram0 */
struct shared {
    volatile uint32_t phase;
    volatile uint32_t ready[NUM_CORES];
    volatile uint32_t sgi_hits[NUM_CORES][NUM_CORES];  /* [receiver][sender] */
    volatile uint32_t spi_hits[NUM_CORES][NUM_CORES];  /* [receiver][spi n] */
    volatile uint32_t uart_rx_hits[NUM_CORES];   /* [receiver] */
    volatile uint32_t unexpected[NUM_CORES];     /* stray INTIDs */
};
#define SHARED ((struct shared *)0x00C00000u)

static const char *const core_name[NUM_CORES] = {
    "aux", "mgmt", "pf0", "pf1", "fcore0"
};

#endif
