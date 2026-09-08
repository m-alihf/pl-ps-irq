# Zynq-7000 AMP: a 12.5 us PL interrupt that never slips, next to lwIP

CPU0 services a 12.5 us (80 kHz) interrupt from the PL and measures its own timing.
CPU1 runs bare-metal lwIP over the same DDR and the same L2 cache.

Target: Mitra-7020, xc7z020clg400-2, 512 MB DDR, UART0 on MIO 10..11.
Tools: Vivado 2019.1 + Xilinx SDK 2019.1.

## Result

Measured on hardware, 4 min 17 s of uninterrupted running with lwIP plus a 1 MB
DDR thrash loop on CPU1, and no debugger attached:

| | |
|---|---|
| interrupts serviced | 20,560,000 |
| interrupts missed | **0** |
| mean period | 12499.991 ns |
| worst period | 12512 ns (**+12 ns**) |
| worst error, any sample | **21 ns** = 0.17 % of the period |
| ISR execution time | ~900 ns |

Before the fix described below: 11,130,289 of 25,613,464 interrupts missed (43 %),
and CPU0 never reached its first `xil_printf`.

## What actually causes the slip

On a Cortex-A9 MPCore one core **cannot** mask the other core's IRQ line - CPSR and
the GIC CPU interface (`ICCPMR`, `ICCICR`) are banked per core. Every real cause is
a shared resource:

| Cause | Effect | Fix |
|---|---|---|
| **CPU1 never claims its own SPIs** | `DistributorInit` on CPU0 aims every SPI at CPU0. CPU1's BSP is built with `USE_AMP=1` so it skips `DistributorInit` and only registers a handler in its private table. GEM0's IRQ 54 is therefore delivered to CPU0, hits `StubHandler`, the EMAC cause is never cleared, and the storm starves IRQ 61. | CPU0 writes `0x02` into every SPI byte of `ICDIPTR` (0xF8F01800 + id), then `0x01` for IRQ 61 only |
| CPU1 BSP built without `USE_AMP=1` | `XScuGic_CfgInitialize` re-runs `DistributorInit` and wipes CPU0's priority/target/trigger config for IRQ 61 | build the CPU1 BSP with `-DUSE_AMP=1` |
| Concurrent read-modify-write on `ICDICFR` | IRQ 54 (GEM0) and IRQ 61 (IRQ_F2P[0]) share the same 32-bit `ICDICFR` word | the handshake keeps CPU1 away from the GIC until CPU0 is done |
| `xil_printf` in the ISR | one char at 115200 baud is ~87 us, i.e. 7 interrupt periods | collect stats in the ISR, print from the main loop |
| Whole-cache maintenance on CPU1 | `Xil_DCacheFlush()` cleans by way and stalls the shared L2/DDR path for microseconds | range operations only |
| Shared L2 (PL310) + DDR | lwIP traffic evicts the ISR's lines | priority 0 for IRQ 61 was enough here; if not, L2 lockdown-by-master or move the ISR to OCM |

The first row was the real bug. It is a direct side effect of the `USE_AMP=1` that
every AMP guide tells you to set, and it is invisible without looking at the GIC.

## Layout

```
pl/pl_irq_gen.v           periodic interrupt source + SEQ counter, AXI4-Lite
cpu0/src/amp_shared.h     memory map, register addresses, two-way handshake
cpu0/src/main.c           ISR, GIC setup, timing statistics, reporting
cpu1/cpu1_changes.c.txt   edits to apply to the SDK lwIP Echo Server template
cpu1/cpu1_load.c.txt      synthetic DDR load, stands in for packet traffic
cpu1/reference/main.c     the CPU1 main.c as actually built and measured
tools/run_amp.tcl         XSCT script: reset, FPGA, ps7_init, download, run both
```

## 1. Hardware (Vivado)

1. `Add Sources` -> `pl/pl_irq_gen.v`.
2. Block design: right-click -> `Add Module` -> `pl_irq_gen`. Vivado infers the
   `s_axi` AXI4-Lite interface from the port names. If it does not, the module has
   to be packaged as IP instead.
3. Configure the Zynq PS, or run the equivalent Tcl:
   ```tcl
   set_property -dict [list CONFIG.PCW_USE_FABRIC_INTERRUPT {1} \
     CONFIG.PCW_IRQ_F2P_INTR {1} CONFIG.PCW_USE_M_AXI_GP0 {1} \
     CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100}] [get_bd_cells processing_system7_0]
   ```
   Disable any unused HP slave port, or its ACLK is left dangling.
4. Connect `pl_irq_gen/irq` to `IRQ_F2P`, run connection automation for `s_axi`.
5. Address editor: `0x43C00000`, range 4K. A different address means changing
   `PL_IRQ_GEN_BASE` in `cpu0/src/amp_shared.h`.
6. Generate bitstream, `Export Hardware` **with** *Include bitstream*.

> After re-exporting, delete the old `.bit` from the SDK hardware platform project
> and re-import, otherwise SDK keeps programming the previous bitstream.

