#!/usr/bin/env python3
"""Re-apply the RTL8211F fixes to the generated lwIP BSP source.

The BSP regenerates this file from the Xilinx tree whenever the hardware is
updated or a BSP setting changes, which throws the edits away. Run this after
any regeneration; it is idempotent.

    python cpu1/apply_bsp_patch.py [<workspace>]

Two fixes, both in configure_IEEE_phy_speed(). bsp_patch_rtl8211f.txt has the
measurements behind them.
"""
import sys
import pathlib

WS = sys.argv[1] if len(sys.argv) > 1 else r'F:/vivado_projects/pl_irq/pl_irq.sdk'
SRC = (pathlib.Path(WS) / 'app_cpu1_bsp/ps7_cortexa9_1/libsrc/lwip211_v1_0/src'
                          '/contrib/ports/xilinx/netif/xemacpsif_physpeed.c')

MARKER = 'AK-StarLite RTL8211F fixes'

OLD_HEAD = (
 "static u32_t configure_IEEE_phy_speed(XEmacPs *xemacpsp, u32_t phy_addr, u32_t speed)\n"
 "{\n"
 "\tu16_t control;\n"
 "\tu16_t autonereg;\n"
 "\n"
 "\tXEmacPs_PhyWrite(xemacpsp,phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 2);\n"
 "\tXEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, &control);\n"
 "\tcontrol |= IEEE_RGMII_TXRX_CLOCK_DELAYED_MASK;\n"
 "\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, control);\n"
 "\n"
 "\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 0);\n")

NEW_HEAD = (
 "static u32_t configure_IEEE_phy_speed(XEmacPs *xemacpsp, u32_t phy_addr, u32_t speed)\n"
 "{\n"
 "\tu16_t control;\n"
 "\tu16_t autonereg;\n"
 "\tu16_t phy_identity;                 /* " + MARKER + " */\n"
 "\tstatic u32_t phy_configured = 0u;\n"
 "\n"
 "\t/* Run once per PHY. The tail of this function resets the PHY, and a reset\n"
 "\t * drops the link; eth_link_detect() sees that a second later and calls\n"
 "\t * straight back in here, resetting it again. That loop is why the link\n"
 "\t * looked like a failing cable - halt the core running lwIP and it goes\n"
 "\t * rock steady. The PHY keeps its settings, so there is nothing to redo. */\n"
 "\tif (phy_configured & (1u << (phy_addr & 31u))) {\n"
 "\t\treturn 0;\n"
 "\t}\n"
 "\n"
 "\tXEmacPs_PhyRead(xemacpsp, phy_addr, PHY_IDENTIFIER_1_REG, &phy_identity);\n"
 "\tif (phy_identity != PHY_REALTEK_IDENTIFIER) {\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp,phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 2);\n"
 "\t\tXEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, &control);\n"
 "\t\tcontrol |= IEEE_RGMII_TXRX_CLOCK_DELAYED_MASK;\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, control);\n"
 "\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 0);\n"
 "\t}\n")

OLD_TAIL = (
 "\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET,\n"
 "\t\t\t\t\t\t\t\t\t\t\tcontrol | IEEE_CTRL_RESET_MASK);\n"
 "\t{\n"
 "\t\tvolatile s32_t wait;\n"
 "\t\tfor (wait=0; wait < 100000; wait++);\n"
 "\t}\n"
 "\treturn 0;\n"
 "}")

NEW_TAIL = (
 "\tXEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET,\n"
 "\t\t\t\t\t\t\t\t\t\t\tcontrol | IEEE_CTRL_RESET_MASK);\n"
 "\t{\n"
 "\t\tvolatile s32_t wait;\n"
 "\t\tfor (wait=0; wait < 100000; wait++);\n"
 "\t}\n"
 "\n"
 "\t/* RGMII clock delay, and it has to be after the reset because the reset\n"
 "\t * restores the register defaults. The Marvell recipe above - page 2,\n"
 "\t * register 21, bits 5:4 - means nothing on an RTL8211F: register 22 is\n"
 "\t * not a page select there and register 21 is not the delay control, so\n"
 "\t * the transmit delay never gets enabled. The MAC then hands the PHY\n"
 "\t * perfectly good frames, the PHY samples them against a misaligned\n"
 "\t * clock, and what reaches the wire has a corrupt FCS - the peer counts\n"
 "\t * receive errors and drops every one, which is what the host counters\n"
 "\t * showed. Receive works throughout because that delay is on by default:\n"
 "\t * read on this board, transmit is 0x0009 with bit 8 clear and receive is\n"
 "\t * 0x0019 with bit 3 set. Both live in page 0xD08 - transmit in register\n"
 "\t * 0x11 bit 8, receive in register 0x15 bit 3. */\n"
 "\tif (phy_identity == PHY_REALTEK_IDENTIFIER) {\n"
 "\t\tu16_t dly;\n"
 "\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, 0x1F, 0x0D08);\n"
 "\t\tXEmacPs_PhyRead(xemacpsp, phy_addr, 0x11, &dly);\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, 0x11, dly | 0x0100);\n"
 "\t\tXEmacPs_PhyRead(xemacpsp, phy_addr, 0x15, &dly);\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, 0x15, dly | 0x0008);\n"
 "\t\tXEmacPs_PhyWrite(xemacpsp, phy_addr, 0x1F, 0x0000);\n"
 "\t}\n"
 "\n"
 "\tphy_configured |= (1u << (phy_addr & 31u));\n"
 "\treturn 0;\n"
 "}")


