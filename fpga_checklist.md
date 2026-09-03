# FPGA Design & Bring-up Checklist — DE10-Standard / Cyclone V / Quartus Prime Lite 21.1

A distilled, reusable rulebook built from the complete bug history of a real
streaming-DSP project on this exact board (35 documented bugs, from RTL
authoring through synthesis, place & route, SD-card flashing, and HPS/Linux
integration).

**Purpose:** paste or reference this at the start of a new FPGA project session
so the same class of bug is caught by a checklist instead of rediscovered by a
multi-hour debugging session. Every rule below cost real time to learn; none of
it is generic advice copied from a textbook.

**Target environment this was derived from:**

| Item | Value |
|---|---|
| Board | Terasic DE10-Standard |
| Device | Cyclone V `5CSXFC6D6F31C6` |
| Capacity | 41,910 ALMs · 112 DSP blocks · M10K block RAM |
| Toolchain | Quartus Prime Lite **21.1** |
| Simulator | Icarus Verilog (`iverilog -g2005`) |
| SoC | HPS (dual Cortex-A9) + DDR3, Terasic GHRD base |
| HPS Linux | Angstrom v2014.12 — Python **2.7 only**, no pip, ~51 MB free, dead opkg feeds |
| HPS↔FPGA | Lightweight bridge @ `0xFF200000`, span `0x200000` |

Rules marked **[Cyclone V / Q21.1]** are toolchain-specific. Rules marked
**[Universal]** apply to any FPGA work.

---

## 0. The five rules that would have prevented the most damage

If you read nothing else:

1. **A passing simulation does not mean working hardware.** Simulation has no
   concept of gate delay, wire delay, temperature, or bus timing. Run
   `quartus_sta` and read the actual slack number before you flash anything.
   **Add it to the build the day you synthesise your first module** — in the
   source project its absence hid a three-clock-period critical path for the
   entire life of the project while every other signal said "correct."
   A design that violates timing can still produce plausible output when its
   data source is slower than its clock; that is the most dangerous failure
   mode there is, because it looks like success.
2. **Never pair a saved bitstream with newer host software.** A bitstream is a
   versioned build artifact independent of your source tree. An Avalon write to
   a register that doesn't exist in older silicon is *legal and silent* — no
   error, just wrong results.
3. **Identical input must produce identical output.** Make this your first
   hardware test, always. Non-reproducibility means state is leaking between
   runs, and every measurement taken in that state is void.
4. **Test the timing dimension your testbenches don't vary.** Almost every
   testbench feeds data one item per cycle. Real sources pause. That single
   untested dimension hid the worst bug in the project.
5. **A hypothesis that explains the symptom is not a root cause until tested.**
   Prefer the cheap experiment that *discriminates between* hypotheses over the
   plausible story that accommodates all of them.

---

## 1. RTL coding rules

### 1.1 Memory inference **[Cyclone V / Q21.1]**

- **Never write an `always` block that should infer RAM inline inside a
  `generate for` body.** Quartus Prime Lite 21.1 will not recognize it and
  synthesizes raw flip-flops instead. Real measured cost in this project:
  **195,947 logic elements / 1,472 memory bits** instead of
  **4,272 LEs / 116,160 memory bits** — a ~46x logic blowup.
  **Fix:** extract the memory into a standalone `module`, instantiate it once
  per tap inside the `generate for`. Same logic, same code, different syntactic
  placement — and it infers correctly.
- This is *not* a general Verilog rule and is not documented by Intel. It is a
  specific limitation of this inference pass.
- **Red flag to watch for:** a resource report showing near-zero memory bits for
  something that is obviously memory. Check syntactic placement *before*
  assuming the logic is wrong.
- Verify inference with a tiny isolated synthesis probe project, not by reading
  the full design's report.

### 1.2 Same-cycle read-after-write **[Universal]**

- If an `always` block writes `mem[addr] <= x` with a non-blocking assignment
  and another expression in the *same* block reads `mem[addr]`, the read gets
  **stale data** — the write isn't visible until the next edge.
- **Fix:** add an explicit bypass for the element known to be current-cycle
  data; read the input signal directly rather than through the memory.
- **Detection trap:** on smooth synthetic stimulus this produces a suspiciously
  *clean constant* error (in this project, exactly `2×SLI`). A clean constant
  offset is a symptom to investigate, not evidence of a simple fixable
  off-by-one.

