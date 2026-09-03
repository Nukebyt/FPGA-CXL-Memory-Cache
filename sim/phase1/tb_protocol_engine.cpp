// Phase 1 self-checking testbench for cxl_mem_protocol_engine.
// Not a UVM/class-based environment (Verilator doesn't run those) -- see
// FPGA_Implementation_Roadmap.md "Verification tooling" table for the mapping
// from the manual's UVM environment to this plain-C++ approach.
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

struct Result {
    bool is_data;
    uint32_t tag;
    uint32_t rdata;
};

static Result do_transaction(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp,
                              bool is_write, uint32_t addr, uint32_t tag,
                              uint32_t wdata) {
    dut->m2s_valid = 1;
    dut->m2s_is_write = is_write;
    dut->m2s_addr = addr;
    dut->m2s_tag = tag;
    dut->m2s_wdata = wdata;
    dut->s2m_ready = 1;
    dut->eval();  // settle m2s_ready for the NEW request's tag/addr before checking it -- without
                  // this, the first check below reads a stale value left over from the previous
                  // transaction's tag, which happened to coincidentally match in every earlier
                  // phase (readiness never actually varied) but not once Phase 5's cache eviction
                  // made readiness genuinely depend on the just-set address. Same bug family as
                  // BUG-002 (Avalon waitrequest sampling) and BUG-005 (s2m_ready always-high) --
                  // a testbench reading a handshake signal before settling it for new inputs.

    int guard = 0;
    while (!dut->m2s_ready) {
        tick(dut, tfp);
        if (++guard > 100) {
            fprintf(stderr, "TIMEOUT waiting for m2s_ready\n");
            exit(1);
        }
    }
    tick(dut, tfp);  // request-accept edge
    dut->m2s_valid = 0;

    guard = 0;
    while (!dut->s2m_valid) {
        tick(dut, tfp);
        if (++guard > 100) {
            fprintf(stderr, "TIMEOUT waiting for s2m_valid\n");
            exit(1);
        }
    }
    Result r{static_cast<bool>(dut->s2m_is_data), dut->s2m_tag, dut->s2m_rdata};
    tick(dut, tfp);  // response-consume edge
    return r;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_mem_protocol_engine;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase1_waveform.vcd");

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
    std::map<uint32_t, uint32_t> expected_mem;

    struct WV { uint32_t addr, tag, data; };
    std::vector<WV> writes = {
        {0x00, 1, 0xDEADBEEFu},
        {0x01, 2, 0x12345678u},
        {0xFF, 3, 0xCAFEF00Du},
        {0x10, 4, 0x00000000u},
    };
    for (auto &w : writes) {
        Result r = do_transaction(dut, tfp, true, w.addr, w.tag, w.data);
        expected_mem[w.addr] = w.data;
        if (r.is_data) {
            printf("FAIL: write to addr 0x%02x returned is_data=1 (expected NDR)\n", w.addr);
            errors++;
        }
        if (r.tag != w.tag) {
            printf("FAIL: write tag mismatch addr 0x%02x expected %u got %u\n", w.addr, w.tag, r.tag);
            errors++;
        }
    }

    struct RV { uint32_t addr, tag; };
    std::vector<RV> reads = {
        {0x00, 5}, {0x01, 6}, {0xFF, 7}, {0x10, 8}, {0x20, 9},  // 0x20 never written -> expect reset value 0
    };
    for (auto &rr : reads) {
        Result r = do_transaction(dut, tfp, false, rr.addr, rr.tag, 0);
        uint32_t exp = expected_mem.count(rr.addr) ? expected_mem[rr.addr] : 0u;
        if (!r.is_data) {
            printf("FAIL: read from addr 0x%02x returned is_data=0 (expected DRS)\n", rr.addr);
            errors++;
        }
        if (r.tag != rr.tag) {
            printf("FAIL: read tag mismatch addr 0x%02x expected %u got %u\n", rr.addr, rr.tag, r.tag);
            errors++;
        }
        if (r.rdata != exp) {
            printf("FAIL: read data mismatch addr 0x%02x expected 0x%08x got 0x%08x\n", rr.addr, exp, r.rdata);
            errors++;
        }
    }

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase1.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE1 PASS: %zu writes, %zu reads, 0 errors\n", writes.size(), reads.size());
        return 0;
    }
    printf("PHASE1 FAIL: %d errors\n", errors);
    return 1;
}
