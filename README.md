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
boot/boot.bif             partition list for bootgen, with the ordering rule
boot/build_fsbl.sh        builds the FSBL outside Eclipse, when SDK holds the workspace
pl_eth/                   a working second Ethernet port over GEM1 and the PL, parked
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
  Regenerating the BSP drops this every time, and CPU1 then prints straight
  through the middle of CPU0's measurement output. Check it after every
  regeneration - the reliable test is that `STDOUT_BASEADDRESS` is absent from
  the generated `xparameters.h`.
- BSP -> `lwip211`: `lwip_dhcp` = `false`, `pbuf_pool_size` = `2048`,
  `phy_link_speed` = `CONFIG_LINKSPEED100`. DHCP has to be off: on a direct
  cable there is no server, and the template spins in

  ```c
  dhcp_start(echo_netif);
  dhcp_timoutcntr = 24;
  while (((echo_netif->ip_addr.addr) == 0) && (dhcp_timoutcntr > 0))
          xemacif_input(echo_netif);
  ```

  for 12 s - the counter drops every other 250 ms timer tick - before falling
  back to 192.168.1.10. Ping inside that window and nothing answers, and the
  DHCP discovers it broadcasts meanwhile show up in the transmit counters and
  muddy any measurement. With it off the interface is created at its static
  address and the board is answerable immediately.
  `tcp_wnd` and `tcp_snd_buf` both to `16384`; the default 2048 is barely one
  segment and caps TCP throughput regardless of the link.
- `lscript.ld`: `ps7_ddr_0` base `0x10000000`, size `0x0F000000`.
- Apply `cpu1/cpu1_changes.c.txt`, plus `cpu1/cpu1_load.c.txt` when testing
  without a live network.
- Run `python cpu1/apply_bsp_patch.py` after every BSP regeneration. Without it
  the PHY link flaps and the MAC transmits frames of zeros - section 7.

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

## 6. Standalone boot

`BOOT.BIN` holds the FSBL, the bitstream and both applications, so the board runs
with nothing attached but power.

```
bootgen -image boot.bif -arch zynq -o BOOT.BIN -w on
```

Run it from the SDK workspace root; `boot/boot.bif` in this repo is the partition
list, and the comment at the top of it is the part worth reading. FSBL hands off to
the **first** PS partition, not the last - `image_mover.c` guards the assignment
with `ExecAddrFlag` and says so in a comment - so `app_cpu0.elf` has to be listed
before `app_cpu1.elf`. Both end up in DDR; only CPU0 is started, and it releases
CPU1 through `0xFFFFFFF0` + `sev` exactly as it does under the debugger. The two
entry points already match the addresses the code uses:

| | entry | linker base |
|---|---|---|
| app_cpu0.elf | `0x00100000` | `0x00100000` |
| app_cpu1.elf | `0x10000000` | `CPU1_DDR_BASE` |

### Building the FSBL

An Empty Application will not do - the FSBL needs the `xilffs` and `xilrsa`
libraries in its BSP, which the `Zynq FSBL` template pulls in. From xsct:

```tcl
setws F:/vivado_projects/pl_irq/pl_irq.sdk
createapp -name fsbl -app {Zynq FSBL} -proc ps7_cortexa9_0           -hwproject design_1_wrapper_hw_platform_0 -os standalone
```

`projects -build` fails with *Invalid Workspace* whenever the SDK GUI has the
workspace open - the headless builder wants the same lock. Either close SDK and
build there, or run `boot/build_fsbl.sh`, which compiles the BSP with its own
Makefile and links the FSBL directly, no Eclipse involved. It builds with `-O2`
and `-DFSBL_DEBUG_INFO`, so the FSBL reports each partition as it loads it over
the same UART - worth having the first time, since it is the only visibility into
a boot that fails before CPU0 ever prints.

`ps7_init.c` is a linked resource from the hardware platform project, not a file
in `fsbl/src`, so anything building outside Eclipse has to compile it explicitly.

### Boot mode - Artemis StarLite

Header **J1**, four pins, pin 1 at the top. Pin 1 is the MIO5 net pulled up by
R57, pin 2 is ground, pin 3 is the MIO4 net pulled down by R60, pin 4 is 3V3.

| jumper on | MIO[5] | MIO[4] | boots from |
|---|---|---|---|
| pins 1-2 | 0 | 0 | JTAG |
| pins 2-3 | 1 | 0 | QSPI flash |
| pins 3-4 | 1 | 1 | microSD |

