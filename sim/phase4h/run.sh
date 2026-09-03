#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

verilator -Wall --cc --exe --build -j 0 --trace \
    --top-module cxl_avalon_shim \
    ../../rtl/cxl_bias_table.sv \
    ../../rtl/cxl_mem_protocol_engine.sv \
    ../../rtl/cxl_avalon_shim.sv \
    tb_avalon_shim.cpp

./obj_dir/Vcxl_avalon_shim
