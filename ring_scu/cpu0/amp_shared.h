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
    volatile u32 dropped;       /* samples discarded because the ring was full, CPU0 */
    volatile u32 rd_ticks;      /* global-timer ticks CPU1 spent in tcp_write, summed */
    volatile u32 rd_n;          /* batches behind rd_ticks */
} amp_sync_t;

#define AMP_SYNC ((amp_sync_t *)SHARED_BASE)

/* CPU0 -> CPU1 sample ring. It lives in its own 1 MB section so its cache
 * attribute can be changed without touching the handshake words. */
#define RING_BASE   0x1F100000u
#define RING_ATTR   0x15de6u        /* write-back cacheable, shareable: the SCU keeps both L1s coherent */
#define RING_LEN    4096u           /* must be a power of two */

typedef struct {
    u32 seq;        /* PL sequence number */
    u32 data;       /* result of the ISR's work */
    u32 ts;         /* global timer at ISR entry */
    u32 cmd;        /* last command seen by CPU0 */
} sample_t;

/* The ring's control words sit in the same section as the ring, so they share
 * its cache attribute. One 32-byte cache line per writer: if head and tail
 * shared a line, every write by one core would take the line away from the
 * other. */
typedef struct {
    volatile u32 head;          /* write index, written only by CPU0 */
    u32 pad0[7];
    volatile u32 tail;          /* read index, written only by CPU1 */
    volatile u32 cmd;           /* last command from the laptop, written only by CPU1 */
    u32 pad1[6];
} ring_ctl_t;

#define RING_CTL ((ring_ctl_t *)RING_BASE)
#define RING     ((volatile sample_t *)(RING_BASE + sizeof(ring_ctl_t)))

#endif /* AMP_SHARED_H */
