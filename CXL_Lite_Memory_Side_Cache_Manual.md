# Building a CXL-Lite Memory-Side Cache Model
## A Complete Manual — Theory, Architecture, Verification Plan, and Interview Prep

**Target:** Intel / AMD / memory-system / SoC-fabric interview readiness (CXL is one of the hottest 2026 hiring topics — see Part 0)
**Scope:** Protocol-level model of a CXL.mem memory-side cache (Type 3 device style), transaction-level, simulated only — no PHY, no PCIe electricals, no FPGA
**Prereqs assumed:** SystemVerilog/UVM, the coherence/ordering thinking you already built on the AXI4 crossbar project

---

## Part 0 — Why This Project, and What "CXL-Lite" Means Here

CXL (Compute Express Link) is the standard the whole industry is converging on for **memory disaggregation and pooling** — letting a CPU access memory that physically lives on another device, over what is electrically a PCIe link, while keeping some notion of coherence. It's the single most-cited trend across Intel, AMD, and hyperscaler silicon teams right now: HBM/CXL co-design, CXL-attached memory expanders, and "memory-side caches" that hide the extra latency of going off-chip over CXL are active engineering problems, not settled ones.

**"Lite" and "protocol-level" mean:** you are not building PCIe electrical PHY, link training, or physical retimers — that's a multi-year silicon team effort and isn't verifiable meaningfully in a student project anyway. You *are* building the **transaction-level protocol engine and cache controller logic** that sits above the PHY: request/response channel handling, the CXL bias/coherence state machine, and a cache that intercepts and accelerates memory accesses. This is exactly the layer that a CXL controller IP or verification team actually spends most of their time on — the PHY is bought as a hard IP; the protocol logic is what gets designed and verified in-house, which is why this scope is both realistic *and* high-signal.

---

## Part 1 — CXL Protocol Theory (own this cold)

### 1.1 The three CXL sub-protocols

CXL isn't one protocol — it's three, multiplexed over the same physical link:

