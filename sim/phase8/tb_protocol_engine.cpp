// Phase 8 testbench: cache_bypass_en correctness. Verifies bypass mode does
// exactly what it's documented to do (see cache_bypass_en's port comment in
// cxl_mem_protocol_engine.sv): every access behaves as it did pre-Phase-5
// (writes go straight to backing memory, reads always pay
// READ_EXTRA_LATENCY), and -- the subtle, worth-testing part -- the cache
// arrays are left completely untouched (frozen, not cleared) while
// bypassed, so re-enabling the cache can expose stale cached data relative
// to what backing memory now holds. That's a deliberate, documented
// consequence of the design (measure "cache vs no cache" for an identical
// access pattern, not "cold cache vs warm cache"), not a bug -- this test
// exists to keep it that way on purpose, not by accident.
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

// Also counts cycles elapsed until the response is seen valid -- used to
// confirm bypassed reads genuinely pay READ_EXTRA_LATENCY rather than
// returning instantly via a stale/short-circuited hit path.
static Resp drain_one(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp, int *cycles_out = nullptr) {
    int guard = 0;
    while (!dut->s2m_valid) {
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT draining response\n"); exit(1); }
    }
    if (cycles_out) *cycles_out = guard;
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
    tfp->open("phase8_waveform.vcd");

    dut->rst_n = 0;
    dut->m2s_valid = 0;
    dut->s2m_ready = 1;
    dut->bias_wr_en = 0;
    dut->bias_addr = 0;
    dut->bias_wr_data = 0;
    dut->bi_req_ready = 0;
    dut->bi_rsp_valid = 0;
    dut->cache_bypass_en = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rst_n = 1;

    int errors = 0;
    const uint32_t X = 0x22;

    // --- Step 1: bypass OFF -- populate the cache normally (write then read
    // X), same as any Phase 5 access. Confirms the default (reset) state is
    // cache-enabled and behaves exactly as before this feature existed. ---
    fire(dut, tfp, true, X, 1, 0x11111111u);
    Resp r1 = drain_one(dut, tfp);
    uint32_t hits0 = dut->dbg_cache_hits, misses0 = dut->dbg_cache_misses;
    fire(dut, tfp, false, X, 2, 0);
    Resp r2 = drain_one(dut, tfp);
    if (r2.rdata != 0x11111111u || dut->dbg_cache_hits != hits0 + 1) {
        printf("FAIL: baseline (bypass off) cache population/hit didn't behave as Phase 5 already verified\n");
        errors++;
    } else {
        printf("PASS: baseline cache-enabled behavior intact (X cached, hit on read-back)\n");
    }

    // --- Step 2: enable bypass. Write NEW data to X. Must go straight to
    // backing memory, NOT into the cache (cache_data/tag/valid must stay
    // exactly as they were from step 1). ---
    dut->cache_bypass_en = 1;
    uint32_t misses1 = dut->dbg_cache_misses;
    fire(dut, tfp, true, X, 3, 0x22222222u);
    Resp r3 = drain_one(dut, tfp);
    if (r3.is_data || r3.tag != 3) { printf("FAIL: bypassed write response wrong\n"); errors++; }
    if (dut->dbg_cache_misses != misses1 + 1 || dut->dbg_cache_hits != hits0 + 1 /* unchanged from step 1 */) {
        printf("FAIL: bypassed write should count as a miss (hits frozen, misses+1); hits=%u misses %u->%u\n",
               (uint32_t)dut->dbg_cache_hits, misses1, (uint32_t)dut->dbg_cache_misses);
        errors++;
    } else {
        printf("PASS: bypassed write correctly counted as a miss, hit counter untouched\n");
    }

    // --- Step 3: still bypassed -- read X back. Must return the NEW value
    // (0x22222222, from backing memory), NOT the stale cached 0x11111111,
    // and must count as a miss (never a hit) since bypass forces
    // cache_hit_eff=0 unconditionally. Also confirm it pays real latency
    // (READ_EXTRA_LATENCY, not an instant same-cycle hit). ---
    uint32_t misses2 = dut->dbg_cache_misses;
    int cycles = 0;
    fire(dut, tfp, false, X, 4, 0);
    Resp r4 = drain_one(dut, tfp, &cycles);
    if (r4.rdata != 0x22222222u) {
        printf("FAIL: bypassed read returned 0x%08x, expected the freshly-bypass-written 0x22222222 "
               "(cache should never shadow backing memory while bypassed)\n", r4.rdata);
        errors++;
    } else if (dut->dbg_cache_misses != misses2 + 1) {
        printf("FAIL: bypassed read should count as a miss; misses %u -> %u\n", misses2, (uint32_t)dut->dbg_cache_misses);
        errors++;
    } else if (cycles < 2) {
        printf("FAIL: bypassed read completed in %d cycle(s) -- looks like it hit cache instead of paying "
               "READ_EXTRA_LATENCY\n", cycles);
        errors++;
    } else {
        printf("PASS: bypassed read returned fresh backing-memory data, correctly counted as a miss, "
               "paid real latency (%d cycles)\n", cycles);
    }

    // --- Step 4: disable bypass. Read X again. Because bypass NEVER touched
    // the cache arrays, the line is still exactly as step 1 left it
    // (valid, tag matches X, data=0x11111111) -- so this correctly HITS and
    // returns the OLD value, now stale relative to backing memory's
    // 0x22222222. This is the documented, intentional tradeoff, not a bug:
    // bypass measures "same access pattern, cache vs no cache", and mixing
    // bypassed and non-bypassed writes to the same address is a caller
    // responsibility outside that measurement's scope. ---
    dut->cache_bypass_en = 0;
    uint32_t hits1 = dut->dbg_cache_hits;
    fire(dut, tfp, false, X, 5, 0);
    Resp r5 = drain_one(dut, tfp);
    if (r5.rdata == 0x11111111u && dut->dbg_cache_hits == hits1 + 1) {
        printf("PASS: re-enabling bypass correctly exposes the untouched (now stale-vs-backing-memory) "
               "cached entry from before bypass was enabled -- confirms bypass truly never touched the cache\n");
    } else {
        printf("FAIL: expected the frozen pre-bypass cache entry (0x11111111, hit+1); got rdata=0x%08x hits %u->%u\n",
               r5.rdata, hits1, (uint32_t)dut->dbg_cache_hits);
        errors++;
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase8.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE8 PASS: cache bypass mode verified -- writes/reads go straight to backing memory, "
               "always miss, pay real latency, and the cache arrays stay untouched while bypassed\n");
        return 0;
    }
    printf("PHASE8 FAIL: %d errors\n", errors);
    return 1;
}
