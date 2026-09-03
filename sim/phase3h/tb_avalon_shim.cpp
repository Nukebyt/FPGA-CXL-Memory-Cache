// Phase 3H testbench: verifies the new BIAS_ADDR/BIAS_SET/BIAS_GET register
// interface, plus a regression check that the existing REQ/RESP registers
// (now at the same addresses, just with a wider avs_address port) still
// work after the address-width change from 3 to 4 bits.
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

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    auto *dut = new Vcxl_avalon_shim;

    Verilated::traceEverOn(true);
    auto *tfp = new VerilatedVcdC;
    dut->trace(tfp, 99);
    tfp->open("phase3h_shim_waveform.vcd");

    dut->rstn = 0;
    dut->avs_write = 0;
    dut->avs_read = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rstn = 1;

    int errors = 0;

    // --- Bias register test: set a few addresses, read back. ---
    struct { uint32_t addr, value; } bias_settings[] = {
        {0x03, 1}, {0x10, 0}, {0x20, 1},
    };
    for (auto &s : bias_settings) {
        avs_write(dut, tfp, 7 /*BIAS_ADDR*/, s.addr);
        avs_write(dut, tfp, 8 /*BIAS_SET*/, s.value);
    }
    for (auto &s : bias_settings) {
        avs_write(dut, tfp, 7 /*BIAS_ADDR*/, s.addr);
        uint32_t got = avs_read(dut, tfp, 9 /*BIAS_GET*/);
        if ((got & 1u) != s.value) {
            printf("FAIL: bias addr 0x%02x expected %u got %u\n", s.addr, s.value, got & 1u);
            errors++;
        }
    }
    // Untouched address should read Device-Biased (0).
    avs_write(dut, tfp, 7, 0x55);
    uint32_t untouched = avs_read(dut, tfp, 9);
    if ((untouched & 1u) != 0) {
        printf("FAIL: untouched bias addr 0x55 expected 0, got %u\n", untouched & 1u);
        errors++;
    }

    // --- Regression: REQ/RESP register interface still works with the
    // wider (4-bit) avs_address port. ---
    avs_write(dut, tfp, 0 /*REQ_ADDR*/, 0x05);
    avs_write(dut, tfp, 1 /*REQ_WDATA*/, 0x99887766u);
    avs_write(dut, tfp, 2 /*REQ_FIRE*/, (0x3 << 4) | 0x1);

    int guard = 0;
    uint32_t status;
    do {
        status = avs_read(dut, tfp, 3 /*STATUS*/);
        if (++guard > 100) { fprintf(stderr, "TIMEOUT waiting write completion\n"); return 1; }
    } while (!(status & 0x1));
    avs_write(dut, tfp, 6 /*RESP_ACK*/, 1);

    avs_write(dut, tfp, 0, 0x05);
    avs_write(dut, tfp, 2, (0x7 << 4) | 0x0);
    guard = 0;
    do {
        status = avs_read(dut, tfp, 3);
        if (++guard > 100) { fprintf(stderr, "TIMEOUT waiting read completion\n"); return 1; }
    } while (!(status & 0x1));
    uint32_t rdata = avs_read(dut, tfp, 5 /*RESP_RDATA*/);
    if (rdata != 0x99887766u) {
        printf("FAIL: REQ/RESP regression check, expected 0x99887766 got 0x%08x\n", rdata);
        errors++;
    }
    avs_write(dut, tfp, 6, 1);

    tfp->close();
    delete dut;

    if (errors == 0) {
        printf("PHASE3H_SHIM PASS: bias register interface verified, REQ/RESP regression clean\n");
        return 0;
    }
    printf("PHASE3H_SHIM FAIL: %d errors\n", errors);
    return 1;
}
