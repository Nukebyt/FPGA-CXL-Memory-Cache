// Phase 8H testbench: verifies the CACHE_BYPASS register (ADDR 16) end to
// end through the Avalon shim -- write/read round trip, and that setting it
// actually changes REQUEST behavior (forces a miss + real latency on an
// address that would otherwise hit), exactly mirroring what the HPS Linux
// driver will do for the cache-on-vs-off latency experiment.
#include "Vcxl_avalon_shim.h"
#include "verilated.h"
#include "verilated_vcd_c.h"

#include <cstdint>
#include <cstdio>

static vluint64_t sim_time = 0;

static void tick(Vcxl_avalon_shim *dut, VerilatedVcdC *tfp) {
    dut->clk = 0;
    dut->eval();
    if (tfp) tfp->dump(sim_time++);
    dut->clk = 1;
    dut->eval();
    if (tfp) tfp->dump(sim_time++);
}

static void avs_write(Vcxl_avalon_shim *dut, VerilatedVcdC *tfp, uint32_t addr, uint32_t data) {
    dut->avs_address = addr;
    dut->avs_writedata = data;
    dut->avs_write = 1;
    dut->avs_read = 0;
    dut->eval();
    int guard = 0;
    while (dut->avs_waitrequest) {
        tick(dut, tfp);
        dut->eval();
        if (++guard > 100) { fprintf(stderr, "TIMEOUT on avs_write addr=%u\n", addr); exit(1); }
    }
    tick(dut, tfp);
    dut->avs_write = 0;
}

static uint32_t avs_read(Vcxl_avalon_shim *dut, VerilatedVcdC *tfp, uint32_t addr) {
    dut->avs_address = addr;
    dut->avs_read = 1;
    dut->avs_write = 0;
    dut->eval();
    uint32_t data = dut->avs_readdata;
    tick(dut, tfp);
    dut->avs_read = 0;
    return data;
}

static void fire_and_drain(Vcxl_avalon_shim *dut, VerilatedVcdC *tfp,
                            bool is_write, uint32_t addr, uint32_t tag, uint32_t wdata) {
    avs_write(dut, tfp, 0 /*REQ_ADDR*/, addr);
    avs_write(dut, tfp, 1 /*REQ_WDATA*/, wdata);
    avs_write(dut, tfp, 2 /*REQ_FIRE*/, (tag << 4) | (is_write ? 1u : 0u));
    int guard = 0;
    while (!(avs_read(dut, tfp, 3 /*STATUS*/) & 0x1u)) {
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT waiting response\n"); exit(1); }
    }
    avs_write(dut, tfp, 6 /*RESP_ACK*/, 1);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_avalon_shim;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase8_shim_waveform.vcd");

    dut->rstn = 0;
    dut->avs_write = 0;
    dut->avs_read = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rstn = 1;

    int errors = 0;

    // --- Register round trip: reset default must be 0 (cache enabled). ---
    uint32_t initial = avs_read(dut, tfp, 16 /*CACHE_BYPASS*/);
    if (initial != 0) { printf("FAIL: CACHE_BYPASS should reset to 0 (enabled), got %u\n", initial); errors++; }
    else printf("PASS: CACHE_BYPASS correctly resets to 0 (cache enabled)\n");

    avs_write(dut, tfp, 16, 1);
    uint32_t after_set = avs_read(dut, tfp, 16);
    if (after_set != 1) { printf("FAIL: CACHE_BYPASS write/read round trip failed, got %u\n", after_set); errors++; }
    else printf("PASS: CACHE_BYPASS write(1)/read round trip correct\n");

    avs_write(dut, tfp, 16, 0);
    uint32_t after_clear = avs_read(dut, tfp, 16);
    if (after_clear != 0) { printf("FAIL: CACHE_BYPASS should read 0 after write(0), got %u\n", after_clear); errors++; }
    else printf("PASS: CACHE_BYPASS write(0)/read round trip correct\n");

    // --- Functional check: populate a line normally, then confirm setting
    // CACHE_BYPASS actually forces a miss on what would otherwise hit. ---
    const uint32_t X = 0x33;
    fire_and_drain(dut, tfp, true, X, 1, 0x77770001u);
    uint32_t hits_before = avs_read(dut, tfp, 14 /*CACHE_HITS*/);
    fire_and_drain(dut, tfp, false, X, 2, 0);
    uint32_t hits_after = avs_read(dut, tfp, 14);
    if (hits_after != hits_before + 1) {
        printf("FAIL: expected a cache hit on read-back before enabling bypass (sanity check)\n");
        errors++;
    }

    avs_write(dut, tfp, 16, 1); // enable bypass via the register interface
    uint32_t misses_before = avs_read(dut, tfp, 15 /*CACHE_MISSES*/);
    uint32_t hits_before2 = avs_read(dut, tfp, 14);
    fire_and_drain(dut, tfp, false, X, 3, 0);
    uint32_t misses_after = avs_read(dut, tfp, 15);
    uint32_t hits_after2 = avs_read(dut, tfp, 14);
    if (misses_after == misses_before + 1 && hits_after2 == hits_before2) {
        printf("PASS: CACHE_BYPASS register correctly forces a miss on an address that would otherwise hit\n");
    } else {
        printf("FAIL: expected miss+1/hit+0 with bypass enabled via register, got hit %u->%u miss %u->%u\n",
               hits_before2, hits_after2, misses_before, misses_after);
        errors++;
    }
    avs_write(dut, tfp, 16, 0); // restore, tidy

    tfp->close();
    delete dut;

    if (errors == 0) {
        printf("PHASE8H_SHIM PASS: CACHE_BYPASS register verified end to end\n");
        return 0;
    }
    printf("PHASE8H_SHIM FAIL: %d errors\n", errors);
    return 1;
}
