# cxl_avalon_shim_hw.tcl -- Platform Designer (Qsys) component descriptor for
# cxl_avalon_shim.sv (which instantiates cxl_mem_protocol_engine.sv directly).
# Modeled directly on the Weibull-CFAR reference project's
# cfar_avalon_bridge_hw.tcl pattern -- hand-written, fixed HDL, no procedural
# codegen, so this uses the plain add_fileset_file pattern.

package require -exact qsys 12.0

set_module_property NAME cxl_avalon_shim
set_module_property DISPLAY_NAME "CXL-Lite Memory-Side Cache Avalon-MM Shim"
set_module_property VERSION 1.0
set_module_property GROUP "CXL-Lite Memory-Side Cache"
set_module_property DESCRIPTION "Avalon-MM slave wrapper around cxl_mem_protocol_engine: register-poke M2S request fire, S2M response readback."
set_module_property AUTHOR "CXL-Lite Memory-Side Cache project"
set_module_property INTERNAL false
set_module_property INSTANTIATE_IN_SYSTEM_MODULE true
set_module_property EDITABLE false
set_module_property ELABORATION_CALLBACK elaborate

# ---- source files ----------------------------------------------------
add_fileset QUARTUS_SYNTH QUARTUS_SYNTH "" ""
set_fileset_property QUARTUS_SYNTH TOP_LEVEL cxl_avalon_shim
foreach f {
    cxl_avalon_shim.sv
    cxl_mem_protocol_engine.sv
    cxl_bias_table.sv
} {
    add_fileset_file $f SYSTEM_VERILOG PATH $f
}

add_fileset SIM_VERILOG SIM_VERILOG "" ""
set_fileset_property SIM_VERILOG TOP_LEVEL cxl_avalon_shim
foreach f {
    cxl_avalon_shim.sv
    cxl_mem_protocol_engine.sv
    cxl_bias_table.sv
} {
    add_fileset_file $f SYSTEM_VERILOG PATH $f
}

# ---- parameters (match cxl_avalon_shim.sv's own #() defaults) ---------
add_parameter ADDR_W INTEGER 8
set_parameter_property ADDR_W DISPLAY_NAME "Address width (bits)"
set_parameter_property ADDR_W HDL_PARAMETER true

add_parameter DATA_W INTEGER 32
set_parameter_property DATA_W DISPLAY_NAME "Data width (bits)"
set_parameter_property DATA_W HDL_PARAMETER true

add_parameter TAG_W INTEGER 4
set_parameter_property TAG_W DISPLAY_NAME "Tag width (bits)"
set_parameter_property TAG_W HDL_PARAMETER true

# ---- interfaces ---------------------------------------------------------
proc elaborate {} {

    add_interface clk clock end
    set_interface_property clk clockRate 0
    add_interface_port clk clk clk Input 1

    add_interface reset reset end
    set_interface_property reset associatedClock clk
    set_interface_property reset synchronousEdges DEASSERT
    add_interface_port reset rstn reset_n Input 1

    add_interface avs avalon end
    set_interface_property avs associatedClock clk
    set_interface_property avs associatedReset reset
    set_interface_property avs addressUnits WORDS
    set_interface_property avs addressAlignment NATIVE
    set_interface_property avs bitsPerSymbol 8
    set_interface_property avs readLatency 0
    set_interface_property avs readWaitTime 0
    set_interface_property avs writeWaitTime 0
    set_interface_property avs setupTime 0
    set_interface_property avs holdTime 0

    add_interface_port avs avs_address     address       Input  5
    add_interface_port avs avs_write       write         Input  1
    add_interface_port avs avs_writedata   writedata     Input  32
    add_interface_port avs avs_read        read          Input  1
    add_interface_port avs avs_readdata    readdata      Output 32
    add_interface_port avs avs_waitrequest waitrequest   Output 1

    # Hardware bring-up debug conduit, same pattern as the Weibull-CFAR
    # reference's cfar_bridge_0_dbg -- hand-wired to HEX0 in the top level
    # for a hardware sanity check independent of whether the register
    # interface itself is working correctly yet.
    add_interface dbg conduit end
    set_interface_property dbg associatedClock clk
    set_interface_property dbg associatedReset reset

    add_interface_port dbg dbg_heartbeat      heartbeat      Output 1
    add_interface_port dbg dbg_seen_request   seen_request   Output 1
    add_interface_port dbg dbg_seen_response  seen_response  Output 1
}
