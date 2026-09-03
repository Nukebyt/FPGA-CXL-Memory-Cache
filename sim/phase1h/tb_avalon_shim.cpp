// Sim smoke test for cxl_avalon_shim's register interface -- exercises the
// same write/write/fire/poll/read/ack sequence the bare-metal HPS driver
// will use over the real Avalon-MM bridge, before ever touching hardware.
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
    dut->eval();  // settle avs_waitrequest for the current cycle before checking it
    int guard = 0;
    while (dut->avs_waitrequest) {
        tick(dut, tfp);
        dut->eval();
        if (++guard > 100) { fprintf(stderr, "TIMEOUT on avs_write addr=%u\n", addr); exit(1); }
    }
    tick(dut, tfp);  // the edge where the transfer actually completes (waitrequest was low)
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
    tfp->open("phase1h_shim_waveform.vcd");

    dut->rstn = 0;
    dut->avs_write = 0;
    dut->avs_read = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rstn = 1;

    int errors = 0;

    // Write 0xABCD1234 to address 0x05, tag 0x3.
    avs_write(dut, tfp, 0 /*REQ_ADDR*/, 0x05);
    avs_write(dut, tfp, 1 /*REQ_WDATA*/, 0xABCD1234u);
    avs_write(dut, tfp, 2 /*REQ_FIRE*/, (0x3 << 4) | 0x1 /*is_write=1, tag=3*/);

    int guard = 0;
    uint32_t status;
    do {
        status = avs_read(dut, tfp, 3 /*STATUS*/);
        if (++guard > 100) { fprintf(stderr, "TIMEOUT waiting write completion\n"); return 1; }
    } while (!(status & 0x1));

    if (status & 0x2) { printf("FAIL: write completion has is_data=1 (expected NDR)\n"); errors++; }
    uint32_t tag = avs_read(dut, tfp, 4 /*RESP_TAG*/);
    if (tag != 0x3) { printf("FAIL: write-ack tag mismatch, expected 3 got %u\n", tag); errors++; }
    avs_write(dut, tfp, 6 /*RESP_ACK*/, 1);

    // Read address 0x05 back, tag 0x7, expect 0xABCD1234.
    avs_write(dut, tfp, 0 /*REQ_ADDR*/, 0x05);
    avs_write(dut, tfp, 2 /*REQ_FIRE*/, (0x7 << 4) | 0x0 /*is_write=0, tag=7*/);

    guard = 0;
    do {
        status = avs_read(dut, tfp, 3 /*STATUS*/);
        if (++guard > 100) { fprintf(stderr, "TIMEOUT waiting read completion\n"); return 1; }
    } while (!(status & 0x1));

    if (!(status & 0x2)) { printf("FAIL: read completion has is_data=0 (expected DRS)\n"); errors++; }
    tag = avs_read(dut, tfp, 4 /*RESP_TAG*/);
    if (tag != 0x7) { printf("FAIL: read-response tag mismatch, expected 7 got %u\n", tag); errors++; }
    uint32_t rdata = avs_read(dut, tfp, 5 /*RESP_RDATA*/);
    if (rdata != 0xABCD1234u) { printf("FAIL: read data mismatch, expected 0xabcd1234 got 0x%08x\n", rdata); errors++; }
    avs_write(dut, tfp, 6 /*RESP_ACK*/, 1);

    tfp->close();
    delete dut;

    if (errors == 0) {
        printf("PHASE1H_SHIM PASS: register-interface write+read round trip verified\n");
        return 0;
    }
    printf("PHASE1H_SHIM FAIL: %d errors\n", errors);
    return 1;
}