### 1.3 Port widths **[Universal]**

- Declare flattened bus ports with the full computed width
  (`output reg signed [SLI*SLI*DATA_WIDTH-1:0] bus`), never the element width.
- A too-narrow port declaration is **zero-padded silently** by the simulator.
  It compiles, it runs, and it produces plausible wrong values.
- Cross-check every port declaration against the widest expression that
  indexes it inside the module.

### 1.4 Fixed-point discipline **[Universal]**

- **Every fixed-point value carries a scale. Write it in the port comment.**
  (`// Q2.13`, `// Q4.12`.) Two values from different sources meeting at one
  arithmetic operator is the single most common silent numeric bug.
- Before any `a + b` / `a > b` between values from different modules or
  different generator scripts, verify their fractional bit counts match, and
  insert an explicit rescale if not. "Just add them" reads correctly in English
  and is wrong in hardware.
- **Reciprocal-multiply precision:** `INV_N = (1<<M)/N` truncates. At `M=16`,
  `N=40` this produced the wrong rounding direction on **~84% of inputs**, all
  biased the same way. `M=24` reduced it to ~2.5%, confined to exact `.5` ties.
  **Sweep `M` against exact-fraction arithmetic in Python before picking it** —
  don't guess.
- When you change a precision constant in RTL, **change it in the golden
  reference model in the same commit.** Otherwise the RTL becomes *more*
  accurate than the thing verifying it, and you get an artificial mismatch.

### 1.5 Shared modules with differently-timed call sites **[Universal]**

- A parameterized utility module (delay line, FIFO, counter) instantiated in two
  places often needs *semantically different* enable logic at each site.
  Example from this project: one `pipe_delay` instance must advance on every
  non-stalled clock cycle; another must advance only on *accepted data* cycles.
  A shared `we = !stall` was correct for one and wrong for the other.
- **Rule:** if two instantiations of the same module have different notions of
  "when time advances," parameterize the enable condition — don't share it.
- This bug is invisible under gap-free streaming and only appears under real
  bus timing.

### 1.6 Handshake and backpressure **[Universal]**

- An extra register stage on a `ready`/`valid` handshake signal is **invisible
  under back-to-back never-stalled traffic** and only diverges once real
  backpressure exercises the boundary cycle.
- Present handshake signals combinationally unless you have a specific timing
  reason not to — and if you register one, re-verify under stall.
- **Best test:** a differential testbench running a known-correct direct-drive
  instance alongside the wrapped instance, comparing cycle by cycle. This caught
  the bug in one run where result-only testbenches passed.

### 1.7 Unstated timing preconditions **[Universal]**

- **The highest-value question in this whole document:** *what does real
  hardware do that none of my testbenches do?*
- A module can carry an unstated precondition about *input timing* that every
  test satisfies by construction. Every testbench in this project fed one pixel
  per cycle — so all of them silently encoded "the source is infinitely fast."
  Real hardware pauses. That was the root cause of a bring-up failure that
  produced **zero output** and survived a comprehensively passing test suite.
- **Rule:** for every streaming module, write at least one testbench with
  randomized gaps and randomized stalls. Parameterize the gap so it can be
  swept 0..N.

### 1.8 Frame/transaction boundaries and sticky state **[Universal]**

- Any "warm-up complete" / "initialized" flag that is **sticky** across a
  transaction will contaminate the *next* transaction.
- Provide an explicit **soft-reset register** that re-arms per-frame state, and
  require software to pulse it before every frame.
- RAM *contents* need not (and cannot) be cleared — re-running warm-up is what
  handles stale contents, exactly as at power-on. Reset the *control* state.
- Reset the result-latch registers on the soft reset too, or a stale result from
  the previous frame is readable as if it belonged to the new one.
- **Beware:** a fix that removes one failure mode can install another at a
  different scope. Fixing correctness *within* a frame moved the defect to the
  boundary *between* frames. Ask, of any "hold state instead of clearing it"
  change: *what used to depend on that clearing?*

### 1.9 Constructs that simulate but don't synthesize **[Universal]**

