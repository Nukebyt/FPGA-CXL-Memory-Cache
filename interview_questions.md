# Interview Q&A — CXL-Lite Memory-Side Cache (DE10-Standard FPGA port)

Organized per phase of [FPGA_Implementation_Roadmap.md](FPGA_Implementation_Roadmap.md). Pure CXL protocol theory (bias, BI handshake, why not MESI, etc.) is already covered in depth in [CXL_Lite_Memory_Side_Cache_Manual.md](CXL_Lite_Memory_Side_Cache_Manual.md) Part 5 — that bank doesn't change just because the target moved from sim-only to real hardware, so it's referenced rather than duplicated. What's new here is everything the **hardware port itself** forces you to reason about: why a given RTL structure, why a given tooling choice, why a given hardware/software boundary.

Phases not yet implemented have a placeholder so the structure exists — fill them in as each phase lands, same discipline as the roadmap's checkboxes and [bugs.md](bugs.md).

---

## Phase 0 — Toolchain & Environment

**Q: Why can't you just implement real CXL on a DE10-Standard?**
A: CXL is three sub-protocols multiplexed over a PCIe link using alternate protocol negotiation introduced in PCIe 5.0/6.0. Cyclone V's PCIe hard IP only supports Gen1/Gen2 classic PCIe — there's no CXL alt-protocol negotiation capability at the silicon level, full stop. So "implement CXL on this board" can only mean: keep the protocol-level logic (the actual engineering content — bias table, BI sequencer, cache controller) as real synthesizable RTL, and replace the PCIe/CXL PHY with a transport you can actually build, in our case the HPS↔FPGA-fabric boundary.

**Q: Why is the HPS↔FPGA boundary a reasonable stand-in for host↔CXL-device, rather than just a cop-out?**
A: Architecturally it has the same shape: two independent, separately-clocked domains that only interact through a narrow, well-defined bridge (Lightweight HPS-to-FPGA Avalon-MM bridge here, versus a PCIe link in real CXL). The HPS genuinely doesn't share memory or clock domain with the fabric logic by default — you have to explicitly bridge them, the same way a real host has to explicitly negotiate and traverse a link to reach a CXL device. It's not identical (no real link training, no real electrical layer), but the request/response/coherence-handshake semantics you're actually trying to demonstrate sit above that layer anyway.

**Q: Why bare-metal on the HPS instead of embedded Linux?**
A: The HPS is standing in for "the host," and the actual engineering content of this project is the CXL.mem protocol engine and coherence logic in the FPGA fabric — not embedded Linux bring-up. Bare-metal gets a minimal, deterministic driver running against the fabric logic fastest, with the fewest confounding variables (no kernel scheduling jitter when you're trying to measure access latency in Phase 8, for instance). Embedded Linux would be the right call if the goal were demonstrating a realistic OS-level CXL driver stack; it isn't here.

**Q: Why JTAG instead of SD card for development?**
A: Two independent reasons converge: (1) iteration speed — reprogramming over JTAG/USB-Blaster takes seconds, rewriting an SD card each change is much slower; (2) SignalTap (needed from Phase 4H onward to debug BI-handshake races) requires a live JTAG connection anyway, so JTAG is a hard dependency for that phase regardless. SD card boot (HPS configuring the FPGA from an `.rbf` at power-on, no PC attached) is a legitimate deployment target, just not a development one — it's deferred to final packaging.

**Q: Why Verilator/Icarus instead of the Questa install you already have?**
A: Open-source-first was an explicit choice for this build. The tradeoff to be honest about: Verilator doesn't run UVM (no real SystemVerilog class/OOP simulation semantics), so the manual's UVM environment (config_object/agents/scoreboard/coverage_collector) had to be re-expressed as a C++ testbench harness instead — see the mapping table in the roadmap. That's not a strictly equivalent capability (you lose UVM's built-in sequence/factory machinery), but a hand-written C++ scoreboard against an independently-coded reference model achieves the same verification *goal* the manual cares about — see BUG-001 below for why running two different tools (Verilator + Icarus) instead of trusting one turned out to matter in practice, on the very first phase.

---

## Phase 1 — Protocol Engine (sim) / done

**Q: Walk me through the Phase 1 FSM.**
A: Three states — `IDLE` (accept a new M2S request, `m2s_ready` asserted only here), `MEM_ACCESS` (one cycle performing the memory read or write), `RESP` (drive the S2M response and hold it until `s2m_ready`). Deliberately single-request-at-a-time: no tag tracking, no reordering, because that's explicitly Phase 2's job. Building the simplest correct thing first and adding concurrency as a distinct, separately-verified increment is the same "get the simple version right first" discipline the manual applies to bias gating in Phase 3.

