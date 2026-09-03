# add_cxl_shim.tcl -- swaps the Weibull-CFAR reference system's custom
# peripheral (cfar_bridge_0) for our cxl_avalon_shim_0, reusing the rest of
# the proven soc_system.qsys (HPS config, DDR3 timing, mm_bridge_0, PIO
# peripherals) as-is. Modeled directly on the reference project's
# add_cfar_bridge.tcl pattern.
#
# Run via:
#   qsys-script --script=add_cxl_shim.tcl --system-file=soc_system.qsys \
#       --search-path=ip,ip/cxl_avalon_shim

package require -exact qsys 12.0

remove_instance cfar_bridge_0

add_instance cxl_avalon_shim_0 cxl_avalon_shim
set_instance_parameter_value cxl_avalon_shim_0 {ADDR_W} {8}
set_instance_parameter_value cxl_avalon_shim_0 {DATA_W} {32}
set_instance_parameter_value cxl_avalon_shim_0 {TAG_W} {4}

add_connection clk_0.clk cxl_avalon_shim_0.clk clock
add_connection clk_0.clk_reset cxl_avalon_shim_0.reset reset
add_connection mm_bridge_0.m0 cxl_avalon_shim_0.avs avalon
set_connection_parameter_value mm_bridge_0.m0/cxl_avalon_shim_0.avs baseAddress {0x6000}

save_system {soc_system.qsys}