The IRQ is a 4-cycle pulse and the GIC entry is configured rising-edge, so the ISR
needs no acknowledge write back to the PL.

## 2. SDK projects

### CPU0 - app_cpu0
- Empty Application on `ps7_cortexa9_0`.
- Copy `cpu0/src/*` into `src/`.
- `lscript.ld`: `ps7_ddr_0` base `0x00100000`, size `0x0FF00000`.
- BSP `standalone`: `stdout` = `stdin` = `ps7_uart_0`.

### CPU1 - app_cpu1
- Template `lwIP Echo Server` on `ps7_cortexa9_1`.
- BSP -> `drivers` -> `ps7_cortexa9_1`: **append to** the existing
  `extra_compiler_flags`, never replace them. The defaults carry the ARM
  architecture flags; dropping them makes the standalone assembly files fail to
  build and compiles the rest soft-float, which shows up as a wall of
  "uses VFP register arguments" plus missing `Xil_Assert`, `xil_printf`, `_sbrk`:
  ```
  -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -nostartfiles -g -Wall -Wextra -DUSE_AMP=1
  ```
  Add `-DNDEBUG` if the board has no Ethernet link: the PHY then reports speed 0
  and `XEmacPs_SetOperatingSpeed` hangs forever inside `Xil_Assert`.
- BSP -> `standalone`: `stdout` = `stdin` = `none`. One UART, and CPU0 owns it.
- `lscript.ld`: `ps7_ddr_0` base `0x10000000`, size `0x0F000000`.
- Apply `cpu1/cpu1_changes.c.txt`, plus `cpu1/cpu1_load.c.txt` when testing
  without a live network.

## 3. Run

The two-way handshake in `amp_shared.h` makes the start order irrelevant: whichever
core comes up first spins until the other reaches its half. So a single SDK Run
configuration with both ELFs works, or from the command line:

```
xsct tools/run_amp.tcl
```

Connect the serial terminal (115200 8N1) **before** starting.

```
CPU0 up. GT = 333333343 Hz, expected period = 4166.666 ticks = 12500 ns
PL generator started (WITH_CPU1=1)
n=80000 missed=0 avg=12499.991 ns  QUIET n=79090 min/max=3982/4171 (11945/12512 ns)  PRINT n=910 max=4373  isr_max=917 ns  hb=178332
  |err| quiet: <1:19577 <2:33885 <4:23665 <8:1962 <256:1
```

## 4. Reading the output

- `missed` is the ground truth. It comes from the PL `SEQ` counter, which
  increments every period regardless of what the CPU is doing, so any gap the ISR
  observes is a genuinely lost interrupt. It is **cumulative since boot**.
- `QUIET` covers samples taken while the main loop was not printing. `PRINT`
  counts those that landed inside a `xil_printf` window and reports their worst
  period separately, so the diagnostic output cannot be mistaken for real jitter.
- The global timer runs at CPU/2 (~333.33 MHz, 3 ns per tick), so 12.5 us is
  4166.67 ticks - not an integer. Samples alternating between bins `<1` and `<2`
  are that rounding, not jitter. `avg` averages over the whole window to recover
  sub-tick resolution.
- Exactly one sample per window lands in `<128` or `<256`. It is the pair that
  straddles the `st.gate` transition: the long half is counted under `PRINT`, the
  short half under `QUIET`. An artifact of the measurement, not of the design.

## 5. Debugging notes

Halting a core over JTAG while the PL generator runs inflates `missed` and shows up
as multi-millisecond `max` and `isr_max` values - the PL keeps counting while the
CPU is stopped. An `isr_max` in the milliseconds can only be a debugger halt.
Measure with nothing attached.

Useful live reads (CPU0 target, no halt needed):

| address | contents |
|---|---|
| `0x1F000000` | cpu0_ready, cpu1_ready, cpu1_heartbeat |
| `0xF8F01800 + id` | GIC CPU target byte for interrupt `id` |
| `0xF8F01104` | ICDISER1, enable bits for IRQ 32..63 |
| `0xF8F0143C` | ICDIPR, priorities for IRQ 60..63 |
| `0xF8F01C0C` | ICDICFR, trigger type for IRQ 48..63 |

`arm-none-eabi-addr2line -f -e app_cpu0.elf <pc>` turns a halted PC into a
function and source line, which is how `StubHandler` was found.

## 6. Not yet done

1. **Real TCP traffic.** The synthetic load reproduces DDR and L2 pressure but not
   the burst pattern of packets or the EMAC DMA interrupts. Needs the board on a
   switch with the echo server driven.
2. **Standalone boot.** Currently launched over JTAG. A `BOOT.BIN` with FSBL,
   bitstream and both ELFs would let it run without a host; `start_cpu1()` already
   does the `0xFFFFFFF0` + `sev` handoff for that case.
3. **`isr_max` ~900 ns** is almost entirely the three AXI-GP register reads. If the
   ISR ever needs to do more work, have the PL push the sample into the shared DDR
   window instead of being read over AXI.
