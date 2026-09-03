#!/usr/bin/env bash
# Full Quartus build chain -- see BUG-010/BUG-011 notes in bugs.md and the
# mandatory hardware procedure in FPGA_Implementation_Roadmap.md before
# using the resulting .sof: always read quartus_sta output before flashing,
# and always confirm the fabric is actually configured via a
# watchdog-protected read (safe_probe.c) before any real MMIO access.
set -e
cd "$(dirname "$0")"

Q="C:/intelFPGA_lite/21.1/quartus/bin64"
QSYS="C:/intelFPGA_lite/21.1/quartus/sopc_builder/bin"
PROJ="CXL_DE10_Standard"
HERE="F:/Projects/cxi_memory/quartus"

echo "=== [1/6] qsys-generate ==="
"$QSYS/qsys-generate.exe" "$HERE/soc_system.qsys" \
    --synthesis=VERILOG --output-directory="$HERE/soc_system" \
    --family="Cyclone V" --part=5CSXFC6D6F31C6 > build_qsys.log 2>&1
echo "    ok"

echo "=== [2/6] quartus_map (analysis & synthesis) ==="
"$Q/quartus_map.exe" "$PROJ" > build_map.log 2>&1
echo "    ok"

echo "=== [3/6] HPS SDRAM pin assignments ==="
"$Q/quartus_sta.exe" -t soc_system/synthesis/submodules/hps_sdram_p0_pin_assignments.tcl "$PROJ" > build_pins.log 2>&1
echo "    ok"

echo "=== [4/6] quartus_cdb --merge ==="
"$Q/quartus_cdb.exe" "$PROJ" --merge=on > build_merge.log 2>&1
echo "    ok"

echo "=== [5/6] quartus_fit (place & route -- slowest stage) ==="
"$Q/quartus_fit.exe" "$PROJ" > build_fit.log 2>&1
echo "    ok"

echo "=== [6/6] quartus_asm + quartus_sta ==="
"$Q/quartus_asm.exe" "$PROJ" > build_asm.log 2>&1
"$Q/quartus_sta.exe" "$PROJ" > build_sta.log 2>&1
echo "    ok"

echo "=== BUILD COMPLETE -- review timing before flashing ==="
grep -c "Slack : -" output_files/${PROJ}.sta.summary && echo "NEGATIVE SLACK PRESENT" || echo "no negative slack found"