- **Verilog hierarchical references** (`top.sub.internal_signal`) work fine in
  Icarus and are unambiguous Verilog — and Quartus Prime Lite 21.1 **rejects
  them outright** ("can't resolve reference to object").
  **Fix:** expose the signal as a real port on the child module.
- `break` inside a loop is SystemVerilog; Icarus in `-g2005` mode rejects it.
  Use a loop condition instead — also more portable.
- **Rule:** simulator acceptance and synthesizer acceptance are independent.
  Never assume they agree. Build a 4-minute isolated `quartus_map` probe before
  committing to a 90-minute full build.

### 1.10 One clock cycle is a budget, not a free pass **[Universal]**

This one cost more than any other single defect in the source project — a
**three-clock-period critical path that survived from a module's first
synthesis through months of work**, because every functional test passed.

- **A clocked `always` block with chained blocking assignments describes
  combinational logic between two registers, however many lines it is.**
  Writing eight arithmetic operations as eight sequential statements does not
  give them eight cycles — it gives them one, and builds the whole chain in
  logic. The offending module did: two 64-bit subtracts → 64×24 multiply →
  rounding divide → **64×64 multiply** → divide → multiply-subtract → 64-bit
  multiply → divide, all between two clock edges. Measured: **54.352 ns
  against a 20 ns period.**
- **Budget roughly one significant arithmetic operation per pipeline stage**
  for multiplies, divides, and wide adder trees at 50 MHz on this device.
- **Never declare intermediates wider than their actual range.** Every
  intermediate above was `reg signed [63:0]` "to be safe," which forced the
  one variable×variable multiply to synthesise at full 64×64. Narrowing the
  squared term to its real 32-bit range (justified: it feeds a 32-bit output,
  so anything wider was already broken) was most of the win. Multiplying by a
  **compile-time constant** is far cheaper than a general multiply — know
  which of yours are which.
- **Watch for stacked reduction trees.** A second violation in the same
  project summed *every* stored row accumulator in one combinational sweep on
  the final cycle of a bootstrap — an `SLI-1`-deep adder tree stacked on top
  of an `SLI`-deep tree and `SLI` multipliers, all reached through an 18:1
  mux. **Fix: accumulate incrementally** as each element completes, so the
  last cycle does one add. Bit-exact, because integer addition is
  associative.
- **Pipelining is value-preserving if you only insert registers.** Keep the
  operations and their widths identical, carry a `valid` shift register
  alongside, and re-verify bit-exactness — then the only thing that changed
  is latency. Export the latency as a `localparam` and mirror it in any
  parallel delay line (and in every testbench latency ledger).
- **Result in the source project:** −34.968 ns → −3.517 ns slack from
  pipelining one module, with resources essentially unchanged (52% ALM, 71%
  DSP), and 100.0000% bit-exactness preserved.

### 1.11 General style that paid off **[Universal]**

- Derive every dependent constant with a `localparam` expression rather than a
  hand-typed number, so one parameter change propagates.
- **Exception:** when a constant needs true round-to-nearest (Verilog integer
  division truncates), precompute it offline with a script and hardcode it —
  *with a comment naming the script that generates it.*
- Keep a single source of truth for shared constants and enumerate every place
  they are duplicated. This project had the same four constants copied across
  **9+ files** (RTL, Qsys `.tcl`, `.qsys`, generated `.v`, Python config, MATLAB
  generator, three test generators). Every geometry change needed a lockstep
  edit of all of them. If you cannot avoid the duplication, write the list of
  locations into a comment at the primary definition site.

---

## 2. Quartus / toolchain gotchas **[Cyclone V / Q21.1]**

### 2.1 The optimizer crash

- **Signature:** `Internal Error … RTL_ADDER::aggressive_adder_balancing`,
  `opt_op_add.cpp` line 10225, sub-system OPT.
- **Trigger:** large combined arithmetic (wide adder trees + multiply-accumulate)
  once flattened together.
- **It is NOT monotonic in your design parameters.** A *smaller* configuration
  can crash where a larger one doesn't. Never extrapolate "this is smaller so it
  will fit" — that assumption cost 7 failed builds in this project.
- **It ignores per-instance scoping.** `set_instance_assignment … -to
  <instance_path>` for `OPTIMIZATION_TECHNIQUE` or `AUTO_DSP_RECOGNITION` has
  **no effect** on this crash, even with a verified-correct hierarchy path — the
  pass operates on the fully-flattened whole-design netlist. Only the global
  default changes its behavior.
