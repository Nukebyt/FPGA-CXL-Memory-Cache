// Phase 5 testbench: cache hit/miss/allocate/evict correctness. All
// addresses used are Device-Biased (the reset default), so this test
// deliberately doesn't need a BI host model -- Phase 3/4 already covered
// bias interaction; this is purely about the cache mechanism itself, per
// the manual's own scoping ("functionally this part is 'just a cache,'
// verify it as such").
#include "Vcxl_mem_protocol_engine.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#if VM_COVERAGE
#include "verilated_cov.h"
#endif

#include <cstdint>
#include <cstdio>

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
    tick(dut, tfp); // consume (s2m_ready held high)
    return r;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_mem_protocol_engine;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase5_waveform.vcd");

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
    const uint32_t A = 0x05; // index=5, tag_field=0 (CACHE_IDX_W=4 default)
    const uint32_t B = 0x15; // index=5, tag_field=1 -- same index as A, different tag

    // --- Step 1: write A -- first touch, must be a MISS (allocate). ---
    uint32_t misses_before = dut->dbg_cache_misses;
    fire(dut, tfp, true, A, 1, 0xAAAA0001u);
    Resp r1 = drain_one(dut, tfp);
    if (r1.is_data || r1.tag != 1) { printf("FAIL: write-A response wrong (is_data=%d tag=%u)\n", r1.is_data, r1.tag); errors++; }
    if (dut->dbg_cache_misses != misses_before + 1) {
        printf("FAIL: write-A should count as a cache miss (first touch); misses %u -> %u\n",
               misses_before, (uint32_t)dut->dbg_cache_misses);
        errors++;
    } else {
        printf("PASS: write-A (first touch) correctly counted as a miss\n");
    }

    // --- Step 2: read A back -- must now be a HIT, fast (0 extra latency
    // is implicit in timing, correctness checked via the counter and data). ---
    uint32_t hits_before = dut->dbg_cache_hits;
    fire(dut, tfp, false, A, 2, 0);
    Resp r2 = drain_one(dut, tfp);
    if (!r2.is_data || r2.tag != 2 || r2.rdata != 0xAAAA0001u) {
        printf("FAIL: read-A-back wrong (is_data=%d tag=%u rdata=0x%08x)\n", r2.is_data, r2.tag, r2.rdata);
        errors++;
    }
    if (dut->dbg_cache_hits != hits_before + 1) {
        printf("FAIL: read-A-back should be a cache hit; hits %u -> %u\n", hits_before, (uint32_t)dut->dbg_cache_hits);
        errors++;
    } else {
        printf("PASS: read-A-back correctly hit the cache with correct data\n");
    }

    // --- Step 3: write B -- same cache index as A, different tag. A is
    // dirty (written in step 1, never evicted), so this MUST trigger an
    // eviction-writeback of A before B can be allocated. ---
    fire(dut, tfp, true, B, 3, 0xBBBB0002u);
    Resp r3 = drain_one(dut, tfp);
    if (r3.is_data || r3.tag != 3) { printf("FAIL: write-B response wrong\n"); errors++; }
    else printf("PASS: write-B (evicting A) completed\n");

    // --- Step 4: read A again -- must now MISS (B occupies the index),
    // but must still return A's correct data, fetched from backing memory
    // where it was written back in step 3 -- the classic
    // read-after-evict-after-write correctness check (manual's Phase 6
    // corner case; a basic version verified here, a more thorough
    // dedicated test lives in sim/phase6). ---
    misses_before = dut->dbg_cache_misses;
    fire(dut, tfp, false, A, 4, 0);
    Resp r4 = drain_one(dut, tfp);
    if (!r4.is_data || r4.tag != 4 || r4.rdata != 0xAAAA0001u) {
        printf("FAIL: read-A-after-eviction wrong (is_data=%d tag=%u rdata=0x%08x, expected 0xaaaa0001) -- "
               "eviction writeback did not preserve A's data correctly\n", r4.is_data, r4.tag, r4.rdata);
        errors++;
    } else {
        printf("PASS: read-A-after-eviction correctly returned A's data via the eviction writeback path\n");
    }
    if (dut->dbg_cache_misses != misses_before + 1) {
        printf("FAIL: read-A-after-eviction should be a cache miss; misses %u -> %u\n",
               misses_before, (uint32_t)dut->dbg_cache_misses);
        errors++;
    }

    // --- Step 5: confirm B is still correctly cached (wasn't disturbed by
    // step 4's re-eviction-of-A-back-in... wait, step 4 was a READ of A,
    // which is itself a MISS against B's now-resident line -- does reading
    // A evict B in turn? It must, by the same direct-mapped logic, since A
    // and B share an index. Confirm B's data was correctly written back
    // too, not lost. ---
    fire(dut, tfp, false, B, 5, 0);
    Resp r5 = drain_one(dut, tfp);
    if (!r5.is_data || r5.tag != 5 || r5.rdata != 0xBBBB0002u) {
        printf("FAIL: read-B-after-re-eviction wrong (is_data=%d tag=%u rdata=0x%08x, expected 0xbbbb0002)\n",
               r5.is_data, r5.tag, r5.rdata);
        errors++;
    } else {
        printf("PASS: B's data correctly preserved through its own eviction-writeback when A was re-read\n");
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase5.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE5 PASS: cache hit/miss/allocate/evict all verified correct\n");
        return 0;
    }
    printf("PHASE5 FAIL: %d errors\n", errors);
    return 1;
}