**Q: Why valid/ready handshaking instead of a fixed-latency protocol?**
A: It's the natural, reusable building block — every later phase (bias gating, BI sequencer, cache hit/miss) changes how long a request takes to service, sometimes unpredictably (a BI handshake round-trip has no fixed latency). A fixed-latency contract from Phase 1 would have to be renegotiated at every subsequent phase; valid/ready absorbs that variability for free.

**Q: What does S2M `is_data` actually correspond to, and why encode it that way?**
A: It's the NDR-vs-DRS distinction from the real CXL.mem channel list — NDR (No Data Response, e.g. a write completion) vs DRS (Data Response, e.g. read data). Modeling that distinction from Phase 1 onward, even in this reduced pass-through form, means the S2M interface shape doesn't need to change when BIRsp-driven flows and cache hits/misses get added later — they all still resolve to "was there a data payload or not."

**Q: Why does the memory array get an `initial` block instead of relying on power-up X's?**
A: Two independent reasons: it makes the sim testbench's "read an address that was never written, expect 0" check meaningful instead of X-comparing garbage, and it's synthesizable on Intel FPGAs specifically — Quartus supports `initial` blocks on inferred M10K block RAM as the mechanism for specifying initial memory contents, so this isn't a sim-only convenience that has to be ripped out before Phase 1H.

**Q: Tell me about a bug you found, even in the simplest phase.**
A: BUG-001 (see [bugs.md](bugs.md)) — `state_n = m2s_valid ? MEM_ACCESS : IDLE;`, a ternary assigning an enum-typed signal, elaborated fine in Verilator but was rejected by Icarus Verilog ("requires an explicit cast"). The real point isn't the one-line fix (replacing it with explicit `if`/`else`) — it's that **a single simulator's silence is not proof of portability**. Verilator's `-Wall` didn't catch a construct that another compliant tool rejected outright, which is exactly the scenario that would have bitten later at Quartus synthesis time instead, much more expensively. That's the argument for running a second open-source tool as a cheap cross-check even on trivial-looking RTL.

**Q: Why C++ scoreboarding instead of UVM's scoreboard component, concretely, for this module?**
A: The testbench keeps its own `std::map<addr, data>` as the expected-value model, updated only on observed writes, and compares every read against it — independently derived from the stimulus the testbench itself issued, not from reading the DUT's internal memory. That's the same non-derivation principle the manual insists on for the Phase 4 bias-state predictor, just applied one phase earlier because even a "trivial" memory pass-through benefits from not trusting the DUT to grade its own homework.

---

## Phase 1H — HPS/Qsys Hardware Port / prepared, hardware bring-up pending

**Q: Why base the Qsys system on the reference project's `soc_system.qsys` instead of authoring the HPS component from scratch?**
A: The HPS component's DDR3 timing/pin configuration is board-specific, fiddly to get right, and effectively unverifiable without hardware in the loop — there's no simulator that catches a wrong DDR3 strobe timing parameter. The reference project's system is for the *same physical board*, already proven correct (it's what a prior working `.sof` was built from). Reusing it and only swapping the custom peripheral turns a high-risk "author a working HPS config blind" problem into a low-risk "diff against something already known to work" problem — the same reasoning that justifies copying proven reference designs anywhere: verify by minimal diff, not by re-deriving from first principles when a validated baseline exists.

**Q: Walk me through the Avalon-MM shim's register protocol.**
A: Two-phase, software-driven: (1) write the address and (if a write) the data into staging registers (`REQ_ADDR`, `REQ_WDATA`), then write `REQ_FIRE` (packing tag + read/write bit) to actually trigger the M2S request — that write stalls via `avs_waitrequest` if the engine isn't idle, so software can never fire a second request before the first is even accepted; (2) poll `STATUS` until the valid bit is set, read `RESP_TAG`/`RESP_RDATA`, then write `RESP_ACK` to consume the response and return the engine to idle. It's a direct register-poke translation of the same valid/ready handshake the protocol engine already speaks — the shim's whole job is impedance-matching "software pokes registers" to "hardware wants a handshake."