- **Both known global workarounds carry severe costs:**
  - `OPTIMIZATION_TECHNIQUE AREA` (global) — avoided the crash and fit the ALM
    budget, but destroyed timing **project-wide**, including HPS/DDR3 paths
    unrelated to the offending logic: **−15.480 ns setup slack, −45,308 ns TNS**.
  - `AUTO_DSP_RECOGNITION OFF` (global) — avoided the crash, but forced
    multiplies into logic everywhere in the SoC: **73,449 logic cells vs 44,961**
    baseline.
- **Conclusion:** treat any change to arithmetic width or window size as
  requiring an *empirical* synthesis check. Budget for it. Do not extrapolate.

### 2.2 Timing closure is a separate, mandatory step

- `quartus_fit` reporting success and a resource count under budget means
  **"compiles and fits"** — a completely different claim from **"works
  reliably in silicon."**
- **Always run `quartus_sta` and read `output_files/*.sta.summary`.** Check the
  worst corner, typically `Slow 1100mV 85C Model Setup`.
- **Negative slack manifests as thermal-looking degradation**: results get worse
  as the chip warms under sustained use, and identical runs disagree. If you see
  run-to-run degradation, check slack *and* check reproducibility (§4.1) —
  both produce this signature and they are easy to confuse.
- Any optimization-technique change known to trade timing for area **requires**
  a fresh timing run. This was the actual process gap that flashed a bad build.

### 2.3 Platform Designer / Qsys components

- Pin the correct package version in `_hw.tcl`
  (`package require -exact qsys <version>`). Wrong version → the component is
  **silently invisible** to `add_instance`, with no error.
- `TOP_LEVEL_HDL_FILE` is deprecated and **silently aborts elaboration**. Use
  the `add_fileset` / `set_fileset_property … TOP_LEVEL` pattern.
- **Bugs mask each other.** The version-pin bug had to be fixed before
  elaboration would even *attempt* to run and surface the next one. A diagnostic
  that stops at the first error is not the same as having found all the errors.
- **`qsys-generate` can regress generated files.** In this project a full
  regeneration dropped required entries from `soc_system.qip`, breaking the DDR3
  sequencer build. The patch must be **re-applied after every regeneration** —
  document it as recurring, not as fixed.
- Because regeneration is risky, hand-editing the already-generated
  `soc_system.v` instantiation parameters is sometimes the safer path. If you do
  that, note it prominently — it is invisible to anyone reading only `.qsys`.

### 2.4 Bitstream generation and flashing

- **Always convert with compression on:**
  `quartus_cpf -c -o bitstream_compression=on foo.sof soc_system.rbf`
- A `.rbf` built **without** compression is roughly **2.4x larger** and will not
  load — a silent bring-up failure that looks like a dead design.
- **Sanity-check the output size against the last known-good `.rbf` before
  copying it to the card.** A large size delta sitting in the same directory is
  the cheapest possible check and was missed once here.
- **Prefer the project's existing script/Makefile target over hand-typing an
  equivalent command.** The flags such scripts carry are load-bearing and
  usually undocumented at the call site.
- On the DE10-Standard the bitstream lives on the **FAT partition
  (`/dev/mmcblk0p1`)** as `soc_system.rbf`, loaded by u-boot at boot. You can
  mount it from the running HPS Linux (`mount /dev/mmcblk0p1 /mnt/boot`), replace
  the file over SSH, `sync`, and reboot — **no SD card removal required.**
- **Back up the working `.rbf` on the FAT partition before overwriting it.**

---

## 3. Verification discipline

### 3.1 Test stimulus **[Universal — this is the big one]**

- **Never verify spatial or indexing logic with a smooth/structured synthetic
  image (ramp, gradient, constant).** Such a stimulus can be *invariant under
  the very transformation the bug introduces*, making an exact, reproducible,
  wrong offset look like the unique correct answer across an entire test run.
- Use stimulus with **enough entropy that only the true alignment can match**
  (e.g. `rng.integers()` over a realistic value range).
- "The RTL passes" is only as strong a claim as the test data's ability to
  distinguish right from wrong.
- Keep a smooth ramp around as a *diagnostic* tool (clean constant errors are
  informative once you know a bug exists) — just never as the *acceptance* test.

