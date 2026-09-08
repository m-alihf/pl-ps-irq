set HW  F:/vivado_projects/pl_irq/pl_irq.sdk/design_1_wrapper_hw_platform_0
set E0  F:/vivado_projects/pl_irq/pl_irq.sdk/app_cpu0/Debug/app_cpu0.elf
set E1  F:/vivado_projects/pl_irq/pl_irq.sdk/app_cpu1/Debug/app_cpu1.elf

connect -url tcp:127.0.0.1:3121

targets -set -filter {name =~ "*Cortex-A9 MPCore #0*"}
rst -system
after 3000

targets -set -filter {name =~ "xc7z020"}
fpga -file $HW/design_1_wrapper.bit
puts "fpga programmed"

targets -set -filter {name =~ "*Cortex-A9 MPCore #0*"}
loadhw -hw $HW/system.hdf -mem-ranges [list {0x40000000 0xbfffffff}]
source $HW/ps7_init.tcl
ps7_init
ps7_post_config
puts "ps7 initialised"

targets -set -filter {name =~ "*Cortex-A9 MPCore #1*"}
dow $E1
puts "cpu1 elf downloaded"

targets -set -filter {name =~ "*Cortex-A9 MPCore #0*"}
dow $E0
puts "cpu0 elf downloaded"

con
targets -set -filter {name =~ "*Cortex-A9 MPCore #1*"}
con
puts "both cores running"