With no jumper at all the pull resistors give QSPI. Both targets are fitted: a
16 MB Winbond `W25Q128JVSIQ` at U7, and a microSD socket at J3 behind a level
shifter. The image is 4.1 MB, so either holds it.

- **microSD**: FAT32, `BOOT.BIN` in the root directory, jumper on pins 3-4.
- **QSPI**: `program_flash -f BOOT.BIN -fsbl fsbl.elf -flash_type qspi_single
  -blank_check -verify -cable type xilinx_tcf url TCP:127.0.0.1:3121`,
  jumper on pins 2-3. This erases whatever the vendor shipped in that flash.

## 7. Bidirectional TCP

The transmit path was the last thing to work, and it took three separate fixes.
The board now holds a TCP echo session at **35 Mbit/s in each direction,
516 MB in 60 s**, measured with `tools/tcp_load.py` against a BSP regenerated
from scratch and patched only by the script below.

Run it on a freshly regenerated BSP, then rebuild:

```
python cpu1/apply_bsp_patch.py            # defaults to F:/vivado_projects/pl_irq/pl_irq.sdk
```

It is idempotent and it touches two files:

1. **`xemacpsif_physpeed.c`** - `configure_IEEE_phy_speed()` resets the PHY every
   time it is called, and `eth_link_detect()` calls it again as soon as it sees
   the link drop, which is exactly what the reset caused. The link flapped
   forever, one to two seconds per cycle. Every experiment run before this was
   measuring the flap. The patch configures each PHY address once.

2. **the RTL8211F RGMII delays**, in the same function. The driver writes Marvell
   registers - page 2, register 21 - into a Realtek PHY, so the transmit clock
   delay was never enabled. On the RTL8211F it is page 0xD08, register 0x11
   bit 8 (TX) and register 0x15 bit 3 (RX), page select on register 0x1F. Read
   back live: TX 0x0009, bit 8 clear. The writes have to come **after** the
   reset, which restores the defaults.

3. **`xil_cache.c`** - the real one. Every L2 operation in that file is wrapped in
   `#ifndef USE_AMP`, and CPU1 is built with `USE_AMP=1`, so
   `Xil_DCacheFlushRange()` cleans L1 and stops. lwIP writes a frame, the line
   stays dirty in L2, and the EMAC DMA reads what is actually in DDR. A capture
   on the peer shows it plainly - one frame back from the board after every ARP
   request, correct length, correct timing, and every byte zero:

   ```
   ff:ff:ff:ff:ff:ff <- d4:93:90:3f:72:a3  et=0806 len=42
   00:00:00:00:00:00 <- 00:00:00:00:00:00  et=0000 len=60
   ```

   The FCS is computed over the zeros, so it is a valid frame and the peer drops
   it as an unknown ethertype without counting an error. Receive was unaffected,
   which is why the link looked half-working for weeks and why both Ethernet
   ports behaved identically - one driver, one bug.

   `Xil_DCacheInvalidateRange()` has the same guard and is the other half. With
   only the flush restored, TCP connected and stalled after about forty round
   trips: buffers the DMA had rewritten still held stale L2 lines. Restoring both
   took it from 43 round trips to 47,070.

   Range operations on the PL310 are safe from either core. The whole-cache ones
   are not, and none are used here. The L2 helper functions are themselves
   compiled out under `USE_AMP`, so the patch writes the registers directly.

   The 19.28 Mbit/s figure this section carried before was taken through a
   faulty host NIC - see section 10. The same build on a working adapter does
   70 Mbit/s echoed both ways.

Two more, in the stock echo template, applied to `app_cpu1/src/echo.c`
(`cpu1/reference/echo.c`): `tcp_recved(tpcb, p->len)` only acknowledges the first
buffer of a chain, so the receive window shrinks on every chained segment until it
reaches zero - it has to be `p->tot_len`, and the whole chain has to be echoed
back, not just the first buffer.

`cpu1/bsp_settings.txt` lists the `.mss` settings this depends on -
`tcp_wnd` and `tcp_snd_buf` at 16384, `stdout = none`, `-DUSE_AMP=1`.
`cpu1/bsp_patch_rtl8211f.txt` is the full investigation, and section 10 has
the check to run before believing any Ethernet fault on this board.

## 8. Bidirectional TCP under the interrupt, measured

Booted from the microSD card, no debug session attached, the whole run captured
off UART0. `tools/tcp_load_capture.log` is the raw capture; the load was
`tools/tcp_load.py --conns 4 --block 4096 --seconds 60`, sustaining
**70 Mbit/s echoed both ways - 516 MB in 60 s**.

