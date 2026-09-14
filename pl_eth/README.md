# The second Ethernet port, over GEM1 and the PL

This directory is a working second Ethernet port for this board, built and
measured, then taken back out of the live design. Everything needed to put it
back is here.

## Why it was built

The board never managed a TCP session because it never transmitted anything the
host recognised. The receive direction was perfect. The theory was a fault past
the MAC pins - the RGMII wires, the PHY, the magnetics - and this port replaces
all of that: different pins, a different PHY chip, a different jack.

## What it proved

It works. The link comes up, the driver finds the PHY, frames go out:

```
phyaddrforemac  = 2          the real PHY, not the converter at 8
eth_link_status = 1          up
converter reg16 = 0x2100     the driver wrote 100 Mbit full duplex into it
GEM1 NWCTRL     = 0x1C       receive and transmit both enabled
```

And in one 20 s window it transmitted 54 frames and received 19, with no errors
on either side.

The host still saw nothing - which is the useful part. Two ports, two PHYs, two
different pin banks, two different I/O voltages, same result. That ruled out the
board, and pointed at the host: both of the laptop's Ethernet adapters have
lifetime receive counters sitting at zero, and two VPN packet filters are bound
to the interface. So the port is parked, not abandoned.

## Putting it back

1. `design_1_bd_pl_eth.tcl` rebuilds the block design: GEM1 out through EMIO,
   Xilinx's `gmii_to_rgmii` converter between EMIO's GMII and the board's RGMII,
   a 200 MHz FCLK for the converter, an inverter for its active-high reset, and
   the unconnected PHY interrupt tied low. `design_1_bd_baseline.tcl` is the
   design without it, for going back.
2. `pl_eth.xdc` carries the fourteen pins. Read the header - this bank is 3.3 V,
   unlike the 1.8 V PS port.
3. In the CPU1 BSP, point lwIP at the other MAC:
   `PLATFORM_EMAC_BASEADDR` becomes `XPAR_XEMACPS_1_BASEADDR`.
4. Append `-DXPAR_GMII2RGMIICON_0N_ETH1_ADDR=8` to the CPU1 BSP's
   `extra_compiler_flags`. See the first gotcha below for why this is not
   optional.

`hardware_notes.txt` has the pin map, the voltage evidence and the measurements.

## Three things that cost time

**The converter's MDIO address is never generated when both GEMs are enabled.**
The driver needs `XPAR_GMII2RGMIICON_0N_ETH1_ADDR` to write the link speed into
the converter, and `emacps.tcl` is supposed to emit it. It does not:

```tcl
set phya [is_gmii2rgmii_conv_present $ip]
if { $phya == 0} {
        close $file_handle
        return 0
}
```

The loop reaches `ps7_ethernet_0` first, finds no converter on it, and returns
out of the whole function before it ever looks at `ps7_ethernet_1`. Any design
with both GEMs enabled and the converter on the second one hits this. Defining
the macro through the BSP's compiler flags works and survives regeneration.

**Regenerating the BSP silently drops `stdout = none`.** CPU1 would start
printing onto the UART that CPU0 owns, straight through the middle of the
measurement output. Check the OS section of the mss after every regeneration.

**The RTL8211F speed patch is not compiled in this configuration.**
`get_Realtek_phy_speed` lives inside `#if defined CONFIG_LINKSPEED_AUTODETECT`,
and the BSP is set to `CONFIG_LINKSPEED100`, which forces the speed instead of
detecting it. Worth knowing before re-applying a patch that cannot run.

## One thing worth keeping in mind

GEM1's interrupt is 77. The GIC fix that made this project work in the first
place - CPU0 handing every shared peripheral interrupt to CPU1 and keeping only
61 - already covers it, with nothing to change.