**Q: What's `dbg_heartbeat`/`dbg_seen_request`/`dbg_seen_response` for, and why bother?**
A: A minimal, register-interface-independent proof that the fabric logic is alive at all — a free-running counter bit (heartbeat) and two sticky "have I ever seen X" latches, wired straight to HEX0 segments. The point: if the register interface itself turns out to be broken on first bring-up, these still tell you whether the clock/reset tree reached the shim and whether any request/response ever happened, which narrows the debugging space enormously compared to a design with zero hardware-visible signal until the software path works end to end. Directly copied from the reference project's own bring-up debug pattern (`cfar_avalon_bridge`'s `dbg_*` conduit) — this is a proven, reusable technique, not something invented for this project.

**Q: You found a bug (BUG-002) in the *testbench*, not the RTL. Why does that matter?**
A: It's evidence the verification methodology itself is being taken seriously — catching your own testbench's mistakes is a skill distinct from catching the DUT's. Concretely: the helper checked `avs_waitrequest` *after* calling `tick()` instead of *before*, which is backwards for Avalon-MM's same-cycle sampling semantics — a `waitrequest`/`valid`/`ready` sampling-direction bug is a recurring bug family across every handshake interface in this project (see BUG-002's write-up), and this was the second time in the session a subtle handshake-timing detail bit — the first being BUG-001's enum-ternary portability issue. Pattern-matching your own bug log is itself a skill worth naming explicitly.

**Q: Walk me through how you diagnosed BUG-003 (the `.sof` generation failure) and why you're confident it's not your bug.**
A: `qsys-generate` failed deep in HPS DDR3 sequencer codegen, with only a generic "child process exited abnormally" — not informative on its own. The isolation step was key: reproduce the *identical* failure on an untouched copy of the reference project's `soc_system.qsys`, with none of my changes present. Same failure, same point, same error text → proves it's an environment issue, not something in `cxl_avalon_shim`. From there, reading the actual failing script (`nios2_command_shell.sh`) rather than treating it as a black box found a real, fixable sub-issue (a `wsl`/`dos2unix` presence-check gate that was failing before any real work started) — confirmed by testing the exact `which wsl`/`which dos2unix` checks the script itself runs. That fix was real and verified (got further into the script), even though a second, deeper issue remains. The methodology — isolate against a known-good baseline before assuming your own change is at fault, then read the actual failing tool's source rather than guessing — generalizes well beyond this one bug.

**Q: What's the honest state of Phase 1H right now?**
A: Done, verified on real silicon. `.sof` built with 0 errors and positive timing slack on every corner, programmed via JTAG, and the exact write/read stimulus from the Verilator testbench produces the correct result when issued by the HPS against real FPGA fabric — confirmed reproducible (4/4 identical runs) and extended to a 7-check matrix (multiple address/tag/data combinations plus a never-written-address zero-check). That's the strongest kind of "done" available at this stage of the project.

**Q: You built a bare-metal driver, then had to throw the plan away mid-session and write a different one. What happened, and what does it say about the process?**
A: The plan assumed we'd need to flash an SD card with a preloader and U-Boot from scratch (no existing bootloader anywhere for this design), which is genuinely substantial embedded-systems work. Partway through, it turned out the board already had a *running* Linux system on its SD card, reached over a network the user brought up live over the serial console. The bare-metal driver I'd already built (own startup code, own linker script, raw physical-address execution) assumes exclusive control of the CPU and would corrupt a running kernel if run as-is — so it had to be superseded, not adapted, by a genuinely different artifact: a plain Linux userspace program using `mmap(/dev/mem)`, which needs none of the bare-metal machinery. The bare-metal version wasn't wasted work — it's still the right artifact for a true from-power-on boot path later — but the lesson is to verify the actual target environment (is there already an OS running, or not?) before investing in a boot-chain build, not just follow the originally-planned path on autopilot once new information changes the premise.

**Q: How did you verify the register base address you used was actually correct, rather than just trusting what you'd set in the Qsys script earlier?**
A: Grepped it directly out of the generated `soc_system.sopcinfo`'s `<baseAddress>` element for `cxl_avalon_shim_0.avs` (found `24576`, confirmed `= 0x6000` by direct calculation) rather than assuming the qsys-script's `set_connection_parameter_value ... baseAddress {0x6000}` call necessarily took effect exactly as written. This is directly the fpga_checklist.md's own explicit rule: "confirm the offset from `soc_system.sopcinfo`'s `<baseAddress>`, not from memory" — a rule earned from a real prior bug in a related project, applied here before it could cause one in this one.

**Q: Why compile the test driver on the board itself instead of cross-compiling?**
A: The board's toolchain (Linaro GCC 4.9.3, old — required `-std=gnu99` explicitly since C89 is its default) doesn't necessarily match the ABI/libc assumptions of a cross-compiler built for a different target triple or libc version. Compiling natively sidesteps that whole class of mismatch for what's a tiny, dependency-free single-file program — the cost (needing gcc on-target, which happened to already be there) was much lower than the risk a cross-compiled binary silently misbehaves from an ABI mismatch neither side would surface as an error.

---

---

## Phase 2 / 2H — Outstanding-Transaction Tracker

**Q: How does the tag-indexed tracker work, structurally?**
A: One slot per possible tag value (`2**TAG_W` slots), not a separate allocated table — occupancy is just "is this tag currently in flight." `m2s_ready` is decoded combinationally from the incoming tag (`!slot_valid[m2s_tag]`), so firing an already-outstanding tag simply stalls until it frees — the same discipline a real ID-based interface (AXI4, this project's own prior work) enforces. Directly reusing that pattern was a deliberate choice, not a coincidence — same underlying problem (route N independent, unordered completions back to the right requester) that AXI4 ID-routing already solves.

**Q: How did you make out-of-order completion actually happen, rather than just being "possible in theory"?**
A: Response-side latency asymmetry: writes (NDR, no data payload) become eligible to respond the instant they're accepted; reads (DRS) carry a fixed extra latency before their data is ready. So a write issued *after* a read can and does complete first — genuine out-of-order completion driven by request type, not a random/artificial delay bolted on. The verification proved this rigorously: the write's tag was deliberately chosen *numerically higher* than the read's, so the arbiter's own lowest-tag-first priority couldn't be coincidentally responsible for the ordering observed — only the latency difference explains it.

**Q: What's the hardest bug you found in this phase, and what does it teach about testbench design?**
A: Not an RTL bug — a testbench one (BUG-005). The testbench held the response-consumer signal (`s2m_ready`) high for the *entire* test, on the assumption that "always ready to accept" is a harmless default. It isn't: since the engine's arbiter runs continuously and immediately consumes any response the moment it's offered, a response that became eligible while a *later* stimulus-firing call's internal wait-loop was still running got silently consumed before the test's own draining loop ever started watching — one of five expected responses vanished with no error, just a hung wait. The fix was gating `s2m_ready` explicitly per test phase (low while firing, high only while draining). The lesson: an "always ready" consumer signal isn't neutral — it actively changes *when* things happen relative to when you're watching, and this bites any testbench driving a DUT with internal arbitration or queueing.

**Q: Also found a bug before ever running the sim at all — what was that?**
A: BUG-004 — an early draft drove the same per-tag state arrays (`slot_valid`, `slot_cnt`) from three separate `always_ff` blocks (one for accepting, one for the countdown, one for clearing on consume). Two or more procedural blocks driving the same signal is illegal for synthesis and undefined even in simulation. Caught by re-reading the design before building it, not by a tool — worth calling out because it shows the review discipline catching something before it could even manifest as a confusing failure.

---

## Phase 3 / 3H — Bias Table + Simple Gating

**Q: Phase 3 barely changes any observable behavior — why build it at all at this point?**
A: Because there's genuinely nothing *to* gate yet — no cache exists until Phase 5, so "Device-Biased proceeds normally, Host-Biased bypasses the cache" is satisfied by construction (everything already goes straight to backing memory regardless of bias). Building the bias table now, get it *correct and wired in* before there's a consequence riding on it, mirrors exactly the same "get the simple version right first" discipline the manual applies elsewhere — and it pays off immediately: Phase 4's BI sequencer needed a working, addressable bias table to gate against, and having it already built and verified meant Phase 4 could focus entirely on the handshake logic itself.

**Q: Why is the bias table its own module rather than folded into the protocol engine?**
A: Matches the manual's own architecture diagram — Protocol Engine → Bias Table → Cache Controller as distinct stages. That's not just documentation fidelity: it mattered concretely once Phase 4 needed the BI sequencer to sit *between* the engine's accept logic and the bias table's write port, arbitrating between a debug write source and the BI sequencer's own atomic flip. Keeping bias table as an independent module with a clean multi-port interface (two independent reads, one arbitrated write) made that arbitration a small, local, obviously-correct piece of logic instead of something tangled through a monolithic module.

**Q: What's the register interface for querying bias state from software, and why that shape?**
A: `BIAS_ADDR` (write, latches an address) then `BIAS_SET`/`BIAS_GET` (write/read a single bit at that latched address) — the same "latch-then-trigger" pattern already established by `REQ_ADDR`→`REQ_FIRE`. Reusing an existing pattern rather than inventing a new register convention for every new feature keeps the whole interface predictable to work with as it grows (it did keep growing — Phase 4H added four more registers on top).

---

## Phase 4 / 4H — BI Sequencer

**Q: Walk through the actual FSM.**
A: Three states — `BI_IDLE`, `BI_REQ`, `BI_WAIT_RSP`. From `BI_IDLE`, an incoming request whose address reads Host-Biased (checked via the bias table's dedicated query read port) triggers a move to `BI_REQ`, where `bi_req_valid` asserts with the live request address. Once the host accepts (`bi_req_ready`), move to `BI_WAIT_RSP` and wait for `bi_rsp_valid`. The moment BIRsp arrives, the bias table's write port — arbitrated between this flip and a concurrent debug write, flip taking priority — commits Device-Biased *atomically*, on that exact cycle, and the FSM returns to `BI_IDLE`. The held M2S request (never dropped — the host holds `m2s_valid`/`m2s_addr` stable throughout, standard valid/ready discipline) then proceeds through the completely unmodified Phase 2 accept path.

**Q: Why does the BI sequencer not need its own address latch for the line it's resolving?**
A: Because it doesn't need to remember it — it reads `m2s_addr` live, continuously, throughout the handshake. That only works because the *host* is contractually required to hold `m2s_valid`/`m2s_addr` stable while `m2s_ready` is low (ordinary valid/ready backpressure semantics), so the address is guaranteed still there, unchanged, for as long as resolution takes. One fewer register, and it's correct specifically because it leans on a protocol guarantee that already has to hold for unrelated reasons — not a coincidence, a deliberate simplification once that guarantee was recognized.

**Q: The BI sequencer stalls the *entire* engine, not just the line being resolved — isn't that a big limitation?**
A: Yes, and it's a deliberate, explicitly-documented scoping choice, not an oversight. A per-address block-list (only stall requests to the *specific* line under BI resolution, let unrelated addresses keep making progress) is more realistic and higher-throughput, but adds real complexity for a race class — a second request touching the *same* line an in-flight BI is resolving — that a global stall rules out by construction. That directly satisfies the manual's own test requirement ("verify correct blocking/ordering, not a race") trivially and safely. Good answer if pushed further: "if I had more time, this is exactly the kind of thing I'd revisit — the manual's own 'what would you add with unlimited time' question."

**Q: What's the best bug story in this whole project?**
A: BUG-006, found while writing the Phase 4H register-interface testbench, not the RTL. The Avalon shim wired `m2s_valid = write_fire && m2s_ready` — correct back when `m2s_ready` depended only on per-tag occupancy, since gating valid on an already-known-combinational-ready was harmless there. It silently became a **circular deadlock** the moment `m2s_ready` grew a bias/BI dependency: for a Host-Biased address, `m2s_valid` can never assert (needs `m2s_ready`, which needs the BI handshake to have already resolved) while the BI handshake needed to resolve it can itself never start (needs `m2s_valid` to trigger). Worse, even setting the circularity aside, the original design would have held the *entire* Avalon bus hostage via `avs_waitrequest` for as long as BI resolution took — and BI resolution is *itself* driven by software issuing further Avalon transactions to service it. Software would have had no way to ever reach the very registers that unblock its own blocked bus. This is a genuine coherence/liveness bug in the *interface design*, invisible to the Phase 4 RTL-level testbench because that testbench drove the engine's `m2s_valid` directly as a true SystemVerilog signal, sidestepping the shim's flawed wiring entirely — it only surfaced once the same interface real software would use was exercised end to end. Fixed with a one-deep pending-request holding register that decouples the Avalon bus transaction from however long the engine actually takes to accept it.

**Q: What does BUG-006 teach that's bigger than this one bug?**
A: That a wiring pattern correct under one set of assumptions can become a protocol deadlock the moment a *later* phase adds a dependency that quietly violates those assumptions — and that a bug like this can hide successfully through an entire phase's testbench if that testbench doesn't exercise the same intermediary layer real usage will actually go through. The manual predicted Phase 4 would be the richest source of trouble once bias/BI logic exists (§4.6) — this is exactly that prediction landing, just one layer up from where it was expected (the shim, not the sequencer FSM itself).

**Q: What's missing relative to the manual's original Phase 4 verification plan?**
A: No independent bias-state predictor class, no SVA assertions, no constrained-random testing, no formal functional coverage — this project's Verilator/C++ track uses directed tests with explicit reference checks instead. The manual's key invariant ("never access a Host-Biased line without a completed BI") is enforced *structurally* here — `m2s_ready` is gated on bias resolution, so an unresolved access literally cannot be accepted — rather than watched for continuously via a runtime assertion. Worth being upfront about that gap rather than overclaiming coverage-closure that doesn't exist.

---

## Phase 5 / 5H — Cache Tag/Data Array

**Q: How did you verify the cache on real hardware, and why does that matter more than just checking data values?**
A: The hardware driver (`host_driver_linux_phase56h.c`) asserts the exact `CACHE_HITS`/`CACHE_MISSES` counter deltas at every single step, not just whether the returned data was correct. That distinction matters: a cache whose hit-detection logic is broken but whose backing memory is always correctly written would still return the right *data* on every access — it would just be silently missing every time instead of caching. A data-only test can't distinguish "cache working" from "cache doing nothing, backing memory saving you." Checking `hits+1 misses+0` on a resident-line read is what actually proves the cache's control logic, not just its data path.

**Q: Did the cache have the same reset problem as the bias table (BUG-009)?**
A: I checked rather than assumed — `cache_valid[]` has the identical `initial`-block-only reset pattern that caused BUG-009. But the hardware test's very first line, `counters at entry: hits=0 misses=0`, followed by the first write correctly registering as a miss (not a spurious hit against garbage tag-compare data), confirms it powers up correctly on this device. Given the BUG-009/BUG-010 experience — I'd already been burned once by assuming a bug from a superficially similar pattern without confirming it was actually the cause — the right move here was to gather the evidence the hardware test already produces for free, not to preemptively "fix" something that isn't demonstrated to be broken.

---

## Phase 6 / 6H — Dirty-Line Writeback + Eviction

**Q: What's the strongest test in the whole project, and why?**
A: The Phase 6/6H thrash sequence: four addresses that all alias the same cache line, written in order, then read back in *reverse* order. By the time the first-written line is finally read back, it has already been evicted once (when the second write landed), and every other line in the chain has *also* been evicted at least once by the time its own readback happens. Every value comes back correct, on both sim and real hardware. That's a much stronger claim than "the cache works" — it's "the eviction-writeback path is stable under repeated forced reuse of a single line," which is exactly the condition a real workload with a hot, undersized working set would create.

---

## Phase 7 — Hardware Stress & Edge Cases
*(not yet built.)*

---

## Phase 8 — Cache-On vs Cache-Off Latency Experiment

**Q: Walk me through the cache-on-vs-off latency experiment and what you found.**
A: Built a `CACHE_BYPASS` mode (new RTL, `cache_bypass_en`) that forces every access to behave exactly as it did pre-cache — reads always pay `READ_EXTRA_LATENCY`, writes go straight to backing memory — while leaving the cache arrays themselves untouched, so toggling it doesn't disturb what's cached. Then measured 2000 repeated reads of one resident line with the cache on vs. with bypass on, timing each access with `clock_gettime(CLOCK_MONOTONIC)`. Result: `cache_hits`/`cache_misses` counters confirmed the RTL behaved exactly as expected (2000/0 vs 0/2000), but the measured wall-clock latency difference (-2.4ns) was smaller than the measurement noise (~1035ns stddev) — not a statistically meaningful signal.

**Q: Why didn't the experiment show a speedup, and is that a problem?**
A: No — it's an honest, explainable result, not a failure. The cache's real RTL-level saving is `READ_EXTRA_LATENCY=3` cycles at 50MHz, which is 60 nanoseconds. Every access in this experiment also pays a full software-driven round trip — mmap'd `/dev/mem`, the Lightweight HPS-to-FPGA bridge, a polling loop — which costs roughly 3.2 *microseconds* per access, identically whether the access hits or misses. A 60ns effect simply can't be resolved above ~3200ns of fixed overhead that doesn't distinguish hit from miss. The cache's correctness and its latency benefit are both independently verified elsewhere (Phase 5-7's functional tests; the `READ_EXTRA_LATENCY` parameter's direct, inspectable effect on `slot_cnt` timing in the sim waveform) — what this specific experiment shows is a *measurement resolution* limit, not a design flaw.

**Q: What would you have to change to actually observe the difference?**
A: Either measure closer to the hardware (SignalTap on the FPGA side, timing the RTL directly rather than through a software round trip — the register-level path exists for correctness testing, not performance measurement), or change what's being compared — e.g. a workload where a miss costs an *additional full round trip* (a real cache miss to DRAM/a remote tier, not just 3 extra clock cycles on an already-fast on-chip array) would produce a difference large enough for even microsecond-scale software timing to see. The honest scoping point: this project's on-chip BRAM backing memory means even a "miss" is fast in absolute terms — a real memory-side cache's value proposition is largest exactly when backing memory is slow (DRAM, a remote CXL device), which this hardware port deliberately doesn't model (see the project's own scope-boundary notes on backing memory).

**Q: Why report a null result instead of reframing the experiment until you got a positive number?**
A: Because the branch logic that decided which conclusion to print was correct and used real values throughout (`fabs(diff) < pooled_std`) — the honest path was already the more interesting one to explain, and manufacturing a percentage the measurement doesn't support would be a worse answer to give in an interview than "here's what I measured, here's why it came out this way, and here's what I'd change to measure the effect I expect is really there." That's also directly in keeping with this project's own established discipline of logging real findings as they're found rather than smoothing them over (see bugs.md) — a null result, correctly explained, is a finding.

---

## Hardware debugging — the BUG-010 story (strongest debug narrative in the project)

*This is the one to tell when asked "tell me about a hard bug you debugged." It's a better story than any of the RTL bugs because the interesting part is the reasoning failure, not the fix.*

**Q: Walk me through the hardest bug you hit on hardware.**
A: Symptom: running the HPS test program made the entire board unreachable — not the process hanging, the whole system. `SIGKILL` couldn't touch it, `timeout(1)` couldn't touch it, and recovery needed a physical power cycle. It took four power cycles to find, and the actual cause had nothing to do with my RTL: **the FPGA fabric was unconfigured, but the Lightweight HPS-to-FPGA bridge was enabled anyway.** JTAG configuration is SRAM-based and volatile, so every power cycle wiped the bitstream, and this SD image's u-boot doesn't load an `.rbf`. So the CPU was issuing Avalon transactions to a bridge with nothing behind it — the transaction never completes, and an ARM load/store to an unresponsive bridge stalls the core *uninterruptibly*. Software timeouts are useless there by construction: execution never returns to software to check them.

**Q: Why did it take four power cycles?**
A: Three things actively pointed the wrong way, and I want to be honest that I followed them too far:
1. **The board still answered pings and SSH while "hung."** Cyclone V is a *dual-core* A9 — one core was stalled on the bus, the other kept Linux and networking running. So "the board is reachable" looked like evidence the platform was fine and the bug was in my logic. It wasn't evidence of anything.
2. **The stall survives JTAG reprogramming.** Once a core is wedged on the bridge, reprogramming the fabric doesn't retire the stuck transaction — only a reboot does. So my fix→reflash→retest loop kept reproducing the hang *even after the fabric was correctly programmed*, which made a perfectly good fix look like it had failed.
3. **`/sys/class/fpga_manager/*/state` reads `power off` even after a successful JTAG configuration** on this board/kernel. The one status field that should have answered the question directly was misleading.

**Q: What did you actually get wrong in your reasoning?**
A: I diagnosed a real bug and then over-attributed. I found that the bias table had no synthesizable reset — only a Verilog `initial` block, which Verilator honors but silicon doesn't — filed it as the root cause, fixed it, and rebuilt. That bug was real and worth fixing, but it was *latent*; it wasn't causing the hangs. The tell I ignored: after the fix, the symptom didn't change at all. I'd also ruled out timing closure by reading the STA report, which was good practice, but ruling out one hypothesis isn't the same as confirming the next one. **The correction: when the symptom is "the whole system wedges," verify the platform is in the state you assume before hunting for logic bugs inside it.** I never asked "is the fabric even configured?" because programming it had always previously happened in the same session as testing, so the precondition was satisfied incidentally and I'd stopped seeing it as a precondition at all.

**Q: How did you finally find it?**
A: Stopped guessing and read platform state instead of RTL: `/sys/class/fpga_manager/*/state` → `power off`, `/sys/class/fpga_bridge/*/state` → `enabled`. Those two lines together are the whole bug. Then I confirmed it safely rather than by another blind test — armed `/dev/watchdog` first, did a *single* register read, and let the watchdog reset the board if it stalled. It didn't stall; it returned valid data, which proved the fabric was live and the theory was right.

**Q: What did you change so it can't happen again?**
A: Turned it into procedure rather than knowledge: (1) `quartus_pgm` after every power cycle, before anything touches the bridge; (2) program only from a cleanly-booted HPS, since reprogramming won't clear an existing stall; (3) **every HPS test driver now arms the hardware watchdog before its first MMIO access.** That third one is the highest-value change — it converts "wedge the board, get up, power cycle it" into "board resets itself in 20 seconds," which is the difference between hardware debugging being expensive and being cheap. I should have added it after the *first* hang, not the fourth.

**Q: Generalize it — what's the transferable lesson?**
A: An unconfigured or absent slave behind an *enabled* bus bridge doesn't fail fast, it hangs forever. Bus fabrics generally have no timeout unless someone deliberately designed one in, so "no responder" and "slow responder" are indistinguishable to the master — it just waits. That's why real SoC interconnects ship with bus-timeout/error-response units, and it's a good argument for adding a timeout with an error status bit to my own shim as a hardening step. Second lesson: on a multi-core system, liveness signals like ping can come from a core that isn't the one that's stuck, so "it responds" is much weaker evidence than it feels like.

---

## Cross-cutting / "why this project, why hardware" questions

**Q: The manual originally scoped this as simulation-only. Why go to the trouble of real hardware?**
A: Two reasons worth stating plainly in an interview: (1) it forces the RTL to actually be synthesizable and timing-closeable, not just simulate-clean — a meaningfully higher bar than a UVM testbench alone verifies; (2) it exposes an entire class of bugs (bridge latency, real clock domains, tool-portability issues like BUG-001) that a purely behavioral sim can hide. Being able to say "I found a bug that only showed up once this left the simulator" is a materially stronger signal than "I simulated it and it passed."

**Q: What's the honest limitation of this hardware port, scope-wise?**
A: Same discipline the manual already applies to the sim-only version, extended: no real CXL PHY/PCIe electricals (impossible on this silicon); backing memory is on-chip BRAM rather than a real DRAM tier (a deliberate simplicity choice, not a limitation of the board — and directly why Phase 8's latency experiment couldn't resolve a difference, see that section); Linux userspace drivers (`mmap(/dev/mem)`) rather than a bare-metal or production kernel-driver stack — the board turned out to already be running Linux from SD card, so that became the natural interface rather than a from-power-on bare-metal application (a bare-metal skeleton exists in `sw/hps_driver/` from before this was known, kept but unused); no host-initiated bias transitions as a real protocol path (see BUG-011); no SD-card standalone boot. Say this proactively — it's scope discipline, not a gap, exactly as the manual argues for its own boundaries. Full list in `README.md`'s "what I deliberately did not model" section.

---

## Coverage discipline — finding and closing a real gap, not just reporting it

**Q: Walk me through how you approached test coverage for this project.**
A: Two deliberately separate signals, not conflated: automated Verilator line/toggle coverage (`--coverage`, merged across all 7 engine-level testbenches — 90/150, 60%), and a hand-curated functional cross-coverage table mapping the manual's own suggested model (`access_type × bias_state_before × bias_state_after`, `cache_result × bias_state`) to the specific test that exercises each cell. The automated number is a blunt instrument on its own — it mixes structural bookkeeping (port declarations, array widths) with real control-flow branches and scores them equally. The hand-curated table is the one I actually trust, because every cell is backed by a named test, not a percentage.

**Q: Did the coverage work actually find anything?**
A: Yes — a real, concrete gap: every single BI-triggering test in the entire project used a *read* to trigger the handshake. A *write* to a Host-Biased line had never been tested, even though the RTL's own trigger condition (`want_bi = (bi_state==BI_IDLE) && m2s_valid && bias_query_rd_data`) doesn't reference `m2s_is_write` at all — so by inspection a write should behave identically. I didn't stop at "should, by inspection" — this project's own bug log (BUG-009/BUG-010) is a standing reminder that assumption isn't verification. I added a real test (`sim/phase4` Test 3: mark a fresh address Host-Biased, fire a *write*, confirm the NDR response, the bias flip, and — the check that actually matters — that a subsequent read confirms the write's data landed correctly, not just that the handshake completed). It passed on the first try. Full regression re-run clean afterward.

**Q: Why does that story matter more than just "we got a coverage number"?**
A: Because the discipline of building a cross-coverage table isn't really about the percentage — it's about being forced to enumerate every *combination* your design claims to handle, not just every *line*. Line coverage would never have surfaced "reads and writes are tested separately, but has a write ever specifically triggered BI?" — that's a question only a cross-coverage table asks. And finding a gap is only half the job; the other half is closing it rather than just writing it down, which is what actually happened here (see `COVERAGE_REPORT.md` §3).

---