| | idle before | under TCP load | idle after |
|---|---|---|---|
| reports | 28 | 60 | 28 |
| **interrupts missed** | **0** | **0** | **0** |
| worst period | 12578 ns | 12629 ns | 12623 ns |
| `isr_max` | 1817 ns | 1343 ns | 1367 ns |

**9,280,000 interrupts, zero missed.** Worst deviation under load is 129 ns,
about 1 % of the period - the same order as the 97 Mbit/s receive-only flood in
the table at the top, and now with the transmit DMA path loaded as well.

An earlier reading of this taken over JTAG showed 904 missed out of 6,894,508.
That was the probe: an idle control run with the same two debugger connects
showed 405 out of 5,974,601. Reading the counters over JTAG perturbs them more
than the network load does.

### Filling the period

`ISR_BUSY_NS` in `cpu0/src/main.c` holds the handler at a fixed duration
measured from entry, so the ISR occupies a chosen slice of the 12.5 us period
whatever the cache state and whatever the compiler does with the loop:

```c
#define ISR_BUSY_NS    11000u
...
while ((Xil_In32(GT_CNT_LO) - t0) < ISR_BUSY_TICKS) { }
```

At 11000 the handler measures 11843 ns and the core spends **95 % of every
period inside the ISR**, leaving 657 ns for the main loop - which still has to
print the statistics over the UART. Under the same 70 Mbit/s bidirectional TCP
load: 6,800,000 interrupts, **zero missed**, worst period 12860 ns.
`tools/isr_11us_capture.log` is the capture. That run was loaded over JTAG with
no commands issued during it, not booted from the card, so it is one step less
clean than section 8.

The worst period moved from 12629 ns to 12860 ns. With the ISR filling the
period there is nothing left to absorb a delay, so what was slack before now
shows up in the measurement. Set `ISR_BUSY_NS` to 0 to get the original handler
back.

## 9. Not yet done

**`isr_max` ~900 ns** is almost entirely the three AXI-GP register reads. If the
ISR ever needs to do more work, have the PL push the sample into the shared DDR
window instead of being read over AXI.

## 10. Two things that cost days

**The BSP patches are not sticky.** Regenerating the BSP - which the SDK will do
on its own, without being asked - restores the pristine `xemacpsif_physpeed.c`
and `xil_cache.c` and resets `stdout` to `ps7_uart_0`. The link then flaps, the
MAC transmits zeros, and CPU1 prints over CPU0's measurement output. Everything
looks like new hardware faults. Check first, before diagnosing anything:

```
grep -c "L2 maintenance restored for AMP" <bsp>/ps7_cortexa9_1/libsrc/standalone_v7_0/src/xil_cache.c
grep -c "phy_configured" <bsp>/.../netif/xemacpsif_physpeed.c
grep -c STDOUT_BASEADDRESS <bsp>/ps7_cortexa9_1/include/xparameters.h
```

Two zeros and a zero is the state you want. Anything else, re-run
`python cpu1/apply_bsp_patch.py` and rebuild.

**The host NIC was the other half.** The Realtek PCIe GbE in the laptop reports
`Disconnected` with the cable in and the board's PHY negotiated and stable -
`BMSR` link bit set for 20 consecutive samples over 10 s, `PHYSR` 0x31DC
(100 Mbit, full duplex), and `ANLPAR` 0xCDE1 with the Acknowledge bit set, which
only the link partner can set and only after it has received our link code words.
So the board was being heard by something that Windows insisted was not there.
Disabling and re-enabling the adapter did not help. A USB-C to Ethernet adapter
in the same laptop linked immediately, pinged 4/4, and ran the 70 Mbit/s echo
above. The lifetime receive counter on that Realtek had been zero from the first
day of this project, for any traffic at all.

Useful registers when the link is the suspect, read over JTAG with the cores
running - `mrd -force` does not halt them:

| | |
|---|---|
| `BMSR` bit 2 | link, latching low - read twice, the second read is current |
| `ANLPAR` bit 14 | the partner acknowledges our link code words |
| `PHYSR` page 0xA43 reg 0x1A | RTL8211F: bit 2 link, bits 5:4 speed, bit 3 duplex |
| `NWCFG` 0xE000B004 | 0x011F20C3 once the driver has configured the MAC |
| frames RX `0xE000B158` | clear-on-read |
