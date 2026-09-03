// Phase 3 testbench: bias table read/write correctness, and a regression
// check that adding the bias table didn't disturb Phase 2's M2S/S2M
// datapath (out-of-order completion, tag routing) -- since Phase 3
// deliberately doesn't change datapath behavior yet (no cache to bypass
// until Phase 5), the bias table itself is what's new to verify here.
#include "Vcxl_mem_protocol_engine.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#if VM_COVERAGE
#include "verilated_cov.h"
#endif

#include <cstdint>
#include <cstdio>
#include <map>

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

static void bias_write(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp, uint32_t addr, bool host_biased) {
    dut->bias_addr = addr;
    dut->bias_wr_data = host_biased;
    dut->bias_wr_en = 1;
    tick(dut, tfp);
    dut->bias_wr_en = 0;
}

static bool bias_read(Vcxl_mem_protocol_engine *dut, uint32_t addr) {
    dut->bias_addr = addr;
    dut->eval();
    return dut->bias_rd_data;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_mem_protocol_engine;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase3_waveform.vcd");

    dut->rst_n = 0;
    dut->m2s_valid = 0;
    dut->s2m_ready = 0;
    dut->bias_wr_en = 0;
    dut->bias_addr = 0;
    dut->bias_wr_data = 0;
    dut->bi_req_ready = 0;
    dut->bi_rsp_valid = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rst_n = 1;

    int errors = 0;

    // --- Test 1: reset state is all Device-Biased (0). ---
    for (uint32_t a : {0x00u, 0x01u, 0x7Fu, 0xFFu}) {
        bool b = bias_read(dut, a);
        if (b) { printf("FAIL: addr 0x%02x expected Device-Biased at reset, got Host-Biased\n", a); errors++; }
    }

    // --- Test 2: bias write/read round trip, several addresses, mixed
    // Host/Device -- and confirm writing one address doesn't disturb
    // another (no aliasing bug in the table indexing). ---
    struct { uint32_t addr; bool host_biased; } settings[] = {
        {0x03, true}, {0x10, false}, {0x20, true}, {0xAA, true}, {0x50, false},
    };
    for (auto &s : settings) bias_write(dut, tfp, s.addr, s.host_biased);
    for (auto &s : settings) {
        bool got = bias_read(dut, s.addr);
        if (got != s.host_biased) {
            printf("FAIL: addr 0x%02x expected %s got %s\n", s.addr,
                   s.host_biased ? "Host-Biased" : "Device-Biased", got ? "Host-Biased" : "Device-Biased");
            errors++;
        }
    }
    // An address never touched should still read Device-Biased.
    if (bias_read(dut, 0x77)) {
        printf("FAIL: untouched addr 0x77 expected Device-Biased, got Host-Biased\n");
        errors++;
    }

    // --- Test 3: regression -- M2S read/write still works correctly for
    // Device-Biased lines (this confirms adding the bias table didn't
    // accidentally break anything Phase 2 already verified). Deliberately
    // uses only Device-Biased addresses (0x10, 0x50) -- accessing a
    // Host-Biased line now correctly triggers the BI sequencer as of
    // Phase 4, which needs a host-side BI responder this testbench was
    // never built with (Phase 3 predates Phase 4). An earlier draft of
    // this test used 0x20 here, which WAS marked Host-Biased above, and
    // hung waiting for a BIRsp that never came -- Phase 4's own testbench
    // (sim/phase4/) is where Host-Biased-line access is actually verified.
    //
    // 0x10 and 0x50 alias to the SAME cache index (CACHE_IDX_W=4 default),
    // so the second write forces an eviction of the first -- which takes
    // a few extra cycles (Phase 5). That extra delay is what newly exposed
    // a BUG-005-pattern bug here: holding s2m_ready high while firing BOTH
    // requests let the first write's response (fast, immediately eligible)
    // be silently consumed by the engine's ever-ready arbiter WHILE the
    // second fire() call's internal wait-for-ready loop was still running
    // -- before the dedicated draining loop below ever started watching.
    // Same fix as BUG-005: keep s2m_ready low while firing, only raise it
    // once actually draining.
    dut->s2m_ready = 0;

    fire(dut, tfp, true, 0x10, 1, 0x11112222u);
    fire(dut, tfp, true, 0x50, 2, 0x33334444u);

    std::map<uint32_t, uint32_t> got_rdata;
    std::map<uint32_t, bool> got_is_data;
    int seen = 0, guard = 0;
    dut->s2m_ready = 1; // now start draining
    while (seen < 2) {
        if (dut->s2m_valid) { seen++; }
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT draining writes\n"); return 1; }
    }

    dut->s2m_ready = 0; // same BUG-005-pattern precaution as above -- don't drain while still firing
    fire(dut, tfp, false, 0x10, 3, 0); // read back
    fire(dut, tfp, false, 0x50, 4, 0); // read back
    std::map<uint32_t, uint32_t> results;
    guard = 0;
    int need = 2;
    dut->s2m_ready = 1; // now start draining
    while (need > 0) {
        if (dut->s2m_valid) {
            results[dut->s2m_tag] = dut->s2m_rdata;
            need--;
        }
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT draining reads\n"); return 1; }
    }
    if (results[3] != 0x11112222u) {
        printf("FAIL: addr 0x10 read back 0x%08x, expected 0x11112222\n", results[3]);
        errors++;
    }
    if (results[4] != 0x33334444u) {
        printf("FAIL: addr 0x50 read back 0x%08x, expected 0x33334444\n", results[4]);
        errors++;
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase3.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE3 PASS: bias table read/write correctness verified, M2S/S2M datapath regression clean\n");
        return 0;
    }
    printf("PHASE3 FAIL: %d errors\n", errors);
    return 1;
}
