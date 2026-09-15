/* CPU0: services the 12.5 us PL interrupt and measures its timing.
 * No printing happens inside the ISR - the UART is far slower than the period.
 * Statistics are collected in the ISR and reported from the main loop. */

#include <string.h>
#include "xparameters.h"
#include "xil_io.h"
#include "xil_cache.h"
#include "xil_mmu.h"
#include "xil_exception.h"
#include "xil_printf.h"
#include "xscugic.h"
#include "xtime_l.h"          /* COUNTS_PER_SECOND */
#include "amp_shared.h"

/* 0 = run CPU0 alone to get a baseline, 1 = release CPU1 and wait for lwIP */
#define WITH_CPU1   1

#define HIST_BINS   16
#define REPORT_EVERY (1000000000u / IRQ_PERIOD_NS)   /* ~1 s worth of interrupts */

/* Expected global-timer ticks between two interrupts. The global timer runs at
 * CPU/2 (~333.33 MHz, 3 ns per tick), so 12.5 us is 4166.67 ticks - not an
 * integer. GT_EXPECTED is the truncated value used for histogram binning;
 * GT_EXPECTED_K is the same figure x1000, used for the printed average. */
#define GT_EXPECTED   ((u32)(((u64)COUNTS_PER_SECOND * IRQ_PERIOD_NS) / 1000000000ull))
#define ISR_BUSY_NS    11000u
#define ISR_BUSY_TICKS ((u32)(((u64)COUNTS_PER_SECOND * ISR_BUSY_NS) / 1000000000ull))
#define GT_EXPECTED_K ((u32)(((u64)COUNTS_PER_SECOND * IRQ_PERIOD_NS) / 1000000ull))

typedef struct {
    u32 prev_ts;
    u32 prev_seq;
    u32 count;
    u32 missed;
    u32 min_d;
    u32 max_d;
    s32 sum_res;      /* sum of (delta - GT_EXPECTED); 32-bit so main reads it atomically */
    u32 isr_max;
    u32 gate;         /* set by main while it is inside xil_printf */
    u32 gated_n;      /* samples that landed in a print window */
    u32 gated_max;
    u32 hist[HIST_BINS];
    u32 sink;
} stats_t;

/* volatile: the main loop polls st.count while the ISR updates it */
static volatile stats_t st;
static XScuGic          gic;

static u32 ticks_to_ns(u32 t)
{
    return (u32)(((u64)t * 1000000000ull) / COUNTS_PER_SECOND);
}

static void pl_irq_handler(void *arg)
{
    u32 t0   = Xil_In32(GT_CNT_LO);
    u32 seq  = Xil_In32(PL_REG_SEQ);
    u32 data = Xil_In32(PL_REG_DATA);
    u32 acc, t1, d, err, b;
    int i;

    (void)arg;

    /* ---- application work ---- */
    acc = data;
    for (i = 0; i < 8; i++) {
        acc = acc * 1664525u + 1013904223u;
    }
    st.sink += acc;

    /* hold the handler at ISR_BUSY_NS measured from entry, so the ISR occupies
     * a fixed slice of the period regardless of cache state or optimisation */
    while ((Xil_In32(GT_CNT_LO) - t0) < ISR_BUSY_TICKS) { }
    /* -------------------------- */

    if (st.count != 0u) {
        d = t0 - st.prev_ts;
        st.missed += (seq - st.prev_seq - 1u);

        if (st.gate != 0u) {
            /* main is busy in xil_printf; keep these samples out of the
             * statistics so the UART does not pollute the measurement */
            st.gated_n++;
            if (d > st.gated_max) st.gated_max = d;
        } else {
            if (d < st.min_d) st.min_d = d;
            if (d > st.max_d) st.max_d = d;
            st.sum_res += (s32)d - (s32)GT_EXPECTED;
            err = (d > GT_EXPECTED) ? (d - GT_EXPECTED) : (GT_EXPECTED - d);
            b = 0u;
            while (err != 0u && b < (HIST_BINS - 1u)) { err >>= 1; b++; }
            st.hist[b]++;
        }
    }
    st.prev_ts  = t0;
    st.prev_seq = seq;
    st.count++;

    t1 = Xil_In32(GT_CNT_LO);
    if ((t1 - t0) > st.isr_max) st.isr_max = t1 - t0;
}

/* GIC distributor CPU-target registers: one byte per interrupt ID */
#define ICDIPTR_BASE 0xF8F01800u

static void gic_setup(void)
{
    XScuGic_Config *cfg = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
    u32 id;

    XScuGic_CfgInitialize(&gic, cfg, cfg->CpuBaseAddress);

    /* DistributorInit above pointed every shared peripheral interrupt at CPU0.
     * CPU1's BSP is built with USE_AMP=1, so it skips DistributorInit and never
     * claims its own: it only registers a handler in its private table and sets
     * the enable bit. The GEM0 interrupt it enables would therefore be delivered
     * here, where no handler exists - XScuGic falls into StubHandler, the EMAC
     * cause is never cleared, and the resulting storm starves IRQ 61.
     * Hand every SPI to CPU1 and keep only the PL interrupt. */
    for (id = 32u; id < 96u; id++) {
        Xil_Out8(ICDIPTR_BASE + id, 0x02u);
    }
    Xil_Out8(ICDIPTR_BASE + PL_IRQ_ID, 0x01u);

    /* priority 0 = highest, trigger type 3 = rising edge */
    XScuGic_SetPriorityTriggerType(&gic, PL_IRQ_ID, 0x00, 0x03);
    XScuGic_Connect(&gic, PL_IRQ_ID, pl_irq_handler, NULL);
    XScuGic_Enable(&gic, PL_IRQ_ID);

    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 &gic);
    Xil_ExceptionEnable();
}

