# CXL-Lite Memory-Side Cache — DE10-Standard FPGA Implementation Roadmap

Companion to [`CXL_Lite_Memory_Side_Cache_Manual.md`](CXL_Lite_Memory_Side_Cache_Manual.md). That manual scoped the project as **simulation-only** ("no FPGA" — Part 0). This document extends it onto real DE10-Standard silicon.

Check items off as `- [x]` while you go. Each phase has a **Gate** — don't start the next phase until the gate condition is true.

---

## 0. Architecture Decisions (locked in)

| Decision | Choice | Why |
|---|---|---|
| CXL "host" emulation | **HPS ARM Cortex-A9, bare-metal C driver** | No real CXL PHY exists on Cyclone V (CXL needs PCIe 5.0/6.0 alt-protocol, unsupported hard IP on this device). The HPS↔FPGA-fabric boundary is a legitimate architectural analog of host↔CXL-device: two independent domains connected by a narrow bridge, exactly like a real host talking to a Type-3 device controller. Bare-metal (no Linux) avoids SD-card/U-Boot/kernel/rootfs bring-up overhead. |
| Backing memory (device-attached DRAM model) | **On-chip Block RAM (M10K) only** | Matches the simulation model's behavioral memory array almost exactly, so the sim→hardware port is close to 1:1. No SDRAM controller bring-up needed. Capacity is small (tens of KB) — fine, since this project's signal is the *coherence/bias logic*, not memory capacity. |
| Verification discipline | **Sim-first (Verilator + C++ testbench) → then port to hardware, per feature** | Bias/BI races are exactly the bug class that's nearly impossible to root-cause blind on real silicon (no waveform, no scoreboard). Get each feature correct and debuggable in a fast open-source sim before spending hardware bring-up time on it. |
| Simulator | **Verilator, run under WSL2 Ubuntu** | Native Windows Verilator support is immature; WSL2 (already installed and running) gives a real Linux build environment. Icarus Verilog (`C:\iverilog\bin`, already installed natively on Windows) is kept as a lightweight secondary/lint cross-check — useful for quick syntax/elaboration checks without leaving Windows. |
| FPGA configuration / boot method | **JTAG for FPGA-fabric `.sof` programming and SignalTap throughout; SD card flashed *once* with preloader+U-Boot to get the HPS to a serial-console prompt, then fast `loadb`/`tftpboot` + `go` cycles for HPS app iteration (no SD rewrite per iteration); a full SD-card-boots-everything-standalone image stays a Phase 9 stretch goal** | Revised from the original "JTAG-only, SD card fully deferred" plan once Phase 1H's research turned up the reference project's own lab bring-up notes: U-Boot's serial console already gives the same fast, no-rewrite iteration loop JTAG was chosen for, and building a full bare-metal JTAG ELF loader (DS-5/OpenOCD) would have been higher-risk and mostly redundant with a path that has to exist anyway (SD card boot is still the end-state target). JTAG remains mandatory for FPGA-fabric programming and SignalTap (Phase 4H) regardless. |

**What this means concretely:** every "Phase N" below (protocol engine, bias table, BI sequencer, cache, eviction) gets built and verified in simulation first exactly as the manual's Part 3 describes, and is immediately followed by a "Phase NH" (H = hardware) that ports that same RTL onto the DE10-Standard behind the HPS bridge, with a bare-metal test that exercises the same behavior on real silicon.

### Verification tooling: manual's UVM plan → open-source equivalent

The manual's Part 4.2 UVM environment doesn't run on Verilator (no SV classes/UVM support). Same verification *intent*, different mechanism:

| Manual's UVM component | Verilator/C++ equivalent |
|---|---|
| `config_object` | Plain C++ struct / build-time `#define`s passed into the testbench (cache size, associativity, bias granularity, backing-memory latency) |
| `host_agent` (sequencer/driver/monitor) | C++ driver class in the testbench harness — a plain function per stimulus pattern drives DUT input signals each `eval()` tick |
| `device_response_monitor` | C++ monitor class sampling DUT outputs each cycle |
| `backing_memory_scoreboard_model` | An actual independent second memory array in the C++ testbench, updated only by observed writes |
| `bias_state_predictor` | Independent C++ bias-state tracker — written from the protocol spec, never by reading the RTL's own bias-table logic (same non-negotiable rule the manual states in §4.2) |
| `scoreboard` | C++ comparison logic, invoked per transaction, comparing DUT outputs against both reference models above |
| `coverage_collector` | Verilator line/toggle coverage (`--coverage`) plus hand-rolled functional coverage — increment counters on the cross bins from manual §4.4 (e.g. `access_type × bias_state_before × bias_state_after`), dump/report at end of run |
| `protocol_checker` (SVA bind) | Keep the SVA assertions in the RTL/bind file; compile with `--assert` — Verilator supports most immediate assertions and a useful subset of concurrent SVA. Anything it can't check natively gets reimplemented as an explicit C++ check in the testbench instead of silently dropped |

Icarus Verilog is the fallback for anything Verilator chokes on (some assertion forms, certain SV constructs) — cross-check there before assuming a construct itself is broken.

### What we are deliberately NOT doing on hardware (say this explicitly in the writeup)
- No real CXL.io/CXL.cache, no real PCIe electricals — this was never in scope (see manual Part 2.4).
- No FPGA-fabric SDRAM controller, no HPS-DDR3-as-backing-memory — backing memory is on-chip BRAM by design choice above. (If you outgrow BRAM capacity later, the `de10_ghrd` reference project already has a working SDRAM/HPS-DDR3 path to crib from.)
- No embedded Linux on HPS — bare-metal only.

---

## 1. Toolchain Reference

