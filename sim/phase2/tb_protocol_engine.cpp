// Phase 2 testbench: verifies multiple truly-concurrent outstanding
// requests complete and route back correctly, INCLUDING out of issue
// order -- the actual thing Phase 2 needs to prove, not just tag-matching
// under FIFO completion (see manual Part 3 Phase 2 / roadmap Phase 2).
#include "Vcxl_mem_protocol_engine.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#if VM_COVERAGE
#include "verilated_cov.h"
#endif

#include <cstdint>
#include <cstdio>
#include <map>
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

// Fire a request; if the tag is currently busy (m2s_ready low), waits until
// it frees. Deasserts m2s_valid after acceptance. Does NOT wait for the
// response -- that's the whole point of this testbench.
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

struct Resp { uint32_t tag; bool is_data; uint32_t rdata; uint64_t arrival_order; };

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_mem_protocol_engine;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase2_waveform.vcd");

    dut->rst_n = 0;
    dut->m2s_valid = 0;
    // s2m_ready starts LOW and is only raised inside each test's explicit
    // drain loop. Holding it high the whole time is a real trap: once a
    // response becomes eligible the arbiter offers and (with s2m_ready
    // always 1) immediately consumes it on the very next cycle, REGARDLESS
    // of whether the testbench is watching yet -- an earlier draft of this
    // test held s2m_ready high throughout and silently lost a response
    // (tag 2, in what's now Test 2) that got arbitrated and consumed while
    // still inside a later fire() call's internal wait-for-ready loop,
    // before the dedicated draining loop had even started. Caught by
    // instrumented debug output showing only 4 of 5 expected responses.
    dut->s2m_ready = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rst_n = 1;

    int errors = 0;

    // --- Test 1: fire a READ then a WRITE with distinct tags, back to
    // back, before either completes. Writes have 0 extra latency, reads
    // have READ_EXTRA_LATENCY (3) extra cycles -- so the write (tag 12),
    // issued SECOND, must complete and respond BEFORE the read (tag 10),
    // issued FIRST. Tag 12 is deliberately NUMERICALLY HIGHER than tag 10:
    // the response arbiter is lowest-tag-first, so if the write still wins
    // despite being both later-issued AND higher-tagged, that's unambiguous
    // proof it's latency-driven, not an artifact of arbitration priority
    // coincidentally matching issue order. (Both tags must fit TAG_W=4's
    // 0-15 range -- an earlier draft used tag 20, which silently truncated
    // to 4 when assigned to the 4-bit m2s_tag port and produced a
    // misleading result; caught by instrumented debug output, not assumed.)
    std::map<uint32_t, uint32_t> mem_model;
    mem_model[0x09] = 0; // never written, expect reset value 0 on read

    fire(dut, tfp, false, 0x09, 10, 0);              // read, tag 10 (never-written addr, expect 0)
    fire(dut, tfp, true,  0x40, 12, 0xCAFEBABEu);     // write, tag 12
    mem_model[0x40] = 0xCAFEBABEu;

    std::vector<Resp> responses;
    uint64_t order = 0;
    int guard = 0;
    dut->s2m_ready = 1; // now start draining
    while (responses.size() < 2) {
        if (dut->s2m_valid) {
            responses.push_back({dut->s2m_tag, (bool)dut->s2m_is_data, dut->s2m_rdata, order++});
        }
        tick(dut, tfp);
        if (++guard > 500) { fprintf(stderr, "TIMEOUT draining test 1\n"); return 1; }
    }

    // Find each tag's arrival position.
    uint64_t pos_read10 = UINT64_MAX, pos_write12 = UINT64_MAX;
    for (auto &r : responses) {
        if (r.tag == 10) pos_read10 = r.arrival_order;
        if (r.tag == 12) pos_write12 = r.arrival_order;
    }
    if (pos_read10 == UINT64_MAX) { printf("FAIL: tag 10 (read) never responded\n"); errors++; }
    if (pos_write12 == UINT64_MAX) { printf("FAIL: tag 12 (write) never responded\n"); errors++; }
    if (pos_write12 != UINT64_MAX && pos_read10 != UINT64_MAX && !(pos_write12 < pos_read10)) {
        printf("FAIL: expected write (tag 12, issued 2nd) to complete BEFORE read (tag 10, issued 1st) -- "
               "got write at position %llu, read at position %llu (no out-of-order completion observed)\n",
               (unsigned long long)pos_write12, (unsigned long long)pos_read10);
        errors++;
    } else {
        printf("Out-of-order completion confirmed: write (tag 12) completed at position %llu, "
               "read (tag 10) at position %llu\n", (unsigned long long)pos_write12, (unsigned long long)pos_read10);
    }
    for (auto &r : responses) {
        if (r.tag == 10) {
            if (!r.is_data) { printf("FAIL: tag 10 response has is_data=0 (expected DRS)\n"); errors++; }
            if (r.rdata != 0) { printf("FAIL: tag 10 rdata mismatch, expected 0 got 0x%08x\n", r.rdata); errors++; }
        }
        if (r.tag == 12) {
            if (r.is_data) { printf("FAIL: tag 12 response has is_data=1 (expected NDR)\n"); errors++; }
        }
    }

    // --- Test 2: five concurrent requests (mix of reads/writes, distinct
    // tags), fired back-to-back with no waiting, then drained. Every tag
    // must appear exactly once with correct data, regardless of arrival
    // order -- directly reusing the AXI4 ID-routing verification pattern
    // the manual calls out.
    struct Req { bool is_write; uint32_t addr, tag, wdata; };
    std::vector<Req> reqs = {
        {false, 0x01, 1, 0},
        {true,  0x02, 2, 0x11111111u},
        {false, 0x02, 3, 0}, // NOTE: reads the OLD value at 0x02 if issued same-ish time as the write above;
                             // to keep this deterministic we instead read an address unrelated to concurrent writes.
        {true,  0x03, 4, 0x22222222u},
        {false, 0x04, 5, 0},
    };
    // Fix up test 2 to avoid same-address read/write races muddying the
    // out-of-order check (that's Test 1's job, not this one's) -- use
    // disjoint addresses per tag here so each expected value is unambiguous.
    reqs[2] = {false, 0x05, 3, 0}; // never written -> expect 0
    std::map<uint32_t, uint32_t> expect2 = {
        {1, mem_model.count(0x01) ? mem_model[0x01] : 0u},
        {2, 0}, // write ack, no data to check
        {3, 0u},
        {4, 0},
        {5, 0u},
    };

    dut->s2m_ready = 0; // stay not-ready while firing, so nothing gets silently consumed before we're draining
    for (auto &r : reqs) fire(dut, tfp, r.is_write, r.addr, r.tag, r.wdata);
    if (reqs[1].is_write) mem_model[reqs[1].addr] = reqs[1].wdata;
    if (reqs[3].is_write) mem_model[reqs[3].addr] = reqs[3].wdata;

    std::map<uint32_t, Resp> seen;
    guard = 0;
    dut->s2m_ready = 1; // now start draining
    while (seen.size() < reqs.size()) {
        if (dut->s2m_valid) {
            seen[dut->s2m_tag] = {dut->s2m_tag, (bool)dut->s2m_is_data, dut->s2m_rdata, 0};
        }
        tick(dut, tfp);
        if (++guard > 500) { fprintf(stderr, "TIMEOUT draining test 2\n"); return 1; }
    }
    for (auto &r : reqs) {
        if (!seen.count(r.tag)) { printf("FAIL: tag %u never responded (test 2)\n", r.tag); errors++; continue; }
        auto &resp = seen[r.tag];
        bool expect_is_data = !r.is_write;
        if (resp.is_data != expect_is_data) {
            printf("FAIL: tag %u is_data mismatch, expected %d got %d\n", r.tag, expect_is_data, resp.is_data);
            errors++;
        }
        if (!r.is_write) {
            uint32_t exp = expect2[r.tag];
            if (resp.rdata != exp) {
                printf("FAIL: tag %u rdata mismatch, expected 0x%08x got 0x%08x\n", r.tag, exp, resp.rdata);
                errors++;
            }
        }
    }

    // --- Test 3: reusing an in-flight tag stalls until it frees. Fire tag
    // 14 as a read (takes a few cycles to become eligible), immediately try
    // to fire ANOTHER request on tag 14 -- m2s_ready must be low for that
    // tag until the first one's response is drained.
    dut->m2s_addr = 0x06;
    dut->m2s_tag = 14;
    dut->m2s_is_write = 0;
    dut->m2s_wdata = 0;
    dut->m2s_valid = 1;
    dut->eval();
    while (!dut->m2s_ready) tick(dut, tfp);
    tick(dut, tfp); // accepted
    // Now tag 14 is outstanding (still counting down its read latency). A
    // second fire attempt on the SAME tag must stall.
    dut->m2s_addr = 0x07;
    dut->m2s_tag = 14;
    dut->m2s_is_write = 1;
    dut->m2s_wdata = 0xDEADu;
    dut->eval();
    if (dut->m2s_ready) {
        printf("FAIL: m2s_ready was high for an already-outstanding tag (14) -- per-tag occupancy not enforced\n");
        errors++;
    } else {
        printf("Per-tag occupancy confirmed: m2s_ready correctly low while tag 14 still outstanding\n");
    }
    // Drain tag 14's response, then confirm the stalled second request now proceeds.
    guard = 0;
    while (!(dut->s2m_valid && dut->s2m_tag == 14)) {
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT waiting tag 14 response\n"); return 1; }
    }
    tick(dut, tfp); // consume it (s2m_ready held high throughout)
    guard = 0;
    while (!dut->m2s_ready) {
        tick(dut, tfp);
        dut->eval();
        if (++guard > 200) { printf("FAIL: tag 14 never freed up for reuse after its response was drained\n"); errors++; break; }
    }
    if (dut->m2s_ready) {
        tick(dut, tfp);
        dut->m2s_valid = 0;
        printf("Tag reuse after drain confirmed: tag 14 accepted again once freed\n");
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase2.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE2 PASS: out-of-order completion, multi-tag routing, and per-tag occupancy all verified\n");
        return 0;
    }
    printf("PHASE2 FAIL: %d errors\n", errors);
    return 1;
}