### 3.2 Golden references

- The reference generator is **as likely to be wrong as the RTL.** Multiple bugs
  in this project were in the MATLAB/Python reference, not the hardware.
- **Cross-check with a second, independently written model** (this project used
  MATLAB *and* a from-scratch Python replica). Two implementations that agree
  are strong evidence; one is an assumption.
- Watch for **same-name-different-meaning variables** across scripts — a classic
  source of "the golden reference is itself wrong."
- **Indexing conventions must match exactly**: centre-indexed vs bottom-right
  indexed, absolute-pixel-position vs valid-output-counter. A reference file
  written one-line-per-absolute-position compared against a counter that only
  advances on valid outputs will mismatch on essentially every line.
- Prefer indexing both sides by the **same absolute stream position** with an
  explicit latency offset, over maintaining a separate skip counter.

### 3.3 Latency bookkeeping

- The moment a testbench checks an **intermediate** signal in addition to the
  final output, it re-introduces multi-latency bookkeeping. Each signal has its
  *own* offset. Do not assume a working final-stage offset applies upstream.
- Maintain an explicit **latency ledger** comment in the testbench listing each
  checked signal and its offset, derived from the pipeline structure.

### 3.4 What "bit-exact" does and doesn't certify

- Proving signal A bit-exact certifies **nothing** about signal B, even when
  both are produced by the same module on the same cycle, if B additionally
  depends on a separately-timed input.
- In this project `T_log_code` was 100.0000% bit-exact while `detect` — computed
  from the same threshold *plus* a separately-delayed pixel — was unreliable.
- **Rule:** enumerate each output's *full* dependency set before claiming a test
  covers it.

### 3.5 New testbench vs stable design

- A **FAIL from a newly written testbench against a long-stable design** should
  raise "check the testbench's own assumptions first" at least as high as "the
  design has a new bug." Several "failures" here were testbench bugs.
- Equally: an **equivalence testbench is only fair while both sides genuinely
  share the thing being held constant.** When one side's reset gained extra
  logic, a long-latent race turned into 285 mismatches that looked exactly like
  a real regression.

---

## 4. Hardware bring-up checklist

Run these **in order**. Each is a gate on the next. Do not skip ahead to
measuring performance.

### 4.1 Gate 0 — Reproducibility (do this first, always)

```
Run the SAME input through the hardware 4 times.
All 4 results MUST be bit-identical.
```

- If they are not, **stop.** Per-frame state is leaking (missing/ineffective
  soft reset), or timing is failing. Every performance number measured in this
  state is void.
- This check takes seconds and would have saved this project a 15-minute sweep
  that produced a confident, completely meaningless result
  (98.7% detection rate at a 94x-too-high false-alarm rate).
- **Note:** run this in a *fresh process* too — if even the first frame of a new
  process differs, the board is retaining state across process boundaries.

### 4.2 Gate 1 — Correctness vs the software model

- Compare hardware output against the bit-exact reference model on both
  synthetic and real input.
- **Sweep the result-index offset** (e.g. −2..+3) before concluding "the values
  are wrong." A constant offset means a *shift* (an indexing bug), not bad
  arithmetic — a completely different fix. In this project the hardware's first
  emitted result was window index **1**, not 0, and misreading that as a value
  bug caused ~100% apparent mismatch.
- A near-zero match rate at *every* offset means a genuine value problem.
- Weak-but-nonzero correlation usually means something structural (wrong LUT,
  wrong scale), not noise.

### 4.3 Gate 2 — Only now, measure performance

- Only after Gates 0 and 1 pass should you run a full dataset sweep or benchmark.
- **Build the gate into the measurement script** so it physically cannot produce
  numbers from unverified hardware. Exit non-zero on gate failure.

### 4.4 When hardware and simulation disagree

Work down this list — cheapest and most likely first:

1. **Is it reproducible?** (§4.1) Non-reproducible ⇒ state leak or timing, not logic.
2. **Is the bitstream actually the one you think?** Check its build date and
   size. Check that your parameters really elaborated — read the value out of
   `output_files/*.map.rpt`, don't trust the source file.
3. **Does the host software assume registers this bitstream has?** Diff the RTL
   against the commit the bitstream was built from. *Undecoded Avalon writes are
   silent.*