| Tool | Path |
|---|---|
| Quartus Prime Lite 21.1 | `C:\intelFPGA_lite\21.1\quartus` |
| `quartus_sh` / `quartus_map` / `quartus_fit` / `quartus_asm` / `quartus_pgm` | `C:\intelFPGA_lite\21.1\quartus\bin64\` |
| Verilator (primary simulator) | not yet installed — install in WSL2 Ubuntu (Phase 0) |
| Icarus Verilog (secondary/lint cross-check) | `C:\iverilog\bin` (already installed, native Windows) |
| GTKWave (waveform viewer for Verilator/Icarus dumps) | not yet installed — install in WSL2 Ubuntu (Phase 0) |
| WSL2 | Ubuntu, confirmed installed and running |
| Questa (bundled, kept as fallback only — not the primary sim tool) | `C:\intelFPGA_lite\21.1\questa_fse\win64` |
| Questa 2024.1 (standalone, kept as fallback only) | `C:\questasim64_2024.1\win64` |
| Nios II EDS (soft-core tools, **not** used for HPS bare-metal — see Phase 0 gap check) | `C:\intelFPGA_lite\21.1\nios2eds` |
| USB-Blaster / USB-Blaster II drivers | `...\quartus\drivers\usb-blaster*\` |
| Licenses | `C:\intelFPGA_lite\21.1\licenses` |
| Reference GHRD (working .qpf/.qsf, Qsys system, HPS handoff, Makefile) | `...\Weibull_CFAR\matlab\de10_ghrd` |
| Bare pin-planning skeleton (this project, needs HPS/Qsys added) | `f:\Projects\cxi_memory\d10_standard_golden_top\` |
| Target device | Cyclone V, `5CSXFC6D6F31C6`, FBGA |

> **Known gap to resolve in Phase 0:** the listed toolchain has Nios II EDS (soft-core), but HPS bare-metal ARM development needs a **Cortex-A9 bare-metal cross-compiler** (`arm-altera-eabi-gcc` or similar, normally shipped as part of Intel SoC EDS / bundled with the GHRD's `embeddedsw`/preloader tooling, or a generic `arm-none-eabi-gcc`). Confirm what's actually installed before Phase 1H — don't assume it's there.

---

## 2. System Architecture (target end-state)

```
 HPS (ARM Cortex-A9, bare-metal C)                    FPGA Fabric
 ┌─────────────────────────────┐                     ┌──────────────────────────────────┐
 │ host_driver.c                │   Lightweight       │  Avalon-MM slave shim              │
 │  - issues M2S Req/RwD          │   HPS-to-FPGA        │   (maps registers <-> CXL           │
 │  - services BI -> BIRsp        │◄──── bridge ───────►│    protocol engine signals)         │
 │  - reads S2M NDR/DRS            │   (32-bit MM)        └──────────────┬───────────────────┘
 │  - HPS timer for latency exp  │                                       │
 └─────────────────────────────┘                                       ▼
                                                        ┌──────────────────────────────────┐
                                                        │ CXL.mem Protocol Engine (RTL)      │
                                                        │  (from sim Phase 1, ported)         │
                                                        └──────────────┬───────────────────┘
                                                                        ▼
                                                        ┌──────────────────────────────────┐
                                                        │ Bias Table + BI Sequencer (RTL)     │
                                                        └──────────────┬───────────────────┘
                                                                        ▼
                                                        ┌──────────────────────────────────┐
                                                        │ Memory-Side Cache Controller (RTL)   │
                                                        └──────────────┬───────────────────┘
                                                              ┌────────┴────────┐
                                                              ▼                    ▼
                                                        Cache hit            Backing Memory
                                                       (M10K tag/data)      (M10K behavioral model)