#if WITH_CPU1
static void start_cpu1(void)
{
    Xil_Out32(CPU1_BOOT_ADDR_REG, CPU1_DDR_BASE);
    dmb();
    __asm__ __volatile__ ("sev");
}
#endif

int main(void)
{
    u32 addr, last = 0u, mn, mx, ms, isr, n, nq, gn, gmx, avg_ps;
    u32 last_pkts = 0u, last_bytes = 0u, pkts, kbytes;
    u32 hist[HIST_BINS];
    s32 res;
    s64 avg_kt;
    int i;

    Xil_DCacheEnable();
    Xil_ICacheEnable();

    /* shared window and the OCM page holding the CPU1 boot address:
     * normal, non-cacheable, shareable */
    for (addr = SHARED_BASE; addr < SHARED_BASE + SHARED_SIZE; addr += 0x100000u) {
        Xil_SetTlbAttributes(addr, 0x14de2);
    }
    Xil_SetTlbAttributes(0xFFF00000u, 0x14de2);

    memset((void *)&st, 0, sizeof(st));
    st.min_d = 0xFFFFFFFFu;
    AMP_SYNC->cpu0_ready     = 0u;
    AMP_SYNC->cpu1_ready     = 0u;
    AMP_SYNC->cpu1_heartbeat = 0u;
    AMP_SYNC->cpu1_pkts      = 0u;
    AMP_SYNC->cpu1_bytes     = 0u;
    AMP_SYNC->cpu1_sum       = 0u;

    /* global timer: stop, zero, start */
    Xil_Out32(GT_CTRL, 0u);
    Xil_Out32(GT_CNT_LO, 0u);
    Xil_Out32(GT_CNT_HI, 0u);
    Xil_Out32(GT_CTRL, 1u);

    xil_printf("\r\nCPU0 up. GT = %u Hz, expected period = %u.%03u ticks = %u ns\r\n",
               (u32)COUNTS_PER_SECOND, GT_EXPECTED_K / 1000u, GT_EXPECTED_K % 1000u,
               IRQ_PERIOD_NS);

    /* All shared GIC distributor writes for this interrupt happen before CPU1
     * is released, so CPU1's own read-modify-write on ICDICFR cannot race. */
    gic_setup();
#if WITH_CPU1
    start_cpu1();                       /* no-op if a tool already started CPU1 */
    AMP_SYNC->cpu0_ready = AMP_MAGIC;   /* CPU1 may now touch the GIC */
    while (AMP_SYNC->cpu1_ready != AMP_MAGIC) { }
#endif

    Xil_Out32(PL_REG_PERIOD, PL_PERIOD_CYCLES);
    Xil_Out32(PL_REG_CTRL, 1u);
    xil_printf("PL generator started (WITH_CPU1=%d)\r\n", WITH_CPU1);

    for (;;) {
        n = st.count;
        if ((n - last) < REPORT_EVERY) {
            continue;
        }
        last = n;

        mn  = st.min_d;
        mx  = st.max_d;
        ms  = st.missed;
        isr = st.isr_max;
        res = st.sum_res;
        gn  = st.gated_n;
        gmx = st.gated_max;
        nq  = 0u;
        for (i = 0; i < HIST_BINS; i++) {
            hist[i] = st.hist[i];
            st.hist[i] = 0u;
            nq += hist[i];
        }
        st.min_d     = 0xFFFFFFFFu;
        st.max_d     = 0u;
        st.isr_max   = 0u;
        st.sum_res   = 0;
        st.gated_n   = 0u;
        st.gated_max = 0u;
        if (nq == 0u) nq = 1u;

        /* averaging over the whole window recovers sub-tick resolution */
        avg_kt = (s64)GT_EXPECTED * 1000 + ((s64)res * 1000) / (s64)nq; /* ticks x1000 */
        avg_ps = (u32)((avg_kt * 1000000000ll) / (s64)COUNTS_PER_SECOND); /* ns x1000  */

        pkts   = AMP_SYNC->cpu1_pkts  - last_pkts;
        kbytes = (AMP_SYNC->cpu1_bytes - last_bytes) / 1024u;
        last_pkts  = AMP_SYNC->cpu1_pkts;
        last_bytes = AMP_SYNC->cpu1_bytes;

        st.gate = 1u;   /* everything printed below is excluded from the stats */

        xil_printf("n=%u missed=%u avg=%u.%03u ns  QUIET n=%u min/max=%u/%u "
                   "(%u/%u ns)  PRINT n=%u max=%u  isr_max=%u ns  app=%u pkt %u KB\r\n",
                   n, ms, avg_ps / 1000u, avg_ps % 1000u, nq, mn, mx,
                   ticks_to_ns(mn), ticks_to_ns(mx), gn, gmx,
                   ticks_to_ns(isr), pkts, kbytes);

        xil_printf("  |err| quiet: ");
        for (i = 0; i < HIST_BINS; i++) {
            if (hist[i] != 0u) {
                xil_printf("<%u:%u ", (1u << i), hist[i]);
            }
        }
        xil_printf("\r\n");

        st.gate = 0u;
    }
}
