# Zynq-7000 AMP: a 12.5 us PL interrupt that never slips, next to lwIP

CPU0 services a 12.5 us (80 kHz) interrupt from the PL and measures its own timing.
CPU1 runs bare-metal lwIP over the same DDR and the same L2 cache.

Target: Mitra-7020, xc7z020clg400-2, 512 MB DDR, UART0 on MIO 10..11.
Tools: Vivado 2019.1 + Xilinx SDK 2019.1.

## Result

Measured on hardware with no debugger attached. Three loads: a 1 MB DDR thrash
loop on CPU1; real network traffic - 97 Mbit/s of UDP broadcast, about 8000 packets
per second; and the same traffic with the application layer actually consuming it,
copying every payload into DDR and reading it back.

| | idle | 1 MB DDR thrash | 97 Mbit/s of traffic | **+ application-layer work** |
|---|---|---|---|---|
| interrupts missed | 0 | 0 | 0 | **0** |
| mean period | 12499.991 ns | 12499.991 ns | 12499.991 ns | 12499.991 ns |
| worst period | 12512 ns | 12512 ns | 12701 ns | **12701 ns** |
| worst error, any sample | 21 ns | 21 ns | 381 ns | **381 ns** = 3 % of the period |
| ISR execution time | ~940 ns | ~920 ns | ~1520 ns | **~1520 ns** |

Error distribution in one second under the network load, out of 78,826 samples:

| error | count | share |
|---|---|---|
| up to 21 ns | 74,096 | 94.0 % |
| 24 - 45 ns | 108 | 0.14 % |
| 48 - 93 ns | 253 | 0.32 % |
| **96 - 189 ns** | **4,346** | **5.51 %** |
| 192 - 381 ns | 22 | 0.028 % |

The population at 96 - 189 ns is the signature of the traffic: at 8000 packets and
80,000 interrupts per second, roughly one interrupt in eighteen collides with an
EMAC DMA burst on the shared DDR path. Causality is unambiguous - the moment the
flood stops the `<16`, `<32` and `<64` histogram bins empty, the worst period
returns to 12512 ns and the ISR to 940 ns, and they all come back when it restarts.

### Where the interference actually comes from

The fourth column is the interesting one. Without a bound PCB lwIP drops the flood
inside `udp_input()`: the DMA, the interrupt and the pbuf allocation all still
happen, but nothing ever touches the payload. Binding one (step 5 of
`cpu1/cpu1_changes.c.txt`) puts the packets in front of an application that copies
each payload into DDR and sums it back, and CPU0 prints what arrived:

```
app=6070 pkt 8725 KB    per second, sustained
```

8725 KB over 6070 packets is 1472 bytes each - the whole payload, every packet, so
none of it is being optimised away. That is about 8.9 MB/s written plus 8.9 MB/s
read, roughly 18 MB/s of load/store traffic that CPU1 was not generating before,
on the same DDR controller and the same L2 that CPU0's ISR depends on.

It changed nothing. `missed` stayed at 0, the mean stayed at 12499.991 ns, the
worst period stayed at 12701 ns, and the error histogram is the same to within its
own run-to-run noise - still ~94 % under 21 ns and still ~4350 samples in the
96 - 189 ns bin. So the jitter that the traffic does cause is the EMAC's DMA
contending for DDR, not CPU1 executing code: the ISR's own working set stays
resident, and the write-allocate traffic from a memcpy on the other core does not
displace it. Two runs, 53 s and 114 s, about 950,000 packets and 1.4 GB copied and
read in total, zero interrupts missed in either.

Note that the interrupt is never missed under any of these loads. `missed` only
ever moved when a JTAG read halted the core, which shows up unmistakably as a
multi-millisecond `max` and `isr_max`.

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
cpu1/bsp_patch_rtl8211f.txt the PHY speed-detection fix and the Ethernet notes
tools/run_amp.tcl         XSCT script: reset, FPGA, ps7_init, download, run both
tools/udp_flood.py        line-rate UDP broadcast, the real network load
tools/tcp_load.py         echo-server driver, for when transmit works
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
n=1040000 missed=0 avg=12499.988 ns  QUIET n=78826 min/max=3946/4232 (11837/12695 ns)  PRINT n=1174 max=4392  isr_max=1490 ns  app=5991 pkt 8612 KB
  |err| quiet: <1:18545 <2:29038 <4:23509 <8:2971 <16:127 <32:305 <64:4305 <128:25 <256:1
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
- `app` is CPU1 reporting back through the shared window: packets delivered to the
  UDP application and payload bytes copied out of the pbufs, both per second. It
  is there so the load can be shown rather than assumed - `app=0` while the flood
  is running would mean lwIP is dropping the frames before the application sees
  them, and the measurement would be of the driver alone.
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

1. **Bidirectional traffic.** The receive direction has been measured at line rate
   with `tools/udp_flood.py`, all the way up through the application layer, which
   needs no reply from the board and so works even though this board's transmit
   path does not reach the peer (see `cpu1/bsp_patch_rtl8211f.txt`). The transmit
   DMA path is therefore still unmeasured; receive is normally the heavier and
   burstier side. `tools/tcp_load.py` drives both directions once transmit works.
2. **Standalone boot.** Currently launched over JTAG. A `BOOT.BIN` with FSBL,
   bitstream and both ELFs would let it run without a host; `start_cpu1()` already
   does the `0xFFFFFFF0` + `sev` handoff for that case.
3. **`isr_max` ~900 ns** is almost entirely the three AXI-GP register reads. If the
   ISR ever needs to do more work, have the PL push the sample into the shared DDR
   window instead of being read over AXI.
