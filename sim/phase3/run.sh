#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

verilator -Wall --cc --exe --build -j 0 --trace \
    --top-module cxl_mem_protocol_engine \
    ../../rtl/cxl_bias_table.sv \
    ../../rtl/cxl_mem_protocol_engine.sv \
    tb_protocol_engine.cpp

./obj_dir/Vcxl_mem_protocol_engine
