// Phase 4H testbench: verifies the BI_STATUS/BI_REQ_ADDR/BI_REQ_ACK/
// BI_RSP_SEND register interface end to end -- this is what the HPS Linux
// driver will poll and drive for real in Phase 4H hardware bring-up, so
// getting the exact register sequence right here first is the point.
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
    tfp->open("phase4h_shim_waveform.vcd");

    dut->rstn = 0;
    dut->avs_write = 0;
    dut->avs_read = 0;
    for (int i = 0; i < 4; i++) tick(dut, tfp);
    dut->rstn = 1;

    int errors = 0;

    // Mark 0x25 Host-Biased via the existing bias registers.
    avs_write(dut, tfp, 7 /*BIAS_ADDR*/, 0x25);
    avs_write(dut, tfp, 8 /*BIAS_SET*/, 1);

    // Fire a read to 0x25. With the pending-request holding register fix
    // (see cxl_avalon_shim.sv), REQ_FIRE now completes immediately on the
    // Avalon bus regardless of how long the engine takes to actually
    // accept it -- so, unlike an earlier draft of this test, we do NOT
    // need to hand-hold avs_write/avs_address across the wait; a normal
    // blocking avs_write() call is enough, and the bus is free again
    // immediately afterward to poll BI_STATUS with ordinary transactions.
    avs_write(dut, tfp, 0 /*REQ_ADDR*/, 0x25);
    avs_write(dut, tfp, 2 /*REQ_FIRE*/, (9u << 4) | 0x0u); // tag=9, is_write=0

    // Poll BI_STATUS until a BI request is pending.
    int guard = 0;
    while (!(avs_read(dut, tfp, 10 /*BI_STATUS*/) & 0x1u)) {
        tick(dut, tfp);
        if (++guard > 200) { fprintf(stderr, "TIMEOUT waiting for BI_STATUS pending\n"); return 1; }
    }
    uint32_t bi_addr = avs_read(dut, tfp, 11 /*BI_REQ_ADDR*/);
    if (bi_addr != 0x25) { printf("FAIL: BI_REQ_ADDR expected 0x25 got 0x%02x\n", bi_addr); errors++; }
    else printf("BI request correctly surfaced via registers: addr=0x%02x\n", bi_addr);

    avs_write(dut, tfp, 12 /*BI_REQ_ACK*/, 1);   // acknowledge receipt
    avs_write(dut, tfp, 13 /*BI_RSP_SEND*/, 1);  // "invalidation done", send BIRsp

    // Bias should now read Device-Biased.
    avs_write(dut, tfp, 7, 0x25);
    uint32_t bias_now = avs_read(dut, tfp, 9 /*BIAS_GET*/);
    if (bias_now & 1u) { printf("FAIL: 0x25 still Host-Biased after BI_RSP_SEND\n"); errors++; }
    else printf("Bias correctly flipped to Device-Biased after BI_RSP_SEND\n");

    uint32_t status;
    guard = 0;
    do {
        status = avs_read(dut, tfp, 3 /*STATUS*/);
        if (++guard > 100) { fprintf(stderr, "TIMEOUT waiting post-BI response\n"); return 1; }
    } while (!(status & 0x1));
    uint32_t tag = avs_read(dut, tfp, 4 /*RESP_TAG*/);
    if (tag != 9) { printf("FAIL: expected tag 9 response after BI, got %u\n", tag); errors++; }
    else printf("Post-BI response correctly delivered via registers\n");
    avs_write(dut, tfp, 6 /*RESP_ACK*/, 1);

    tfp->close();
    delete dut;

    if (errors == 0) {
        printf("PHASE4H_SHIM PASS: BI register interface verified end to end\n");
        return 0;
    }
    printf("PHASE4H_SHIM FAIL: %d errors\n", errors);
    return 1;
}