# --- second file: the L2 maintenance that USE_AMP throws away ---------------
#
# USE_AMP disables every L2 operation in xil_cache.c. On the core running lwIP
# that breaks the EMAC in both directions:
#
#   transmit  a frame lwIP has just written stays dirty in L2 while the DMA
#             reads DDR underneath it, so the MAC puts a correctly sized frame
#             of zeros on the wire - with a valid FCS, so the peer does not even
#             log an error, it just drops a frame of ethertype 0x0000
#   receive   buffers the DMA has rewritten keep stale L2 lines, so ACKs and
#             payload read back as garbage and TCP stalls within a few tens of
#             kilobytes
#
# Range operations on the PL310 are safe from either core; it is the whole-cache
# ones that stall the shared path and must stay out of CPU1. The L2 helpers are
# themselves compiled out under USE_AMP, so the registers are written directly.

CACHE_REL = 'app_cpu1_bsp/ps7_cortexa9_1/libsrc/standalone_v7_0/src/xil_cache.c'
CACHE_MARKER = 'L2 maintenance restored for AMP'

FLUSH_OLD = (
 "#ifndef USE_AMP\n"
 "\t\t\t/* Flush L2 cache line */\n"
 "\t\t\t*L2CCOffset = LocalAddr;\n"
 "\t\t\tXil_L2CacheSync();\n"
 "#endif\n")

FLUSH_NEW = (
 "\t\t\t/* Flush the L2 line - L2 maintenance restored for AMP */\n"
 "\t\t\t*L2CCOffset = LocalAddr;\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_SYNC_OFFSET, 0x0U);\n")

EDGE_OLD = (
 "#ifndef USE_AMP\n"
 "\t\t\t/* Disable Write-back and line fills */\n"
 "\t\t\tXil_L2WriteDebugCtrl(0x3U);\n"
 "\t\t\tXil_L2CacheFlushLine(%s);\n"
 "\t\t\t/* Enable Write-back and line fills */\n"
 "\t\t\tXil_L2WriteDebugCtrl(0x0U);\n"
 "\t\t\tXil_L2CacheSync();\n"
 "#endif\n")

EDGE_NEW = (
 "\t\t\t/* the partial line at the edge, through the registers */\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_DEBUG_CTRL_OFFSET, 0x3U);\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_INV_CLN_PA_OFFSET, %s);\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_SYNC_OFFSET, 0x0U);\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_DEBUG_CTRL_OFFSET, 0x0U);\n")

INVAL_OLD = (
 "#ifndef USE_AMP\n"
 "\t\t\t/* Invalidate L2 cache line */\n"
 "\t\t\t*L2CCOffset = tempadr;\n"
 "\t\t\tXil_L2CacheSync();\n"
 "#endif\n")

INVAL_NEW = (
 "\t\t\t/* Invalidate the L2 line - not optional on this core */\n"
 "\t\t\t*L2CCOffset = tempadr;\n"
 "\t\t\tXil_Out32(XPS_L2CC_BASEADDR + XPS_L2CC_CACHE_SYNC_OFFSET, 0x0U);\n")


def patch_cache():
    src = pathlib.Path(WS) / CACHE_REL
    if not src.exists():
        print("not found: %s" % src)
        return 1
    text = src.read_text(encoding='utf-8', newline='')
    if CACHE_MARKER in text:
        print("xil_cache.c already patched")
        return 0
    nl = '\r\n' if '\r\n' in text else '\n'
    E = lambda s: s.replace('\n', nl)
    done = 0
    if E(FLUSH_OLD) in text:
        text = text.replace(E(FLUSH_OLD), E(FLUSH_NEW), 1)
        done += 1
    for var in ("tempadr", "tempend"):
        o = E(EDGE_OLD % var)
        if o in text:
            text = text.replace(o, E(EDGE_NEW % var), 1)
            done += 1
    if E(INVAL_OLD) in text:
        text = text.replace(E(INVAL_OLD), E(INVAL_NEW), 1)
        done += 1
    if done != 4:
        print("xil_cache.c: matched %d of 4 blocks - not the expected source" % done)
        return 1
    src.write_text(text, encoding='utf-8', newline='')
    print("patched %s (4 L2 blocks)" % src)
    return 0


def main():
    if not SRC.exists():
        print("not found: %s" % SRC)
        return 1
    text = SRC.read_text(encoding='utf-8', newline='')
    if MARKER in text:
        print("physpeed already patched")
        return patch_cache()
    nl = '\r\n' if '\r\n' in text else '\n'
    for old, new, what in ((OLD_HEAD, NEW_HEAD, 'head'), (OLD_TAIL, NEW_TAIL, 'tail')):
        o = old.replace('\n', nl)
        if o not in text:
            print("%s anchor not found - BSP source is not the version this patch expects" % what)
            return 1
        text = text.replace(o, new.replace('\n', nl), 1)
    SRC.write_text(text, encoding='utf-8', newline='')
    print("patched %s" % SRC)
    return patch_cache()


if __name__ == '__main__':
    sys.exit(main())