| Sub-protocol | Purpose |
|---|---|
| **CXL.io** | Non-coherent, PCIe-based — device discovery, configuration, enumeration. Basically "PCIe as usual." |
| **CXL.cache** | Lets a **device** cache the **host's** memory, coherently. Device-to-Host (D2H) and Host-to-Device (H2D) channels carry requests, snoops, and data. |
| **CXL.mem** | Lets the **host** access **device-attached** memory (e.g., a memory expander card's DRAM). Master-to-Subordinate (M2S) and Subordinate-to-Master (S2M) channels. |

**This project targets CXL.mem**, because a memory-side cache is fundamentally a CXL.mem concept: the "subordinate" (device) side is managing access to its own attached memory and deciding what to cache and how to stay coherent with the host.

### 1.2 CXL.mem channels — the ones you'll actually model

- **M2S Req** — host requests without data (e.g., a read request, or a metadata-only request).
- **M2S RwD** (Request with Data) — host requests carrying write data.
- **M2S BIRsp** — host's response to a **Back-Invalidate** snoop the device initiated (see 1.4 — this is the channel your cache controller cares about most).
- **S2M NDR** (No Data Response) — device's response with no data payload (e.g., write completion).
- **S2M DRS** (Data Response) — device's response carrying read data.

### 1.3 Why CXL.mem doesn't use classic MESI-style broadcast snooping

A full MESI/MOESI directory or snoop-broadcast scheme (like you'd use for a multi-core cache-coherent interconnect) would require the host to track coherence state for every line the device might cache — expensive, and defeats the point of disaggregated memory. Instead, CXL.mem uses a **much lighter mechanism: bias**.

### 1.4 Device Bias vs Host Bias — the actual theoretical core of this project

Every line of device-attached memory is, at any moment, in one of two "bias" states:

- **Host Bias:** the host may have a cached copy of this line (e.g., in its own last-level cache). The device **must not** service a read/write for this line out of its own memory-side cache without first checking with the host (in practice: the device either forwards the request or the host is assumed to hold the authoritative copy) — the device's memory-side cache must treat these lines cautiously.
- **Device Bias:** the host does **not** have a cached copy. The device is free to access, cache, and modify this line locally without host involvement — this is the fast path, and it's the whole reason bias exists: most memory-side cache accesses should be able to happen without a round trip to the host.

**Switching bias** (Host Bias → Device Bias) requires the device to issue a **Back-Invalidate (BI) request** to the host, asking the host to drop/writeback any cached copy, and wait for the host's BIRsp before the device can safely take ownership and start locally caching that line. This BI/BIRsp handshake is the single most important state machine in this entire project — it is directly analogous to a directory-based coherence protocol's invalidate/ack sequence, but scoped down to two parties (host, device) instead of N cores.

### 1.5 Why this matters for a *memory-side cache* specifically

A memory-side cache sits between the CXL.mem protocol engine and the device's backing memory (DRAM, or a mix of fast DRAM + slower media). Its job: absorb repeat accesses to hot lines so the device doesn't have to hit slower backing storage every time, and — critically — every cache allocate/evict decision it makes must respect the bias state of the line, because caching a Host-Biased line locally without the correct BI handshake is a **coherence violation**, not just a performance bug. This is what makes this project meaningfully harder than "add an LRU cache in front of memory" — the caching policy and the coherence protocol are entangled by design.

### 1.6 Ordering and completion semantics

CXL.mem requests complete asynchronously — M2S and S2M are independent channels (should feel familiar from AXI4's independent-channel model, but the completion semantics differ: CXL uses explicit completion IDs (tags) to match S2M responses back to outstanding M2S requests, since multiple requests can be outstanding simultaneously). You'll reuse the "track outstanding requests, route responses back correctly" skill from your AXI4 crossbar project directly here.

---

## Part 2 — Memory-Side Cache Architecture Theory

### 2.1 What is a "memory-side" cache, precisely?

Contrast with a CPU-side (last-level) cache: a CPU-side cache sits close to the compute core and caches whatever the core touches. A **memory-side cache** sits close to the memory itself (in this case, inside/near the CXL Type-3 device) and caches whatever *the memory controller* decides is hot, independent of which requester asked for it. Its purpose is purely to hide the latency/bandwidth gap between the CXL link + backing media and what a fast requester expects — this is a well-known real technique (HBM used as a memory-side cache in front of slower CXL-expanded capacity is a live 2026 industry pattern, per current CXL/HBM co-design trends).

### 2.2 High-level structure

```
   Host (M2S Req / M2S RwD / M2S BIRsp)
              │
              ▼
   ┌───────────────────────────┐
   │  CXL.mem Protocol Engine    │  (parses M2S, tags outstanding txns, drives S2M)
   └──────────────┬─────────────┘
                     │
                     ▼
   ┌───────────────────────────┐
   │   Bias Table                │  (per-line: Host-Biased / Device-Biased, one entry per cacheable region granule)
   └──────────────┬─────────────┘
                     │
                     ▼
   ┌───────────────────────────┐
   │   Memory-Side Cache          │  (tag array + data array, allocate/evict policy,
   │   Controller                  │   gated by bias state)
   └──────────────┬─────────────┘
                     │
          ┌──────────┴──────────┐
          ▼                        ▼
   Cache Hit (fast path)     Cache Miss → Backing Memory Model
                                          (simple latency-injected DRAM behavioral model)
```

### 2.3 Core modules to design

1. **CXL.mem protocol engine (transaction-level):** parses incoming M2S transactions, assigns/tracks tags for outstanding requests, formats outgoing S2M responses. This can be built as SystemVerilog interface + UVM-style transaction classes rather than pin-level RTL — appropriate for "protocol-level, simulated."
2. **Bias table:** a table (implement as an associative array or simple SRAM-model, keyed by address range/granule) holding current bias state per line/region. Must support atomic-looking transitions during the BI handshake (no line should be "in between" states from an external observer's point of view).
3. **Back-Invalidate (BI) sequencer:** the FSM that, on a Device-Bias-required cache allocate for a currently Host-Biased line, issues the BI request, blocks that line's pending requests, waits for BIRsp, then flips bias and proceeds. This is your hardest, highest-value module — treat it with the same care you gave the AW/W coupling FIFO in the AXI4 project.
4. **Cache tag/data array + allocate/evict policy:** a straightforward set-associative cache (reuse general cache design knowledge), but every allocate must first check bias state and trigger the BI sequencer if needed; every evict of a dirty Device-Biased line needs a clean writeback path to backing memory.
5. **Backing memory behavioral model:** doesn't need to be real DRAM RTL — a simple latency-injected memory array model (configurable access latency, maybe with a "slow tier vs fast tier" split if you want to show the memory-side-cache value proposition quantitatively) is sufficient and appropriate for this scope.
6. **Outstanding-transaction tracker:** tag-indexed table matching S2M responses back to M2S requests — directly reuses the same "who's waiting for what" skill from your AXI4 crossbar's outstanding-transaction table.

### 2.4 What you are deliberately NOT modeling (say this explicitly in your report)

- PCIe PHY/electricals, link training, flow control credits at the physical layer.
- CXL.io and CXL.cache in full (mention them for context, but the project is CXL.mem-focused).
- Multi-host / multi-device switch fabrics (CXL 2.0+ switching) — single host, single device is the right scope.

Being explicit about scope boundaries is itself a strong interview signal — it shows you understand the difference between the full standard and what's tractable to actually verify.

---

## Part 3 — Implementation Roadmap (phased, ~8–9 week plan, simulation-only)

| Phase | Weeks | Deliverable |
|---|---|---|
| 1. Protocol study + transaction classes | 1 | Define M2S Req/RwD/BIRsp and S2M NDR/DRS as SystemVerilog transaction classes; write a simple driver/monitor pair that can send/receive a basic read and write with no caching or bias logic yet (pure pass-through to a memory model) |
| 2. Backing memory model + tag/completion tracking | 1 | Add the outstanding-transaction tracker; verify multiple in-flight requests complete and route correctly, unordered |
| 3. Bias table + simple bias-aware gating | 1 | Add the bias table; requests to Device-Biased lines proceed normally, requests to Host-Biased lines are (for now) just forced to bypass the cache and go straight to backing memory — get this simple version correct first |
| 4. BI sequencer (the hard phase) | 2 | Implement the full Back-Invalidate handshake: device-initiated BI request, host BIRsp, atomic bias flip, then allow caching to proceed. Budget extra time here — this is your equivalent of the AXI4 project's coupling FIFO |
| 5. Memory-side cache (tag/data array, allocate/evict) | 1–2 | Wire the cache in behind the bias gating; implement a simple replacement policy (start with direct-mapped or 2-way, don't over-engineer this part — it's not where the interview signal is) |
| 6. Dirty-line writeback + eviction correctness | 1 | Evicting a dirty Device-Biased cached line must correctly write back to backing memory before reuse — a classic corner case to get wrong |
| 7. Verification hardening (runs in parallel from Phase 2 onward) | ongoing | See Part 4 |
| 8. Documentation + a small "value proposition" experiment | 1 | Run your model with the cache enabled vs disabled, latency-injected, and report the average access latency improvement — turns this from "a protocol model" into "a protocol model with a measured result," which is a strong resume line |

---

## Part 4 — Verification Plan

### 4.1 Verification strategy

Same two-tier approach as your other projects: block-level (bias table transitions, BI sequencer FSM, cache tag logic tested in isolation with directed tests) and system-level (full CXL.mem transaction flows, constrained-random address/access patterns, coverage-closed).

### 4.2 UVM environment architecture

```
cxl_mem_cache_env
 ├── config_object                  (cache size/associativity, bias granularity, backing memory latency)
 ├── host_agent                       (active — drives M2S Req/RwD, responds to BI with BIRsp)
 │     ├── host_sequencer
 │     ├── host_driver
 │     └── host_monitor
 ├── device_response_monitor           (passive — observes S2M NDR/DRS coming from the DUT)
 ├── backing_memory_scoreboard_model    (a reference memory array — golden copy of what backing memory *should* contain, updated by observed writes)
 ├── bias_state_predictor                (reference model tracking expected bias state per line, driven by the same stimulus as the DUT — independently reimplemented, not copy-pasted from RTL logic)
 ├── scoreboard                          (compares DUT read data, bias transitions, and BI sequencing against the two reference models above)
 ├── coverage_collector                  (see 4.4)
 └── protocol_checker (SVA bind)         (see 4.5)
```

**Design note worth stating explicitly in interviews:** the bias-state predictor is a *second, independent* model of bias transitions, not derived from the DUT's own bias table — this avoids the "reference model has the same bug as the RTL" trap, same principle as the DPI-C golden model in your NPU project.

### 4.3 Test plan — feature list to verify

**Basic protocol correctness**
- Simple read/write to Device-Biased lines completes correctly with correct S2M response type and data.
- Multiple outstanding requests, different tags, complete out of order and route back correctly (direct reuse of your AXI4 ID-routing verification skill).

**Bias transition correctness — the core of this project's verification value**
- Read/write to a Host-Biased line triggers a BI request; verify BI is issued with correct addressing before any local cache allocate happens.
- BIRsp correctly and atomically flips the bias state — verify no request in flight during the transition observes an inconsistent state (e.g., gets serviced from a partially-allocated cache line before the bias flip formally completes).
- Repeated bias flips (line goes Device-Biased → Host-Biased → Device-Biased again, if your model supports host-initiated reclaim) — verify no state leakage or stale cache data reused incorrectly.

**Cache behavior**
- Allocate, hit, evict — basic cache correctness (functionally this part is "just a cache," verify it as such).
- Dirty-line eviction correctly writes back to backing memory before the cache line is reused — verify with a read-after-evict-after-write sequence that would return stale data if the writeback were missing or misordered.
- Cache disabled/bypass mode still produces correct data (useful as a debug mode and as a verification cross-check — data must match whether cache is on or off).

**Ordering / completion**
- Response tags always match the correct originating request, even under heavy concurrent outstanding-request load (stress test, similar structure to your AXI4 max-outstanding test).
- No response is ever generated for a request that hasn't been received (a "phantom completion" check — good defensive assertion).

**Error/edge conditions**
- Backing memory access during an in-flight BI handshake for the same line — verify correct blocking/ordering, not a race.
- Access pattern that thrashes the cache heavily (worst-case working set larger than cache) — correctness must hold even though performance degrades; this is a good place to separate "is it correct" tests from "is it fast" measurements (Part 3, Phase 8).

### 4.4 Functional coverage plan

- `access_type (read/write) × bias_state_before × bias_state_after` — did you exercise every legal transition?
- `cache_result (hit/miss/allocate/evict) × bias_state` — cross this explicitly, since the entanglement between caching and bias is the whole point of the project.
- `outstanding_request_count` bins, including max-outstanding stress scenarios.
- `dirty_eviction × subsequent_access_to_same_line` — did you verify writeback correctness, not just trigger it?

### 4.5 SVA assertions

- A line must never be cached locally (allocated into the memory-side cache) while its bias state is Host-Biased and no completed BI handshake has occurred for it — this is your single most important protocol-correctness assertion, directly encoding the coherence rule from Part 1.5.
- Every S2M response's tag must match a currently-outstanding M2S request's tag (no phantom completions, no double completions).
- A BI request, once issued, must receive exactly one BIRsp before the corresponding line's bias state changes.
- No X propagation on cache tag/data arrays during a valid access.

### 4.6 Bug log

Same discipline as your other projects — keep a running table. Given the bias/coherence entanglement, expect your best bug stories to come from Phase 4 (the BI sequencer) — races between an in-flight access and a bias transition are the classic bug class here, directly analogous to the AW/W coupling bug in your crossbar project but one layer more subtle because it's a coherence correctness issue, not just a routing issue.

---

## Part 5 — Interview Q&A Bank

### CXL protocol theory
- **Q: What are the three CXL sub-protocols and what does each one do?**
A: CXL.io (PCIe-based, non-coherent, config/enumeration), CXL.cache (device caches host memory, coherently), CXL.mem (host accesses device-attached memory) — and be ready to say immediately that your project targets CXL.mem specifically, and why.

- **Q: Why doesn't CXL.mem use a full snoop-broadcast or directory-based coherence scheme like a multi-core cache hierarchy would?**
A: Full coherence tracking at fine granularity between host and every CXL device would be expensive and defeats the latency/scalability goals of disaggregated memory; CXL.mem instead uses a lightweight two-state bias mechanism (Host Bias / Device Bias) that only requires explicit coordination (the BI handshake) at bias-transition points, not on every access.

- **Q: Explain the Back-Invalidate handshake and why it's needed.**
A: When a device wants to locally cache a line currently in Host Bias (meaning the host might have its own cached copy), it must first issue a BI request asking the host to invalidate/writeback any cached copy; only after receiving BIRsp can the device safely flip the line to Device Bias and begin local caching — without this handshake, the device could read/modify data the host still considers authoritative, a direct coherence violation.

- **Q: What's the difference between Host Bias and Device Bias, functionally?**
A: Host Bias = device must assume the host may have this line cached and cannot freely cache/modify it locally without coordination; Device Bias = device has exclusive local control and can service accesses from its own memory-side cache without host involvement, which is the fast path most accesses should end up on.

### Architecture / design
- **Q: Why is a memory-side cache architecturally different from a normal CPU-side last-level cache?**
A: A memory-side cache sits near the memory/device and caches based on what the memory controller observes as hot, independent of a specific requesting core's locality behavior; its purpose is to hide backing-media and link latency, not core-side access latency — and in a CXL context, its caching decisions are additionally constrained by bias state, which a normal CPU-side cache doesn't have to reason about at all.

- **Q: Walk me through what happens, step by step, when a request arrives for a Host-Biased line that isn't yet cached.**
A: (Use your real design — should include: protocol engine parses M2S Req, bias table lookup returns Host-Biased, cache controller triggers BI sequencer instead of allocating immediately, BI request sent to host, request held/blocked, BIRsp received, bias table atomically updated to Device-Biased, cache allocate now proceeds, S2M response finally issued.)

- **Q: How would you extend this to support multiple hosts or a switched CXL topology?**
A: Be honest about scope — explain that multi-host bias arbitration and switch-level routing are a materially larger problem (need to track bias per host-device pair, and handle bias conflicts/arbitration when multiple hosts might want the same line), and that your project deliberately scoped to single-host/single-device to keep the coherence problem verifiable in the time available — this honesty is a good answer, not a weakness.

### Verification-specific
- **Q: Why did you build an independent bias-state predictor instead of just checking the DUT's own bias table?**
A: Checking the DUT against its own internal state only proves internal self-consistency, not correctness against the spec; an independently implemented reference model driven by the same stimulus catches the case where the DUT's bias logic itself has a bug that would otherwise look "consistent" with its own (wrong) bookkeeping.

- **Q: What's the hardest race condition you had to verify, and how did you catch it?**
A: (Use your real bug log — the strongest possible answer involves an access arriving while a BI handshake for the same line is in flight, and how your scoreboard/assertion caught the resulting coherence violation before you fixed the blocking logic.)

- **Q: How do you know your test suite actually exercises the bias-transition logic thoroughly, not just the common-case cache hit/miss path?**
A: Point directly at your `bias_state_before × bias_state_after` cross-coverage bins from 4.4 — coverage-driven proof, not just "I wrote a lot of tests."

### Systems / "why this project" framing
- **Q: Why did you choose CXL over, say, extending your AXI4 crossbar work?**
A: CXL introduces a genuinely new problem class — cross-device coherence with an explicit lightweight handshake protocol, and memory-side caching tied to that coherence state — that's directly relevant to where the industry (HBM/CXL co-design, memory pooling) is actually investing right now, and it let you reuse and extend real skills (outstanding-transaction tracking, independent reference modeling) from your prior project rather than starting from zero.

- **Q: If you had unlimited time, what's the next thing you'd add?**
A: A good answer: multi-host bias arbitration, a more realistic tiered backing-memory model (fast DRAM tier + slow tier, showing the memory-side cache's value proposition more concretely), or CXL.cache integration to show the device-caches-host-memory direction as well, for a fuller picture of the standard.

---

## Part 6 — Documentation Checklist

- [ ] Block diagram (Part 2.2, redrawn with your actual module/signal names)
- [ ] A written explanation of the bias mechanism and the BI handshake — this is your best "do you actually understand coherence" artifact
- [ ] Waveform/transaction-log screenshots: basic hit/miss, a full BI handshake sequence end-to-end, a dirty eviction + writeback
- [ ] Coverage report with explicit callout of the bias-transition cross-coverage bins
- [ ] The bug log table, with at least one bias/race-condition bug story front and center
- [ ] The Phase 8 "cache enabled vs disabled" latency experiment, with a number attached (e.g., "40% average latency reduction on a repeated-access workload") — quantitative results are rare in student projects and stand out
- [ ] README with an explicit "what I deliberately did not model" section (PHY, multi-host, CXL.cache/.io) — shows scope discipline, not a gap

---

Want the SystemVerilog transaction-class definitions (M2S/S2M types) and the bias-table + BI-sequencer FSM skeleton to start Phase 1–4, or the independent bias-state predictor / scoreboard structure first since that's what will let you validate the BI sequencer correctly once it exists?