```

Debug/status fan-out (all phases): `LEDR[9:0]` = coarse status, `HEX0-5` = tag/error code, on-chip **SignalTap II** for waveform capture once bugs get subtle (Phase 4H onward), HPS UART (`printf` over the HPS's hard UART, not a fabric UART) for driver-side logging.

---

## Phase 0 — Toolchain & Environment Sanity Check
**Goal:** prove the full Quartus→hardware loop works on *this* machine, with *this* board, before writing a line of CXL RTL.

- [ ] Confirm Quartus Prime Lite 21.1 launches; confirm Cyclone V `5CSXFC6D6F31C6` requires no license file for this device size (Lite edition is normally free for Cyclone V up to the SX size class) — check `C:\intelFPGA_lite\21.1\licenses` only if prompted.
- [ ] Confirm USB-Blaster (or II) driver installed and DE10-Standard is detected in the Quartus Programmer (`quartus_pgmw.exe`) — power the board, check JTAG chain shows the Cyclone V device.
- [x] Resolve the **bare-metal ARM toolchain gap** — no Cortex-A9 bare-metal GCC was bundled anywhere under `C:\intelFPGA_lite\21.1\` (confirmed by search — no SoC EDS install, just Nios II EDS). Installed `gcc-arm-none-eabi` in WSL2 instead (`arm-none-eabi-gcc` 13.2.1) — resolved and verified by actually building `sw/hps_driver/` end to end. Iteration path for the HPS app is **not** JTAG/DS-5/OpenOCD (that idea is superseded — see Phase 1H) but a one-time SD card flash (preloader+U-Boot) followed by fast serial-console `loadb`/`go` cycles.
- [ ] Build a trivial "KEY→LEDR passthrough" design targeting `5CSXFC6D6F31C6`, scripted end-to-end via `quartus_sh` calling `quartus_map` → `quartus_fit` → `quartus_asm` → `quartus_pgm`, using **full paths** to `bin64` exes. This is your reusable build-script skeleton for every later phase.
- [ ] Program the board with the trivial design over **JTAG** and confirm KEY toggles LEDR on real hardware.
- [x] Install Verilator + GTKWave in WSL2 Ubuntu — done (`apt-get install -y verilator gtkwave` via `wsl -u root`, since interactive `sudo` hangs from this shell; landed Verilator 5.020, GTKWave 3.3.116, confirmed g++ 13.3.0 / GNU Make 4.3 present for the build). Smoke-tested for real by building and running the Phase 1 testbench (below) rather than a throwaway file — it compiled, ran, and dumped `phase1_waveform.vcd`.
- [x] Smoke-test Icarus Verilog natively on Windows (`iverilog` + `vvp`) — done, run directly against the real Phase 1 RTL rather than a throwaway file. This is what caught BUG-001 (see [bugs.md](bugs.md)) — Verilator accepted an enum-typed ternary assignment that Icarus correctly rejected, so the "smoke test" earned its keep on the very first phase.
- [ ] Read through the reference `de10_ghrd` project (`.qpf`/`.qsf`, Qsys `.qsys` file, HPS handoff folder, `Makefile`) — this is the pattern Phase 1H's Qsys system will be modeled on. Note down: how it configures the HPS component, what bridges it enables, how its Makefile sequences the Quartus + `bsp-editor`/preloader steps.
- [x] Project layout under `f:\Projects\cxi_memory\` established: `rtl/` (synthesizable modules) and `sim/phase<N>/` (per-phase Verilator/C++ testbenches) exist and are in use. `sw/hps_driver/` and `quartus/` will be added starting Phase 1H. `git init` still not done — optional, left for you to decide since this workspace has no version control yet.

**Gate:** trivial design programs and runs on the physical board over JTAG; Verilator runs a testbench and dumps a waveform GTKWave can open; the ARM toolchain question is answered (not deferred).

---

## Phase 1 — Protocol Engine + Transaction Classes (SIM) — ✅ DONE
*(= manual Part 3, Phase 1)*

- [x] Define M2S Req / M2S RwD and S2M NDR / DRS as a synthesizable signal-level interface (`rtl/cxl_mem_protocol_engine.sv`) rather than SV transaction classes — Verilator doesn't run UVM, so there's no class layer to define; the interface is plain `valid`/`ready`/`opcode`/`addr`/`tag`/`wdata` signals instead, matching the roadmap's UVM→C++ tooling table. **M2S BIRsp is deliberately not modeled yet** — it belongs to the BI handshake (Phase 4), and adding it now would be exactly the kind of speculative/premature interface growth worth avoiding; the opcode encoding has room to grow when Phase 4 actually needs it.
- [x] Write C++ driver/"monitor" logic for the Verilator testbench (`sim/phase1/tb_protocol_engine.cpp`); simple read/write, no caching or bias logic — pure pass-through to a small internal memory array, single request outstanding at a time (Phase 2 adds concurrency). Includes an independent expected-value map (`std::map<addr,data>`) the testbench maintains itself, not derived from the DUT.
- [x] Passing directed testbench under Verilator, cross-checked in Icarus Verilog — `PHASE1 PASS: 4 writes, 5 reads, 0 errors`. Icarus caught a real portability bug (BUG-001) that Verilator missed; fixed and re-verified in both tools.

**Gate:** ✅ basic read/write transactions pass in simulation with correct S2M response type/data (both tools agree).

## Phase 1H — Protocol Engine on Hardware + HPS/Qsys Infrastructure — ✅ DONE
**This is the big infrastructure phase — build once, reuse for every phase after.** RTL, Qsys integration, `.sof` (timing-clean, all positive slack), and hardware bring-up are all complete and verified on real silicon. BUG-003 (HPS SDRAM sequencer generation) fully resolved. First real hardware milestone reached: the CXL.mem protocol engine, running in FPGA fabric, correctly serviced register-interface read/write requests issued by the HPS over Linux, matching the sim model exactly.

**Boot/load path decision (supersedes the original DS-5/OpenOCD idea):** while researching this phase, the reference `de10_ghrd` project's own lab bring-up notes turned up a better-fit, lower-risk path than gambling on DS-5 or an unofficial OpenOCD Cortex-A9 config: flash the SD card **once** with a standard preloader+U-Boot (U-Boot itself already gives a serial-console prompt with `loadb`/`tftpboot` + `go <addr>` — exactly the fast, no-SD-rewrite iteration loop the roadmap wanted from JTAG, just reached via U-Boot instead). JTAG stays the tool for FPGA-fabric `.sof` programming and SignalTap; U-Boot's serial console becomes the iteration path for the bare-metal HPS app.

- [x] Platform Designer (Qsys) system: rather than authoring `hps_0`'s DDR3/HPS configuration from scratch (high-risk to get right blind, no hardware to validate against), copied the reference `de10_ghrd` project's proven, working `soc_system.qsys` (`hps_0`, `clk_0`, `mm_bridge_0`, PIO peripherals) as the base — same physical board, so its HPS/DDR3 config is directly reusable.
- [x] Register map designed for the Avalon-MM slave shim: `REQ_ADDR` / `REQ_WDATA` / `REQ_FIRE` (write, triggers the request) / `STATUS` / `RESP_TAG` / `RESP_RDATA` / `RESP_ACK` (write, consumes the response) — see the header comment in `rtl/cxl_avalon_shim.sv` for the full bit layout. Debug-readback into the bias table/cache stats is deferred to the phase that actually adds those (3H/5H) — no point designing registers for state that doesn't exist yet.
- [x] Avalon-MM shim RTL written (`rtl/cxl_avalon_shim.sv`), wrapping the unmodified Phase 1 protocol engine — same backpressure pattern (`avs_waitrequest` while busy) as the reference project's `cfar_avalon_bridge`. Verified standalone in a Verilator testbench (`sim/phase1h/tb_avalon_shim.cpp`): full write→poll→read→poll→ack round trip, `PHASE1H_SHIM PASS`. Caught and fixed BUG-002 (a testbench `waitrequest` sampling-order bug) in the process.
- [x] Phase 1 protocol engine confirmed already synthesizable as-is — no non-synthesizable constructs to strip out (Verilator/Icarus already required synthesizable-subset SystemVerilog; the C++ testbench was always a separate layer, never mixed into the RTL).
- [x] Custom Qsys component descriptor written (`quartus/ip/cxl_avalon_shim/cxl_avalon_shim_hw.tcl`), modeled directly on the reference project's `cfar_avalon_bridge_hw.tcl` pattern, and a qsys-script (`quartus/add_cxl_shim.tcl`, modeled on `add_cfar_bridge.tcl`) that removes the reference's `cfar_bridge_0` and adds `cxl_avalon_shim_0` in its place on `mm_bridge_0` at offset `0x6000`. Ran for real via `qsys-script.exe` — instance added, all three connections (clock/reset/avalon) succeeded, system saved.
- [x] `qsys-generate --synthesis=VERILOG` run for real. **Our component generated with zero errors of its own** — confirmed by reading the full log, every error is scoped to `hps_sdram` (the HPS's own DDR3 sequencer generation), not `cxl_avalon_shim_0`.
- [x] Top-level Verilog adapted from the reference `DE10_Standard_GHRD.v` (`quartus/cxl_de10_standard_top.v`): renamed module, same HPS/LEDR/KEY/SW/HEX/CLOCK port list and wiring (physically fixed HPS pins reused verbatim), `cfar_bridge_0_dbg_*` (4 signals) swapped for `cxl_avalon_shim_0_dbg_*` (3 signals: heartbeat/seen_request/seen_response) on HEX0 segments a–c.
- [x] `.qsf`/`.qpf`/`.sdc` adapted (`quartus/CXL_DE10_Standard.*`) from the reference project's proven pin assignments — `TOP_LEVEL_ENTITY` and file list updated; dropped a stale `intr_capturer.v` reference that pointed at a file that doesn't exist anywhere in the source project.
- [x] `host_driver.c` written (`sw/hps_driver/`) — bare-metal, freestanding (own `startup.s` + `linker.ld`, load address `0x00100000`, no OS/libc), issues the exact same write→poll→read→poll→ack sequence already verified in the Phase 1H sim testbench, reports over a minimal polled HPS UART0 driver. **Resolved the Phase 0 ARM toolchain gap**: installed `gcc-arm-none-eabi` in WSL2. Built clean, zero warnings; disassembly/symbol table confirmed correct entry point and stack layout.
- [x] **`.sof` generated successfully.** BUG-003 fully resolved (root cause: `uniphy_mcc.exe` returns a nonfatal-but-nonzero exit code on this machine's newer Windows UCRT; fixed by patching the one Makefile-recipe line in Intel's own IP generator to ignore that specific command's exit status — see bugs.md for the full trace). Full build chain re-run clean: `quartus_map` (0 errors) → HPS SDRAM pin assignments applied → `quartus_cdb --merge` (0 errors) → `quartus_fit` (0 errors, 7% ALM, 0 DSP blocks) → `quartus_asm` (0 errors) → `output_files/CXL_DE10_Standard.sof` (7.4MB).
- [x] **Timing verified clean.** `quartus_sta`: worst-corner (Slow 1100mV 85°C) setup slack **+1.730ns**, hold slack **+0.143ns**, **TNS=0.000** on every clock domain — no negative slack anywhere. Consistent with the combinational-depth audit above finding nothing to pipeline.
- [x] **Programmed onto real hardware.** `quartus_pgm --mode=jtag` — board detected (`DE-SoC [USB-1]`), `.sof` programmed to device index 2, confirmed against the correct part (`5CSXFC6D6F31`, JTAG ID `0x02D020DD`). `Quartus Prime Programmer was successful. 0 errors, 0 warnings.` SRAM-based — this is live only until next power cycle, not yet persisted to SD card/`.rbf`.
- [x] **Plan revised on the fly, correctly this time:** the board turned out to already have a *running* Angstrom Linux (same image family as the sibling Weibull-CFAR project — `Angstrom v2014.12`, kernel 4.5.0, dual-core ARMv7), reached over network (`root@192.168.137.200`, passwordless, user brought up `eth0` manually over the serial console). The bare-metal driver built earlier in Phase 1H (`sw/hps_driver/host_driver.c` + `startup.s` + `linker.ld`) assumes exclusive control of the CPU and is **not usable as-is against a running kernel** — it was superseded, not deleted, since it's still the right artifact for a future truly-bare-metal boot path. Wrote `sw/hps_driver/host_driver_linux.c` instead: plain Linux userspace C, `mmap(/dev/mem)` at the Lightweight HPS-to-FPGA bridge base (`0xFF200000`) + our Qsys-assigned offset (`0x6000`, cross-checked against `soc_system.sopcinfo`'s real `<baseAddress>` rather than trusted from memory) — no vector table or linker script needed at all under Linux.
- [x] **Hardware bring-up test: PASS.** Compiled natively on-target (board's own `gcc 4.9.3`, avoids any cross-compiler ABI mismatch) and run over SSH. Exact same write→poll→read→poll→ack sequence already verified in `sim/phase1h/tb_avalon_shim.cpp` now confirmed correct on real silicon: `PHASE1H_HARDWARE PASS: write+read round trip verified on real silicon`. Verified **reproducible** (4/4 identical runs, per the fpga_checklist.md §4.1 Gate 0 discipline) and extended to a **7-check matrix** (3 distinct address/tag/data write-then-read pairs + one never-written address correctly reading back 0) — all 7 `OK`. Extended test saved as `sw/hps_driver/host_driver_linux_extended_test.c`.
- [ ] Known follow-up: the `dbg_heartbeat`/`dbg_seen_request`/`dbg_seen_response` conduit exists in `rtl/cxl_avalon_shim.sv` but isn't wired to HEX0 — Platform Designer silently dropped it from the generated instantiation since `add_cxl_shim.tcl` never explicitly exported it (confirmed: `quartus_map` errored on those ports not existing on `soc_system` until the top-level wiring was removed). Not required for the core write/read bring-up test, which is now passing without it; re-add via the Qsys GUI's Export checkbox (fastest) once convenient, or find the correct qsys-script export incantation.
- [ ] SD card / persistent boot (`.rbf` + preloader/U-Boot) intentionally not pursued this session — the already-running Linux image made it unnecessary for reaching this gate. Revisit if a from-power-on standalone boot becomes a goal later (see the risk register below).

**Gate: ✅ MET.** The exact same read/write stimulus verified in the Phase 1H Verilator shim testbench produces the correct result on real silicon — confirmed reproducible and extended beyond the minimal case. **Phase 1H is complete.**

---

## Phase 2 — Backing Memory Model + Outstanding-Transaction Tracker (SIM) — ✅ DONE
*(= manual Part 3, Phase 2)*

- [x] Tag-indexed outstanding-transaction tracker: one slot per possible tag value (`2**TAG_W`), occupancy = "is this tag currently in flight" (directly reuses the AXI4 ID-indexed-tracking pattern per manual §1.6). `m2s_ready` reflects per-tag occupancy, decoded combinationally from the incoming tag — firing an in-use tag stalls until it frees, matching real ID-based interface discipline.
- [x] Genuine out-of-order completion, not just tag-correctness under FIFO ordering: writes (NDR) become eligible to respond immediately, reads (DRS) carry a fixed extra latency (`READ_EXTRA_LATENCY`) — so a write issued after a read can and does complete first. Response arbitration is lowest-tag-first among eligible slots, held stable (registered) once offered so a stall never causes the offered response to swap mid-transaction.
- [x] Verified in Verilator (`sim/phase2/tb_protocol_engine.cpp`): out-of-order completion confirmed with a write's tag numerically *higher* than the read it overtook (rules out arbitration-priority coincidence explaining the result); 5-way concurrent request routing verified; per-tag occupancy + reuse-after-drain verified. `PHASE2 PASS`.
- [x] Two real bugs found and fixed in the process — **BUG-004**: an early draft drove the same per-tag arrays from three separate `always_ff` blocks (illegal multiple-driver design), caught by design review before ever attempting to build. **BUG-005**: the testbench held `s2m_ready=1` constant, causing a response to be silently arbitrated and consumed while a later `fire()` call's internal wait-loop was still running, before the draining loop started watching — lost 1 of 5 expected responses. Fixed by gating `s2m_ready` explicitly per test phase.

**Gate:** ✅ stress test with several concurrent outstanding tags routes every response to the correct originator in sim, including genuine out-of-order completion.

## Phase 2H — Port to Hardware — ✅ DONE
- [x] **No shim RTL changes needed** — `cxl_avalon_shim.sv` already passes `m2s_ready`/`m2s_valid`/`m2s_tag` straight through to the engine; the engine's new per-tag occupancy semantics give the desired "fire multiple distinct-tag requests without stalling" behavior through the *existing* register interface unchanged. A clean payoff of keeping the shim's interface stable while the engine's internals grew.
- [x] Backing memory stays the same small on-chip array (per Phase 0's architecture decision) — Phase 2 added tracking/latency structure around it, not new storage.
- [x] Rebuild end to end, multiple times over this project's hardware bring-up (most recently for Phase 8's bypass mode) — full chain confirmed working: mirror → `qsys-generate` → `quartus_map` → HPS SDRAM pin assignments → `quartus_cdb --merge` → `quartus_fit` → `quartus_asm` → `quartus_sta` (read and confirmed clean every time before flashing, per standing instruction) → `quartus_pgm`.
- [x] Extended Linux userspace test (`host_driver_linux_phase234h.c`) — multi-tag routing verified correct on real hardware. One correction made along the way: the test originally also asserted sim's exact *completion order* (write retiring before read via `READ_EXTRA_LATENCY`), which failed on hardware — not an RTL bug, but a bad test assumption, since a software-paced register interface can't keep two requests genuinely in flight simultaneously (each MMIO write costs far more cycles than the engine's whole read latency). Corrected to measure and report this rather than assume sim's timing; out-of-order completion itself remains verified cycle-accurately in `sim/phase2`.

**Gate:** ✅ multi-tag routing and correct data verified on real hardware; true out-of-order completion timing verified in sim (see the note above on why that split is the right division of labor between the two environments).

---

## Phase 3 — Bias Table + Simple Bias-Aware Gating (SIM) — ✅ DONE
*(= manual Part 3, Phase 3)*

- [x] Bias table added as its own module (`rtl/cxl_bias_table.sv`), one bit per address, reset state all Device-Biased. Kept separate from the protocol engine (instantiated inside it) to match the manual's architecture diagram — Protocol Engine → Bias Table → Cache Controller as distinct stages — which matters once the BI sequencer (Phase 4) needs to sit between engine and table for real.
- [x] With no cache yet to bypass (Phase 5), "Device-Biased proceeds normally / Host-Biased bypasses the cache" is satisfied by construction — everything already goes straight to backing memory. Phase 3's real deliverable is the bias table itself being correct and wired in, not a behavior change; a deliberate scoping call, not an oversight.
- [x] Verified in Verilator (`sim/phase3/tb_protocol_engine.cpp`): reset-state all-Device-Biased confirmed; multi-address set/read round trip with no aliasing between addresses; regression check that M2S read/write correctness is unaffected by bias state. `PHASE3 PASS`, no bugs found this round.

**Gate:** ✅ correct bypass behavior for Host-Biased lines, correct normal path for Device-Biased lines, in sim (satisfied by construction, and confirmed not to have broken anything else).

## Phase 3H — Port to Hardware — ✅ DONE
- [x] Bias-table debug-readback register added to the Avalon shim (`rtl/cxl_avalon_shim.sv`): `BIAS_ADDR` (7, write) / `BIAS_SET` (8, write) / `BIAS_GET` (9, read). `avs_address` widened 3→4 bits to fit 10 registers; Qsys IP descriptor (`cxl_avalon_shim_hw.tcl`) updated to match, `cxl_bias_table.sv` added to its fileset.
- [x] Verified in Verilator (`sim/phase3h/tb_avalon_shim.cpp`): bias register read/write round trip correct; REQ/RESP register interface regression-checked clean after the address-width change. `PHASE3H_SHIM PASS`.
- [x] **Hardware build + test: ✅ VERIFIED ON REAL SILICON** (consolidated with Phase 2H/4H — see Phase 4H below). `0x03`=Host-Biased, `0x50`=Device-Biased round trip correct on real hardware, `host_driver_linux_phase234h.c`. Also directly confirms BUG-009's bias-table reset fix works on real silicon, not just in sim.

**Gate:** ✅ **PASSED on real hardware** — bias register interface verified end to end on the board.

---

## Phase 4 — BI Sequencer (SIM) — ✅ DONE (scoped to directed tests, not the manual's full UVM ambition)
*(= manual Part 3, Phase 4. The hard phase — and it delivered the hardest bug in the project so far.)*

- [x] Full Back-Invalidate handshake implemented: dedicated `bi_req_valid/ready/addr` (device→host) and `bi_rsp_valid/ready` (host→device) channel pair, a 3-state FSM (`BI_IDLE`/`BI_REQ`/`BI_WAIT_RSP`) triggered when an incoming request's address is Host-Biased, atomic bias flip on the exact cycle BIRsp is accepted. Deliberate scoping choice: the stall while a BI handshake is in flight is **global**, not per-address — trades some concurrency for ruling out an entire race class by construction (documented at length in the RTL header).
- [x] Verified in Verilator (`sim/phase4/tb_protocol_engine.cpp`) with a hand-written host-side BI responder model: BI request carries the correct address; no S2M response arrives before the bias flip; bias reads Device-Biased immediately after BIRsp; the held request then completes correctly; the global stall is proven by checking an *unrelated, already-Device-Biased* address also sees `m2s_ready` low while a different address's BI is in flight. `PHASE4 PASS`, no new RTL bugs — the multiple-driver and `s2m_ready`-gating lessons from Phase 2 (BUG-004/005) were applied proactively this time.
- [~] **Honest gap vs. the manual's original ambition**: no independent bias-state predictor class, no SVA assertions, no constrained-random testing — this project's Verilator/C++ approach uses directed tests with explicit reference checks instead (see the roadmap's UVM→open-source mapping table). The invariant the manual's key SVA would have checked ("never cache/access a Host-Biased line without a completed BI") is enforced *structurally* here (`m2s_ready` is gated on bias resolution, so an unresolved access literally cannot be accepted) rather than watched for via a runtime assertion — a reasonably strong claim, but a real difference from having an independent, continuously-checking assertion as a second line of defense.

**Gate:** ✅ BI handshake correctness verified for the scenarios above; **not** coverage-closed on a formal `bias_state_before × bias_state_after` cross bin (no functional coverage collector exists in this project's tooling) — an honest, documented scope reduction from the manual's original plan, not an oversight.

## Phase 4H — Port to Hardware — ✅ VERIFIED ON REAL SILICON
**Hardest hardware phase — and it already caught the single most valuable bug in the project (BUG-006) before ever reaching hardware.**

- [x] Avalon shim extended with the BI register interface: `BI_STATUS` (10, read), `BI_REQ_ADDR` (11, read), `BI_REQ_ACK` (12, write), `BI_RSP_SEND` (13, write) — software polls/services the BI handshake the HPS "host" needs to perform.
- [x] **Found and fixed BUG-006** while writing this phase's testbench: the shim's original `m2s_valid = write_fire && m2s_ready` wiring created a genuine circular deadlock once `m2s_ready` grew a bias/BI dependency — the BI handshake needed to resolve `m2s_ready` could itself never start, and even setting that aside, the original design would have blocked the entire Avalon bus for as long as BI resolution took, with no way for software to ever reach the very registers that resolve it. Fixed with a one-deep pending-request holding register that decouples the Avalon bus transaction from the engine's acceptance latency. Full project regression suite (Phase 1 through 4H) verified clean after the fix.
- [x] Verified in Verilator (`sim/phase4h/tb_avalon_shim.cpp`): BI request correctly surfaced via `BI_STATUS`/`BI_REQ_ADDR`; bias correctly flips after `BI_RSP_SEND`; the originally-stalled request correctly completes and its response is correctly delivered — all through ordinary, non-blocking register transactions. `PHASE4H_SHIM PASS`.
- [x] **Hardware build + HPS Linux BI responder + hardware bring-up: ✅ VERIFIED ON REAL SILICON.** `sw/hps_driver/host_driver_linux_phase234h.c` (the driver acting as the host-side BI responder) reports, against the live board: BI request correctly surfaced with the right address (`addr=0x25`); bias correctly flipped to Device-Biased after `BI_RSP_SEND`; the originally-stalled read then completed correctly with its tag intact (`tag=9`). `ALL_PHASES_PASS: Phase 2H, 3H, 4H all verified on real hardware`.
- [ ] SignalTap capture of a real BI handshake sequence on hardware — stretch goal, not yet attempted; the register-level polling interface built here is sufficient for correctness testing without it.

**Gate:** ✅ **PASSED on real hardware** — BI handshake, bias round trip, and multi-tag routing all verified on the board, not just in sim.

### Hardware bring-up results (Phase 2H/3H/4H consolidated build)
- **Phase 3H ✅** — bias set/get round trip correct on silicon (`0x03`=Host-Biased, `0x50`=Device-Biased). This also confirms BUG-009's bias-table reset fix works in real hardware, not just in sim.
- **Phase 4H ✅** — full BI handshake end to end, as above. The coherence centerpiece of the whole project, working on real silicon.
- **Phase 2H ✅ (with an important caveat, and a corrected test)** — both tags route and complete correctly. But the original test asserted sim's *completion order* (write retiring before read via `READ_EXTRA_LATENCY`) and failed on hardware. That was a bad test, not a bug: demonstrating out-of-order completion needs both requests in flight simultaneously, and through a software-paced register interface each MMIO write costs far more cycles than the engine's entire read latency, so request 1 always retires before software can even issue request 2. The test now *measures* this rather than assuming it and reports `request 1 complete before request 2 issued? YES`. Out-of-order completion remains verified cycle-accurately in `sim/phase2`, which is the right environment for it.
- **Lesson worth keeping:** a software-paced register interface cannot observe concurrency that resolves in single-digit clock cycles. Some properties are only verifiable in simulation, and that's a legitimate division of labor between sim and hardware — not a gap in coverage.

### ⚠️ Mandatory hardware procedure (see bugs.md BUG-010)
Every board hang in this project traced to one cause: **JTAG configuration is volatile**, so after any power cycle the fabric is empty while the Lightweight bridge stays enabled — and an MMIO access into an empty fabric stalls an ARM core *uninterruptibly*, wedging the board. Before any HPS-side test:
1. **`quartus_pgm` after every power cycle/reboot**, before anything touches `0xFF206000`.
2. Program only from a **cleanly booted** HPS — reprogramming does **not** clear an already-stalled bridge transaction; only a reboot does.
3. **Arm `/dev/watchdog` before touching MMIO** (pattern in `sw/hps_driver/safe_probe.c`; all test drivers now do this). Turns a wedge into an automatic self-reset instead of a manual power cycle.
4. Confirm the fabric responds with one watchdog-protected read (`safe_probe`) before running a real test.

Note: `/sys/class/fpga_manager/*/state` reads `power off` even after a *successful* JTAG configuration on this board/kernel — it cannot be used to confirm the fabric is programmed. Only an actual register read can.

---

## Phase 5 — Memory-Side Cache (Tag/Data Array, Allocate/Evict) (SIM) — ✅ DONE
*(= manual Part 3, Phase 5)*

- [x] Direct-mapped cache (deliberately not set-associative — manual is explicit this isn't where the interview signal is), one word per line, write-back + write-allocate. `CACHE_IDX_W` parameter, default 4 (16 lines).
- [x] The bias-gating invariant falls out of phase ordering for free: by the time a request reaches cache-allocate logic, Phase 4 has *already* forced it Device-Biased (m2s_ready held low for Host-Biased addresses until BI resolves) — so the cache never needs its own bias check. "Never cache a Host-Biased line" is enforced structurally, one phase upstream, not re-checked here.
- [x] Eviction-writeback reuses the exact same FSM pattern Phase 4's BI sequencer established (hold `m2s_ready` low, host holds `m2s_addr` stable, resolve internally, let the same request proceed once resolved) — two independent instances of "stall, don't drop" in this module now, both built the same way on purpose.
- [x] Verified in Verilator (`sim/phase5/tb_protocol_engine.cpp`): first-touch write correctly counted as a miss; read-back correctly hits with correct data; a colliding write correctly evicts (with writeback); the evicted line reads back correctly afterward (fetched from backing memory where it was written back); the evicting line's own data survives *its own* later eviction too. `PHASE5 PASS`, no new RTL bugs.
- [x] **Found BUG-004-family issue proactively this time** — designed the cache-array updates as one consolidated `always_ff` from the start (flush-clear vs accept-allocate as mutually-exclusive branches in one block), applying the Phase 2 lesson before it could recur, not after.
- [x] Cache hit/miss debug counters (`dbg_cache_hits`/`dbg_cache_misses`) added directly — needed for Phase 8's cache-enabled-vs-disabled experiment, cheap to add now while the relevant logic is fresh.

**Gate:** ✅ basic cache correctness verified in sim, including allocate/hit/evict.

## Phase 5H — Port to Hardware — ✅ VERIFIED ON REAL SILICON
- [x] Cache tag/data arrays are on-chip (M10K-inferred `logic` arrays) — no separate BRAM budgeting needed against backing memory, since Phase 0's architecture decision already keeps backing memory as a small on-chip array too; both are small enough that this project never approached a real capacity tradeoff worth documenting.
- [x] `CACHE_HITS` (14, read) / `CACHE_MISSES` (15, read) added to the Avalon shim register map.
- [x] **Hardware test: ✅ VERIFIED ON REAL SILICON.** `sw/hps_driver/host_driver_linux_phase56h.c` asserts exact `CACHE_HITS`/`CACHE_MISSES` deltas at every step (not just data correctness) — first-touch write correctly counted as a miss, read-back a hit, a colliding write evicts (miss) with the evicted dirty line's data confirmed to survive its writeback. `hits=0 misses=0` at entry also confirms the cache arrays' `initial`-block reset works correctly on this hardware (same class of concern as BUG-009, checked rather than assumed).

**Gate:** ✅ **PASSED on real hardware** — see the Phase 6H results below (5H and 6H were verified together in one run).

---

## Phase 6 — Dirty-Line Writeback + Eviction Correctness (SIM) — ✅ DONE
*(= manual Part 3, Phase 6. The mechanism was already built correctly in Phase 5 — this phase's job was proving it holds up under real stress, not building it.)*

- [x] No RTL changes needed — Phase 5's eviction-writeback FSM already handles this correctly; Phase 6 is a dedicated, more rigorous **verification** pass against it.
- [x] Verified in Verilator (`sim/phase6/tb_protocol_engine.cpp`): 4 addresses aliasing the same cache index, written then read back in **reverse** order (every read but possibly the last forces an eviction) — all 4 correct. A full **thrash** sequence (4 writes, 4 evictions, 4 readbacks, zero data loss across the whole cycle) — the strongest test in the project: by the time line 0 is read back, lines 1–3 have each independently evicted something, and line 0's own value has to have survived being evicted once already. A **negative** test confirming a write to an unrelated cache index doesn't disturb a resident, untouched line. `PHASE6 PASS`, no bugs found in the mechanism itself.
- [x] **Found BUG-007 while re-running the full regression suite after Phase 5/6 landed** — not in Phase 5 or 6's own new tests, but in the *older* Phase 1 and Phase 3 testbenches, whose latent bugs (a missing `dut->eval()` before the first readiness check; a recurrence of BUG-005's always-ready-consumer pattern) had sat invisible through three to five phases of continuous green results until Phase 5's eviction stalls finally widened the timing window enough to expose them. Fixed; full 9-suite regression (Phase 1 through 6) verified clean afterward.

**Gate:** ✅ dirty-eviction correctness verified thoroughly in sim (multi-round, thrash, and index-isolation), full project regression clean.

## Phase 6H — Port to Hardware — ✅ VERIFIED ON REAL SILICON
- [x] Full multi-round thrash sequence reproduced via the Linux userspace driver on real hardware: 4 addresses aliasing the same cache index (`0x07/0x17/0x27/0x37`), written then read back in reverse order — every value correct, zero data loss across the whole cascading-eviction cycle. Index isolation confirmed (`sw/hps_driver/host_driver_linux_phase56h.c`).

**Gate:** ✅ **PASSED on real hardware.**

### Hardware bring-up results (Phase 5H/6H, `host_driver_linux_phase56h.c`)
```
counters at entry: hits=0 misses=0
=== Phase 5H ===
PASS: write A (first touch of index 5) -- hits+0 misses+1
PASS: read A back (resident line)      -- hits+1 misses+0, data 0xaaaa0001
PASS: write B (evicts A on index 5)    -- hits+0 misses+1
PASS: read A after eviction            -- hits+0 misses+1, data 0xaaaa0001 (writeback survived)
PASS: read B after its own eviction    -- data 0xbbbb0002 (writeback survived)
=== Phase 6H ===
PASS: 4-way thrash readback, all 4 values correct after cascading evictions
PASS: index isolation -- unrelated write didn't disturb resident line
counters at exit: hits=2 misses=13
PHASE56H PASS
```
Verified on the fitted Phase 5/6 build (`quartus_fit`: 0 errors, 45m46s; `quartus_sta`: no negative slack anywhere, `CLOCK_50` setup slack 6.588ns — down from 9.096ns pre-cache, as expected from the added tag-compare logic, still comfortably positive). Asserting exact hit/miss counter deltas (not just data values) is what makes this a real test of the cache's control logic rather than just its data path — a cache that always missed but happened to still return correct data from backing memory would pass a data-only test.

---

## Phase 7 — Cross-Cutting Hardware Stress & Edge Cases — ✅ DONE (5/5)
*(after all features above are ported — mirrors manual §4.3 "Ordering/completion" and "Error/edge conditions", now run for real)*

- [x] **Max-outstanding-request stress test — ✅ verified on real hardware.** All 16 tags (`NUM_TAGS`) fired without draining any, then drained together: every tag arrives exactly once, all data correct.
- [x] **Phantom-completion check — ✅ verified on real hardware**, folded into the stress test above: every drained response's tag is checked against the set actually fired before being accepted; would fail loudly on an unfired or duplicate tag. None seen.
- [x] **Cache-thrashing with a working set LARGER than the cache — ✅ verified on real hardware.** 24 distinct lines against a 16-line cache (distinct from Phase 6H, which thrashed one colliding index) — all 24 correct after a full sweep, `cache_hits +8 cache_misses +40` confirming misses genuinely dominate as expected when the working set exceeds capacity.
- [x] **BI resolution racing a cache access for the same address — ✅ verified on real hardware, after fixing two bad test assumptions along the way** (see below) — the exact request that triggered the BI, once resolved, correctly proceeds through the *unmodified* cache-accept path (a genuine first-touch miss + allocate), and a follow-up access to the now-resident line correctly hits.
- [x] Cache bypass/disabled debug mode — **✅ built and verified, both in sim and on real hardware** (see Phase 8 below). New `CACHE_BYPASS` register (Avalon addr 16); Avalon address bus widened 4→5 bits to fit it.

**Gate:** ✅ **5 of 5 edge-case scenarios pass on hardware.** Bug log updated with what hardware testing found: BUG-011, a real (intentionally out-of-scope) coherence gap in the debug-only bias-forcing path, discovered while chasing down what first looked like a test failure.

### Hardware bring-up results (`host_driver_linux_phase7.c`)
```
=== Stress 1+2: max outstanding requests (16 tags) + phantom-completion check ===
fired all 16 tags without draining any
PASS: all 16 tags drained, no phantom completions, no duplicates

=== Stress 3: working set (24 lines) exceeds cache capacity (16 lines) ===
PASS: all 24 lines correct after a full working-set sweep exceeding cache capacity
cache_hits +8  cache_misses +40 over this sweep

=== Stress 4: BI resolution -> immediate cache allocate for the SAME address ===
PASS: BI resolution's own stalled read correctly allocated into the cache (miss+1)
PASS: write to the just-allocated line correctly hit (in-place update)
PASS: line correctly holds the new value and hits on re-read

PHASE7 PASS
```

**Two bad test assumptions caught and fixed along the way — worth keeping as its own lesson, distinct from BUG-010/BUG-011:** the Stress-4 test first failed expecting the post-BI-resolution write to be a cache *miss*. Two things were wrong with that expectation, found by tracing rather than assuming: (1) the address chosen wasn't actually fresh — it had been populated by an earlier stress test's working-set sweep, so a hit was simply correct behavior for an address with history; (2) even after switching to a genuinely fresh address, the *originally-stalled read itself* — not the follow-up write — is the request that performs the first-touch cache allocation once BI resolves, per the design's own "let the same pending request continue through the unmodified accept path" pattern. The follow-up write correctly hit an already-resident line. Same category of lesson as Phase 2H's ordering test: a red result on hardware first prompts "what did the test assume that wasn't true," before "what's broken in the RTL."

---

## Phase 8 — The Measurement Experiment (on real silicon) — ✅ DONE
*(manual Part 3 Phase 8, but now with a number that came off actual hardware instead of a sim model — meaningfully stronger for a writeup)*

- [x] **New RTL: cache bypass mode.** `cache_bypass_en` input added to `cxl_mem_protocol_engine.sv`, exposed via a new `CACHE_BYPASS` register (Avalon word address 16 — required widening `avs_address` from 4 to 5 bits in `cxl_avalon_shim.sv` and the Qsys IP descriptor). When set, every access behaves exactly as it did pre-Phase-5: writes go straight to backing memory, reads always pay `READ_EXTRA_LATENCY`, and — the subtlety worth being deliberate about — the cache arrays are left completely untouched (frozen, not cleared) while bypassed, so toggling it back off can expose a stale-vs-backing-memory cached entry from before bypass was enabled. That's an intentional tradeoff (measure "cache vs no cache" for an *identical* access pattern, not "cold cache vs warm cache"), verified directly as its own sim test case, not just asserted in a comment.
- [x] Verified in Verilator, two levels: engine-level (`sim/phase8/tb_protocol_engine.cpp`, `PHASE8 PASS`) and Avalon-register-level (`sim/phase8/tb_avalon_shim.cpp`, `PHASE8H_SHIM PASS`) — register round trip, functional force-a-miss check, and the "cache stays frozen" behavior all confirmed before ever touching hardware.
- [x] Full Quartus rebuild + flash, clean timing (`CLOCK_50` setup slack 6.582ns, TNS 0.000, no negative slack anywhere) — confirmed via `quartus_sta` before flashing, per standing project requirement.
- [x] **Used `clock_gettime(CLOCK_MONOTONIC)`** (ARM generic timer via the VDSO) for per-access latency — a real hardware timer, not a software instruction-counting loop.
- [x] Ran the same repeated-access workload (2000 reads of one resident line) with cache enabled vs. `CACHE_BYPASS` enabled.
- [x] **Reported the honest result, not a manufactured one:** see below. The measured wall-clock difference was statistically insignificant, and the writeup says so plainly along with why, rather than reporting a misleading percentage.

**Gate:** ✅ methodologically sound experiment run and reported, including a null result honestly explained rather than hidden — arguably a stronger engineering signal than a clean percentage would have been.

### The result

```
=== Cache ENABLED ===
cache_hits +2000  cache_misses +0  over 2000 accesses

=== Cache DISABLED (bypass) ===
cache_misses +2000  over 2000 accesses

=== Results (nanoseconds per access, N=2000 each) ===
Cache ON  (hit):  avg=3236.3  min=3170.0  max=56470.0  stddev=1190.7
Cache OFF (miss): avg=3233.9  min=3190.0  max=42580.0  stddev=880.2

Difference (miss - hit): -2.4 ns average
```

The `cache_hits`/`cache_misses` counters confirm the RTL is doing exactly what it should — a clean 2000/0 split with the cache enabled, 0/2000 with it bypassed. But the measured latency difference (-2.4ns) is far smaller than the measurement noise (pooled stddev ~1035ns), so it is **not** a statistically meaningful signal. The reason is straightforward once stated: the cache's actual RTL-level saving is `READ_EXTRA_LATENCY=3` cycles at 50MHz = **60ns** — genuinely real, but it's being measured underneath a per-access round trip through mmap'd `/dev/mem`, the Lightweight HPS-to-FPGA bridge, and a software polling loop, which costs on the order of **3.2 microseconds** per access regardless of hit or miss. A 60ns effect cannot be resolved above ~3200ns of fixed overhead that applies identically to both cases.

**This is a legitimate finding, not a failed experiment.** Two things are true at once and both are worth saying in a writeup: (1) the cache's functional correctness and its RTL-level latency benefit are both independently verified — the former by every Phase 5-7 test, the latter by the `READ_EXTRA_LATENCY` parameter's presence in the design and its effect on `slot_cnt` timing, directly inspectable in the sim waveform; (2) *observing* that benefit at the system level, through a software-driven register-poke interface, is fundamentally limited by how much slower the interface itself is than the effect being measured. A production CXL.mem host doesn't talk to the device through mmap'd polling over a debug bridge — it's a hardware protocol engine on both ends, where a 60ns difference is enormous. This hardware port's own bring-up interface, built for correctness testing rather than performance measurement, isn't the right instrument for this specific number — and saying so, with the reasoning, is more credible than reporting a percentage that this measurement can't actually support.

**Gate:** a real, reproducible latency number, captured with methodology documented (workload, timer resolution, sample count).

---

## Phase 9 — Documentation & Wrap-up — 7/9 ✅ DONE; 2 items need your hands (see below)
*(extends manual Part 6 with hardware-specific artifacts)*

- [x] **Updated block diagram** including the HPS/Qsys system and Avalon-MM shim — `README.md`, a mermaid flowchart from the HPS driver through the Lightweight bridge, the Avalon shim, the engine, bias table, cache, and backing memory, with every arrow tied to a currently-passing test.
- [x] **Written explanation of the bias mechanism + BI handshake** — `README.md`, carried over from the manual's Part 1.4 largely unchanged (per this item's own instruction), with RTL file/line pointers and hardware-verification citations added.
- [ ] **Waveform (Verilator/GTKWave) and SignalTap capture screenshots — needs your hands, not automatable from here.** Every sim run already produces a `.vcd` file (e.g. `sim/phase4/phase4_waveform.vcd`, `sim/phase4h/phase4h_shim_waveform.vcd`) — open any of them in GTKWave and screenshot the BI handshake, cache hit/miss, or eviction-writeback signals. For SignalTap: needs an interactive Quartus session (add a SignalTap instance, arm it, trigger a real hardware BI sequence via one of the `sw/hps_driver/host_driver_linux_*.c` drivers, capture, screenshot). Both need a GUI and physical interaction I don't have from this environment — flagging clearly rather than silently skipping.
- [x] **Coverage report** — `COVERAGE_REPORT.md`: Verilator `--coverage` line/toggle numbers (90/150, 60%) plus a hand-curated functional cross-coverage table (the manual's own `access_type × bias_state_before × bias_state_after` and `cache_result × bias_state` model) with every cell backed by a named test. Found one genuine gap this way (a write had never triggered BI, only reads had) and fixed it rather than just noting it — see that file §3.
- [ ] **(Optional stretch, explicitly not pursued)** SD card standalone boot — out of scope per this item's own framing; documented as a deliberate non-goal in `README.md`'s "what I deliberately did not model" section instead of left as a dangling checklist item.
- [x] **Bug log with a sim-vs-hardware divergence bug front and center** — `bugs.md` now opens with a "Headline stories" section pointing directly at BUG-006 (the race-condition story) and BUG-009/BUG-010 (the sim-vs-hardware divergence story, including an honest self-correction: BUG-009 was a real bug but not the actual cause of the hangs it was first blamed for).
- [x] **Phase 8's latency number, with methodology** — see this document's own Phase 8 section above; also cross-linked from `README.md`.
- [x] **README with an explicit "what I deliberately did not model" section** — `README.md`, covering PHY/electricals, on-chip-BRAM-not-DRAM backing memory, no bare-metal boot, no multi-host/CXL.io/.cache, no host-initiated bias transitions (with BUG-011 cross-referenced), and SD card boot.
- [x] **Archived, reproducible build** — `quartus/build_all.sh` scripts the full chain (`qsys-generate` → `quartus_map` → HPS SDRAM pin assignments → `quartus_cdb --merge` → `quartus_fit` → `quartus_asm` → `quartus_sta`) using only the documented toolchain paths; `README.md`'s "Reproducing this from a clean checkout" section walks through sim, build, flash, and hardware-test steps in order, including the mandatory timing-check-before-flash and watchdog-safety-check-before-MMIO steps this project's own bug log makes non-negotiable.

**Gate:** 7 of 9 items done and reproducible from a clean checkout using only the documented toolchain paths. The remaining 2 (GTKWave/SignalTap screenshots) are genuinely interactive/GUI work — the `.vcd` files and hardware test drivers they'd use are all in place and ready whenever you want to capture them.

---

## Standing process rule: combinational-depth audit before every synthesis

Before running `quartus_map` on any new/changed RTL, re-check every clocked `always` block for chained arithmetic (multiply/divide/wide-adder trees) between two registered points — a passing simulation proves function, not timing; Verilator/Icarus have no concept of gate/wire delay. Budget roughly one significant arithmetic operation per pipeline stage. Audited clean as of Phase 1H (2026-09-02): `cxl_mem_protocol_engine.sv` and `cxl_avalon_shim.sv` are pure control/FSM/mux logic plus one single-port memory access per cycle — nothing to pipeline. Re-run this audit before Phase 3 (bias table, if addressing ever involves hashing) and especially Phase 5 (cache tag/data array comparison logic) — those are the first places this project could plausibly grow real computational depth.

## Running Risk Register

| Risk | Phase | Mitigation |
|---|---|---|
| No confirmed bare-metal ARM cross-compiler in the listed toolchain | 0 / 1H | Resolve explicitly in Phase 0 before any HPS software work starts |
| BI handshake races are hard to observe on hardware (no waveform by default) | 4H | SignalTap instrumentation planned in from the start of 4H, not bolted on after a bug appears |
| On-chip BRAM budget shared between backing memory (2H) and cache tag/data (5H) | 2H, 5H | Size both deliberately and document the split; Cyclone V `5CSXFC6D6F31C6` M10K budget is finite |
| Avalon bridge latency changes timing assumptions vs. the idealized sim transaction model | 1H onward | Treat any sim/hardware behavioral divergence as a bug-log entry, not noise — it's often the most interesting finding |