4. **Is a result-index offset involved?** Sweep it.
5. **Is timing actually closed?** Read `*.sta.summary`.
6. **What does real hardware do that no testbench does?** (gaps, stalls,
   multiple frames, warm chip)
7. Only then suspect the RTL logic itself.

### 4.5 Calibrate the instrument before trusting the measurement

- When a debugging chain narrows to a single observation, **audit what that
  observation assumes** before building more instrumentation on top of it.
- In this project, three unverified board-level facts sat underneath a "the core
  is dead" theory — each individually capable of producing the symptom, each
  checkable in a two-minute build.
- The reflex to add more debug signals to the complex design was wrong.
  Building a tiny **known-good calibration design** (blink an LED, echo a
  register) to validate the measurement path was right.

### 4.6 Debug signal instrumentation

- Expose internal state as **real ports** (hierarchical references don't
  synthesize — §1.9).
- Make debug bits **sticky latches** (set once, held) so a human can see them on
  an LED without catching a nanosecond pulse.
- Include a **free-running heartbeat counter bit** that proves `clk`/`rstn` are
  alive independently of anything else working.
- Route them to spare LEDs via a Qsys conduit interface.

---

## 5. HPS / Linux integration (DE10-Standard specific)

- **Do not plan to run a modern Python stack on the stock image.** Angstrom
  v2014.12 has Python 2.7 only, no pip, ~51 MB free, and dead package feeds.
  Building Python 3 + numpy from source on a 925 MHz Cortex-A9 is not realistic.
  **Move the boundary, not the language:** write a small C daemon on the HPS that
  owns `/dev/mem`, and talk to it over TCP from a development machine.
- `mmap` `/dev/mem` at the lightweight bridge base (`0xFF200000`, span
  `0x200000`), then index your peripheral at its Qsys offset. Confirm the offset
  from `soc_system.sopcinfo`'s `<baseAddress>`, not from memory.
- **Blocking MMIO stores respect `avs_waitrequest` in hardware** — the ARM store
  instruction does not retire until the peripheral is ready. You do not need
  software polling to honor backpressure.
- **A read-to-clear status bit must be polled once per write, and results must be
  drained in a `while` loop, not sampled once.** Downstream pipeline stages keep
  advancing on their own valid chains, so several results can land between your
  writes. A single poll per write silently *drops* results — measured at 41 lost
  over one 512×512 frame. And a dropped result **shifts every subsequent result
  onto the wrong position**, which looks like a total value corruption rather
  than a missing sample.
- **Background processes:** plain `nohup … &` does **not** survive SSH session
  end on this board's shell. Use
  `setsid ./daemon > daemon.log 2>&1 < /dev/null &` then `disown`.
- **Verify a daemon is running from a *fresh* SSH connection** (`netstat -ltn`),
  not from the session that started it. Also note `pgrep -f <name>` can match
  its own command line and report a false positive.
- Streaming input must be sized to the **synthesis-time** frame dimensions. A
  narrower raw stream never completes a row and produces zero output — pad to
  the compiled size first.

---

## 6. Environment (WSL / Windows)

- `systemd=true` in `wsl.conf` **breaks Windows interop** — native `.exe`
  invocation from WSL fails entirely.
- `appendWindowsPath=true` breaks downstream tools when `PATH` contains spaces
  or dead drive letters.
- **Test the mechanism in isolation before blaming the tool.** "This tool
  crashes" and "this environment cannot launch native tools at all" look
  identical from one failed invocation. `cmd.exe /c echo hello` separates them
  in one cheap check.
- **Do not bundle a speculative "probably helps" setting change with a confirmed
  necessary one.** Doing so here cost a full extra debugging round-trip.
- **PowerShell output buffering:** `quartus_map … | tail -N` yields nothing until
  the whole pipeline closes. Redirect to a plain log file (`> build.log 2>&1`)
  and `grep`/`cat` it at any time instead.
- **Backgrounding:** don't nest `nohup … &` inside an already-backgrounded call —
  the outer command returns instantly and the real work becomes untracked.

---

## 7. Process rules

### 7.1 Provenance

- **Version-control everything a synthesis step reads**, including generated
  data files (`.hex` ROM images, coefficient tables). A file living outside the
  repo that `$readmemh` bakes into the bitstream is a silent external dependency:
  your source can be perfectly reverted while the flashed binary still reflects
  whatever was in that directory when it was built — and nothing records that.
