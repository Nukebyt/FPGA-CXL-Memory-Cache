// Phase 6 testbench: dirty-line writeback + eviction correctness, stress-
// tested more thoroughly than Phase 5's basic single-round check. The
// mechanism itself was already built (and unit-verified) in Phase 5 --
// this phase's job is proving it holds up under repeated cycling and
// thrash, per the manual's "classic corner case to get wrong" framing.
#include "Vcxl_mem_protocol_engine.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#if VM_COVERAGE
#include "verilated_cov.h"
#endif

#include <cstdint>
#include <cstdio>
#include <vector>

static vluint64_t sim_time = 0;

static void tick(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp) {
    dut->clk = 0;
    dut->eval();
    if (tfp) tfp->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    if (tfp) tfp->dump(sim_time++);
}

static void fire(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp,
                  bool is_write, uint32_t addr, uint32_t tag, uint32_t wdata) {
    dut->m2s_addr = addr;
    dut->m2s_tag = tag;
    dut->m2s_is_write = is_write;
    dut->m2s_wdata = wdata;
    dut->m2s_valid = 1;
    dut->eval();
    int guard = 0;
    while (!dut->m2s_ready) {
        tick(dut, tfp);
        dut->eval();
        if (++guard > 200) { fprintf(stderr, "TIMEOUT firing tag %u\n", tag); exit(1); }
    }
    tick(dut, tfp);
    dut->m2s_valid = 0;
}

struct Resp { bool is_data; uint32_t tag; uint32_t rdata; };

static Resp drain_one(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp) {
    int guard = 0;
    while (!dut->s2m_valid) {
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT draining response\n"); exit(1); }
    }
    Resp r{(bool)dut->s2m_is_data, dut->s2m_tag, dut->s2m_rdata};
    tick(dut, tfp);
    return r;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_mem_protocol_engine;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase6_waveform.vcd");

    dut->rst_n = 0;
    dut->m2s_valid = 0;
    dut->s2m_ready = 1;
    dut->bias_wr_en = 0;
    dut->bias_addr = 0;
    dut->bias_wr_data = 0;
    dut->bi_req_ready = 0;
    dut->bi_rsp_valid = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rst_n = 1;

    int errors = 0;

    // --- Test 1: cycle through 4 addresses that all alias to the SAME
    // cache index (CACHE_IDX_W=4 default -> index = addr & 0xF), each
    // written with a distinct value, then read back in a DIFFERENT order
    // than written -- every eviction along the way must correctly
    // preserve the outgoing line's data via writeback, not just the
    // immediately-previous one. ---
    const uint32_t idx = 0x07;
    struct Line { uint32_t addr, value; };
    std::vector<Line> lines = {
        {0x00 | idx, 0x11111111u},
        {0x10 | idx, 0x22222222u},
        {0x20 | idx, 0x33333333u},
        {0x30 | idx, 0x44444444u},
    };

    const uint32_t tag = 1; // safe to reuse: each fire()+drain_one() pair fully completes (tag freed) before the next starts
    for (auto &l : lines) {
        fire(dut, tfp, true, l.addr, tag, l.value);
        Resp r = drain_one(dut, tfp);
        if (r.is_data || r.tag != tag) { printf("FAIL: write to 0x%02x wrong response\n", l.addr); errors++; }
    }
    // Now read them back in REVERSE order -- each read (except possibly
    // the last-written line, which may still be cache-resident) forces an
    // eviction of whatever currently occupies the index, which must not
    // disturb correctness of the read actually being serviced, nor
    // corrupt the data of the line being evicted (verified implicitly by
    // reading everything back afterward too).
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        fire(dut, tfp, false, it->addr, tag, 0);
        Resp r = drain_one(dut, tfp);
        if (!r.is_data || r.tag != tag || r.rdata != it->value) {
            printf("FAIL: reverse-order read of 0x%02x expected 0x%08x got 0x%08x (is_data=%d)\n",
                   it->addr, it->value, r.rdata, r.is_data);
            errors++;
        } else {
            printf("PASS: 0x%02x correctly read back 0x%08x after multi-round eviction cycling\n", it->addr, r.rdata);
        }
    }

    // --- Test 2: thrash -- write all 4 lines again with NEW values, in
    // forward order, back-to-back (each write evicting the previous
    // occupant), then verify ALL FOUR still read back correctly in
    // forward order. This is the strongest test: by the time we read
    // line 0 back, lines 1, 2, and 3 have each been written (and line 0
    // evicted once already for line 1's write) -- every value must still
    // be correctly recoverable from backing memory via its own writeback,
    // with zero cumulative data loss across the whole thrash sequence. ---
    for (size_t i = 0; i < lines.size(); i++) {
        lines[i].value = 0xA0000000u + (uint32_t)i;
        fire(dut, tfp, true, lines[i].addr, tag, lines[i].value);
        Resp r = drain_one(dut, tfp);
        if (r.is_data) { printf("FAIL: thrash write %zu wrong response type\n", i); errors++; }
    }
    bool thrash_ok = true;
    for (auto &l : lines) {
        fire(dut, tfp, false, l.addr, tag, 0);
        Resp r = drain_one(dut, tfp);
        if (!r.is_data || r.rdata != l.value) {
            printf("FAIL: thrash readback of 0x%02x expected 0x%08x got 0x%08x\n", l.addr, l.value, r.rdata);
            errors++;
            thrash_ok = false;
        }
    }
    if (thrash_ok) printf("PASS: full thrash sequence (4 writes, 4 evictions, 4 readbacks) -- zero data loss\n");

    // --- Test 3 (negative): a write to an address at a DIFFERENT cache
    // index than anything touched above must NOT trigger any eviction --
    // confirm cache_misses increments by exactly the expected amount (one
    // miss for the new line's own allocate) and nothing about the
    // untouched, unrelated lines changes. ---
    const uint32_t unrelated_idx_addr = 0x08; // index=8, untouched by tests 1-2 (which used index 0x07)
    uint32_t misses_before = dut->dbg_cache_misses;
    fire(dut, tfp, true, unrelated_idx_addr, tag, 0xDEADBEEFu);
    drain_one(dut, tfp);
    if (dut->dbg_cache_misses != misses_before + 1) {
        printf("FAIL: unrelated-index write should count as exactly one new miss (its own allocate), "
               "got %u -> %u\n", misses_before, (uint32_t)dut->dbg_cache_misses);
        errors++;
    }
    // Confirm the last thrashed line (still cache-resident from test 2) is unaffected.
    fire(dut, tfp, false, lines.back().addr, tag, 0);
    Resp rcheck = drain_one(dut, tfp);
    if (rcheck.rdata != lines.back().value) {
        printf("FAIL: unrelated write disturbed an untouched cache line (index 0x07), got 0x%08x expected 0x%08x\n",
               rcheck.rdata, lines.back().value);
        errors++;
    } else {
        printf("PASS: write to an unrelated cache index did not disturb index 0x07's resident line\n");
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase6.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE6 PASS: multi-round eviction, thrash, and index-isolation all verified with zero data loss\n");
        return 0;
    }
    printf("PHASE6 FAIL: %d errors\n", errors);
    return 1;
}
