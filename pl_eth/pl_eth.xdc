# ETH-PL: the second RTL8211F, reached through GEM1 over EMIO and the
# GMII-to-RGMII converter in the PL. All fourteen signals are in bank 34.
#
# Bank 34 runs at 3.3 V here - R24 fitted, R26 and R27 marked NC - and the PHY's
# own RGMII supply is 3.3 V as well (R145 fitted, R146 NC). The PS port next to
# it is 1.8 V on both sides, so do not copy its constraints onto this one.

set_property -dict {PACKAGE_PIN N18 IOSTANDARD LVCMOS33} [get_ports rgmii_rxc]
set_property -dict {PACKAGE_PIN P19 IOSTANDARD LVCMOS33} [get_ports rgmii_rx_ctl]
set_property -dict {PACKAGE_PIN Y18 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[0]}]
set_property -dict {PACKAGE_PIN Y19 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[1]}]
set_property -dict {PACKAGE_PIN V16 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[2]}]
set_property -dict {PACKAGE_PIN W16 IOSTANDARD LVCMOS33} [get_ports {rgmii_rd[3]}]

set_property -dict {PACKAGE_PIN V20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports rgmii_txc]
set_property -dict {PACKAGE_PIN W20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports rgmii_tx_ctl]
set_property -dict {PACKAGE_PIN N20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports {rgmii_td[0]}]
set_property -dict {PACKAGE_PIN P20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports {rgmii_td[1]}]
set_property -dict {PACKAGE_PIN T20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports {rgmii_td[2]}]
set_property -dict {PACKAGE_PIN U20 IOSTANDARD LVCMOS33 SLEW FAST DRIVE 12} [get_ports {rgmii_td[3]}]

set_property -dict {PACKAGE_PIN U14 IOSTANDARD LVCMOS33} [get_ports mdio_phy_mdc]
set_property -dict {PACKAGE_PIN U15 IOSTANDARD LVCMOS33} [get_ports mdio_phy_mdio_io]
