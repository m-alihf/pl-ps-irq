#ifndef AMP_SHARED_H
#define AMP_SHARED_H

#include "xil_types.h"

/* DDR map for a 512 MB board (0x00000000 - 0x1FFFFFFF).
 * Keep the two cores in disjoint regions; the last 16 MB is the shared,
 * non-cacheable window used for cross-core state. */
#define CPU0_DDR_BASE       0x00100000u   /* 0x00100000 - 0x0FFFFFFF */
#define CPU1_DDR_BASE       0x10000000u   /* 0x10000000 - 0x1EFFFFFF */
#define SHARED_BASE         0x1F000000u   /* 0x1F000000 - 0x1FFFFFFF */
#define SHARED_SIZE         0x01000000u

/* pl_irq_gen, AXI-Lite */
#define PL_IRQ_GEN_BASE     0x43C00000u
#define PL_REG_CTRL         (PL_IRQ_GEN_BASE + 0x00u)
#define PL_REG_PERIOD       (PL_IRQ_GEN_BASE + 0x04u)
#define PL_REG_SEQ          (PL_IRQ_GEN_BASE + 0x08u)
#define PL_REG_DATA         (PL_IRQ_GEN_BASE + 0x0Cu)

#define PL_IRQ_ID           61u           /* IRQ_F2P[0] */
#define PL_FCLK_HZ          100000000u
#define IRQ_PERIOD_NS       12500u
#define PL_PERIOD_CYCLES    ((u32)(((u64)PL_FCLK_HZ * IRQ_PERIOD_NS) / 1000000000ull) - 1u)

/* ARM global timer, clocked at CPU_3x2x = CPU clock / 2 */
#define GT_CNT_LO           0xF8F00200u
#define GT_CNT_HI           0xF8F00204u
#define GT_CTRL             0xF8F00208u

/* CPU1 boot address register in OCM */
#define CPU1_BOOT_ADDR_REG  0xFFFFFFF0u

/* Two-way handshake so the start order of the two cores does not matter:
 * CPU1 waits for cpu0_ready before it touches the GIC, and CPU0 waits for
 * cpu1_ready before it starts the PL generator. Whichever core the tool
 * releases first simply spins until the other catches up. */
#define AMP_MAGIC 0xA5C0FFEEu

typedef struct {
    volatile u32 cpu0_ready;
    volatile u32 cpu1_ready;
    volatile u32 cpu1_heartbeat;
    volatile u32 cpu1_pkts;     /* packets delivered to the UDP application */
    volatile u32 cpu1_bytes;    /* payload bytes copied out of the pbufs */
    volatile u32 cpu1_sum;      /* keeps the read-back from being optimised away */
} amp_sync_t;

#define AMP_SYNC ((amp_sync_t *)SHARED_BASE)

#endif /* AMP_SHARED_H */