- **Archive build inputs and outputs together per release**, or at minimum
  record the commit hash and the checksum of every generated input.
- **Reverting source to "match" a saved bitstream matches only what the source
  says, never what the binary contains.** When in doubt, rebuild.

### 7.2 Investigation

- **Two crashes with an identical low-level error signature are the same bug
  until proven otherwise**, even when surface symptoms look unrelated. Match the
  exact file/line/stack trace, not just "another crash."
- **A reproducible crash is a lead, not automatically the blocker.** Ask what
  the crashing step actually *produces* and whether anything downstream needs
  it. In this project the real bug sat one layer away from where every error
  message pointed.
- **Investigate why an assertion exists before routing around it.** A library's
  "input must be odd" check was pointing at a genuine structural property of the
  hardware, and understanding it turned a chore into a real improvement.
- **A coincidental partial match under an incorrect theory is a red herring.**
  Correct the theory mid-investigation rather than accepting the partial match.
- **Record wrong hypotheses and how they were disproven**, not just the final
  answer. The reasoning that eliminated a plausible-but-wrong explanation is
  often more reusable than the fix.

### 7.3 Documentation

Keep a numbered bug log with a fixed structure — it compounds in value:

```
### N. One-line title naming the actual root cause

**Symptom:**       what was observed, with real numbers
**Root cause:**    the mechanism, verified not assumed
**Fix:**           what changed
**Lesson:**        the transferable, generalizable rule
```

- Include the **exact numbers** (slack values, resource counts, match
  percentages, error signatures). They make a recurrence instantly
  recognizable.
- Note explicitly when a workaround **must be re-applied** after a regeneration
  step, rather than marking it "fixed."

---

## 8. Pre-flight checklist (condensed)

**While writing RTL**
- [ ] No clocked `always` block chains several multiplies/divides between one
      pair of registers (§1.10) — budget ~one big operation per stage
- [ ] No intermediate declared wider than its real range
- [ ] No reduction tree stacked on another reduction tree in one cycle

**Before synthesis**
- [ ] Parameters changed in *every* duplicated location (RTL, `_hw.tcl`, `.qsys`,
      generated `.v`, software config, all test/reference generators)
- [ ] Golden vectors regenerated for the new configuration
- [ ] Full simulation regression passes
- [ ] Value-level check passes at the **real production** parameters, not just a
      small test configuration
- [ ] No hierarchical references in synthesizable code

**After synthesis (`quartus_map`)**
- [ ] 0 errors, no `Internal Error`
- [ ] Parameters confirmed **from `*.map.rpt`**, not from the source file
- [ ] Memory bits look plausible (not near-zero for something that is memory)
- [ ] DSP / resource counts in line with expectations

**After fit + `quartus_sta`** ← *never skip*
- [ ] Positive setup slack on the **slow, hot** corner
- [ ] TNS ≈ 0
- [ ] Resource usage within budget

**Before flashing**
- [ ] `.rbf` built **with compression**
- [ ] `.rbf` size sane vs last known-good
- [ ] Previous working `.rbf` backed up
- [ ] Bitstream and host software built from the **same source revision**

**After flashing**
- [ ] Gate 0: 4 identical runs → bit-identical output
- [ ] Gate 1: matches the software model (with offset sweep)
- [ ] Only then: measure performance

---

## 9. Prompt snippet for a new session

> This project targets a Terasic DE10-Standard (Cyclone V `5CSXFC6D6F31C6`,
> 41,910 ALMs, 112 DSP) with Quartus Prime Lite 21.1, Icarus Verilog for
> simulation, and an HPS running an old Angstrom Linux (Python 2.7 only).
> Follow `fpga_checklist.md` in this repo. In particular:
> never infer RAM inline in a `generate for` body; never use hierarchical
> references in synthesizable code; always run `quartus_sta` and read the actual
> slack before flashing; always verify hardware reproducibility (same input →
> bit-identical output) before trusting any measurement; and never pair a saved
> bitstream with newer host software. Verify claims against real evidence —
> tool output, measured numbers — rather than asserting them, and keep a
> numbered bug log with Symptom / Root cause / Fix / Lesson for each issue found.
