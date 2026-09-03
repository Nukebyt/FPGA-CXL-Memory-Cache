// Phase 4 testbench: the Back-Invalidate (BI) handshake -- the manual's
// hardest, highest-value module. Verifies (per manual Part 4.3 "Bias
// transition correctness"):
//   1. A request to a Host-Biased line triggers a BI request with the
//      correct address, held (m2s_ready low) until it resolves.
//   2. BIRsp atomically flips bias to Device-Biased, and the held request
//      then proceeds and completes correctly -- no local completion
//      before the bias flip.
//   3. The GLOBAL stall: an unrelated, already-Device-Biased request is
//      also blocked while a BI handshake is in flight (the deliberate
//      scoping choice documented in cxl_mem_protocol_engine.sv), and both
//      requests correctly proceed once the handshake resolves.
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

// Host-side BI responder model: call every cycle (after tick, before the
// next). Accepts a BI request one cycle after seeing it, then holds BIRsp
// for RSP_DELAY cycles before asserting it -- a nonzero delay so the
// handshake's multi-cycle nature is actually exercised, not accidentally
// collapsed to a single always-ready cycle.
struct HostBiModel {
    int state = 0; // 0=idle, 1=req_accepted_waiting_to_respond, 2=offering_rsp
    int delay_left = 0;
    static constexpr int RSP_DELAY = 4;

    void step(Vcxl_mem_protocol_engine *dut) {
        dut->bi_req_ready = 0;
        dut->bi_rsp_valid = 0;
        switch (state) {
            case 0:
                if (dut->bi_req_valid) {
                    dut->bi_req_ready = 1; // accept this cycle
                    state = 1;
                    delay_left = RSP_DELAY;
                }
                break;
            case 1:
                if (delay_left > 0) {
                    delay_left--;
                } else {
                    state = 2;
                }
                break;
            case 2:
                dut->bi_rsp_valid = 1;
                if (dut->bi_rsp_ready) state = 0; // consumed this cycle
                break;
        }
    }
};

static void fire(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp, HostBiModel &host,
                  bool is_write, uint32_t addr, uint32_t tag, uint32_t wdata, int max_cycles = 200) {
    dut->m2s_addr = addr;
    dut->m2s_tag = tag;
    dut->m2s_is_write = is_write;
    dut->m2s_wdata = wdata;
    dut->m2s_valid = 1;
    dut->eval();
    int guard = 0;
    while (!dut->m2s_ready) {
        host.step(dut);
        tick(dut, tfp);
        dut->eval();
        if (++guard > max_cycles) { fprintf(stderr, "TIMEOUT firing tag %u\n", tag); exit(1); }
    }
    host.step(dut);
    tick(dut, tfp);
    dut->m2s_valid = 0;
}

static void bias_write(Vcxl_mem_protocol_engine *dut, VerilatedVcdC *tfp, HostBiModel &host, uint32_t addr, bool host_biased) {
    dut->bias_addr = addr;
    dut->bias_wr_data = host_biased;
    dut->bias_wr_en = 1;
    host.step(dut);
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
    tfp->open("phase4_waveform.vcd");

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
    HostBiModel host;

    // --- Test 1: mark 0x10 Host-Biased, fire a read to it, verify the BI
    // handshake happens with the correct address, and the read completes
    // correctly (with the right data) only after BIRsp. ---
    bias_write(dut, tfp, host, 0x10, true);
    if (!bias_read(dut, 0x10)) { printf("FAIL: addr 0x10 should be Host-Biased after bias_write\n"); errors++; }

    dut->s2m_ready = 1;
    dut->m2s_addr = 0x10;
    dut->m2s_tag = 1;
    dut->m2s_is_write = 0;
    dut->m2s_wdata = 0;
    dut->m2s_valid = 1;
    dut->eval();

    // Before BI resolves, m2s_ready must stay low, and we should observe
    // bi_req_valid asserted with the correct address at some point.
    bool saw_bi_req_correct_addr = false;
    int guard = 0;
    while (!dut->m2s_ready) {
        if (dut->bi_req_valid && dut->bi_req_addr == 0x10) saw_bi_req_correct_addr = true;
        if (dut->s2m_valid) {
            printf("FAIL: S2M response arrived BEFORE the BI handshake resolved (no local completion should happen before the bias flip)\n");
            errors++;
        }
        host.step(dut);
        tick(dut, tfp);
        dut->eval();
        if (++guard > 200) { fprintf(stderr, "TIMEOUT: BI handshake for 0x10 never resolved\n"); return 1; }
    }
    if (!saw_bi_req_correct_addr) {
        printf("FAIL: never observed bi_req_valid with the correct address (0x10) during the handshake\n");
        errors++;
    } else {
        printf("BI request with correct address confirmed\n");
    }
    host.step(dut);
    tick(dut, tfp); // accept edge for the now-unblocked request
    dut->m2s_valid = 0;

    // Bias must now read Device-Biased (atomically flipped).
    if (bias_read(dut, 0x10)) {
        printf("FAIL: addr 0x10 should be Device-Biased after BIRsp, still reads Host-Biased\n");
        errors++;
    } else {
        printf("Atomic bias flip to Device-Biased confirmed\n");
    }

    // Drain the response; should be a correct DRS with rdata=0 (never written).
    guard = 0;
    while (!dut->s2m_valid) {
        host.step(dut);
        tick(dut, tfp);
        if (++guard > 50) { fprintf(stderr, "TIMEOUT waiting for the post-BI response\n"); return 1; }
    }
    if (dut->s2m_tag != 1 || !dut->s2m_is_data || dut->s2m_rdata != 0) {
        printf("FAIL: post-BI response wrong: tag=%u is_data=%d rdata=0x%08x (expected tag=1 is_data=1 rdata=0)\n",
               dut->s2m_tag, dut->s2m_is_data, dut->s2m_rdata);
        errors++;
    } else {
        printf("Post-BI read completed correctly\n");
    }
    host.step(dut);
    tick(dut, tfp); // consume it

    // --- Test 2: global stall -- while a BI handshake is in flight for one
    // address, an unrelated Device-Biased request must also be blocked. ---
    bias_write(dut, tfp, host, 0x30, true);  // 0x30 will trigger BI
    // 0x40 stays Device-Biased (default) -- should still be blocked while
    // 0x30's BI is in flight.

    dut->m2s_addr = 0x30;
    dut->m2s_tag = 2;
    dut->m2s_is_write = 0;
    dut->m2s_valid = 1;
    dut->eval();
    guard = 0;
    while (!(dut->bi_req_valid)) { // wait until the BI sequencer is actively mid-handshake
        host.step(dut);
        tick(dut, tfp);
        dut->eval();
        if (++guard > 50) { fprintf(stderr, "TIMEOUT: bi_req_valid never asserted for 0x30\n"); return 1; }
    }
    // Now, WHILE bi_req_valid is asserted (handshake clearly in flight),
    // check that a request to a DIFFERENT, Device-Biased address also
    // sees m2s_ready low -- this specific check is what proves the stall
    // is global, not per-address.
    dut->m2s_addr = 0x40; // different signals momentarily just to probe readiness;
    dut->m2s_tag = 3;      // the tag-30 request is still logically "in flight" via
    dut->eval();            // m2s_valid/m2s_addr held by the test driver, matching
                             // real bus discipline (host holds request stable while stalled).
    if (dut->m2s_ready) {
        printf("FAIL: m2s_ready was high for address 0x40 (Device-Biased) while a BI handshake for a "
               "DIFFERENT address (0x30) was in flight -- stall is not global as designed\n");
        errors++;
    } else {
        printf("Global stall confirmed: unrelated Device-Biased request blocked during in-flight BI\n");
    }
    // Restore the original tag-2/0x30 request and let it complete normally.
    dut->m2s_addr = 0x30;
    dut->m2s_tag = 2;
    dut->eval();
    guard = 0;
    while (!dut->m2s_ready) {
        host.step(dut);
        tick(dut, tfp);
        dut->eval();
        if (++guard > 100) { fprintf(stderr, "TIMEOUT: 0x30 request never got accepted after BI\n"); return 1; }
    }
    host.step(dut);
    tick(dut, tfp);
    dut->m2s_valid = 0;

    guard = 0;
    while (!dut->s2m_valid) {
        host.step(dut);
        tick(dut, tfp);
        if (++guard > 50) { fprintf(stderr, "TIMEOUT waiting response for tag 2\n"); return 1; }
    }
    if (dut->s2m_tag != 2) { printf("FAIL: expected tag 2 response, got tag %u\n", dut->s2m_tag); errors++; }
    host.step(dut);
    tick(dut, tfp);

    // Now confirm 0x40 (Device-Biased, never needed BI) can be accessed
    // normally once the stall lifts.
    fire(dut, tfp, host, false, 0x40, 4, 0);
    guard = 0;
    while (!dut->s2m_valid) { host.step(dut); tick(dut, tfp); if (++guard > 50) { fprintf(stderr, "TIMEOUT tag4\n"); return 1; } }
    if (dut->s2m_tag != 4) { printf("FAIL: expected tag 4 response after stall lifted, got tag %u\n", dut->s2m_tag); errors++; }
    else printf("Post-stall normal access (0x40) confirmed working\n");
    host.step(dut);
    tick(dut, tfp);

    // --- Test 3: a WRITE (not a read) triggers BI just as correctly. Every
    // BI test up to this point used a read to trigger the handshake --
    // `want_bi` in cxl_mem_protocol_engine.sv doesn't reference m2s_is_write
    // at all, so a write "should" behave identically by inspection, but this
    // project's own bug log (BUG-009/BUG-010) is a standing reminder that
    // "should, by inspection" isn't good enough on its own -- see
    // COVERAGE_REPORT.md, where this was found to be a genuine, previously
    // untested gap and fixed here rather than just written down. ---
    bias_write(dut, tfp, host, 0x60, true); // fresh address, Host-Biased
    fire(dut, tfp, host, true, 0x60, 5, 0xFEEDFACEu);
    guard = 0;
    while (!dut->s2m_valid) { host.step(dut); tick(dut, tfp); if (++guard > 50) { fprintf(stderr, "TIMEOUT tag5 (write-triggered BI)\n"); return 1; } }
    if (dut->s2m_is_data || dut->s2m_tag != 5) {
        printf("FAIL: write-triggered-BI response wrong (is_data=%d tag=%u, expected NDR tag=5)\n",
               dut->s2m_is_data, dut->s2m_tag);
        errors++;
    } else if (bias_read(dut, 0x60)) {
        printf("FAIL: 0x60 still Host-Biased after a write-triggered BI handshake\n");
        errors++;
    } else {
        printf("Write-triggered BI handshake confirmed: correct NDR response, bias flipped to Device-Biased\n");
    }
    host.step(dut);
    tick(dut, tfp);

    // Confirm the write's data actually landed (BI resolution didn't just
    // let the write's completion race ahead of the write itself).
    fire(dut, tfp, host, false, 0x60, 6, 0);
    guard = 0;
    while (!dut->s2m_valid) { host.step(dut); tick(dut, tfp); if (++guard > 50) { fprintf(stderr, "TIMEOUT tag6\n"); return 1; } }
    if (dut->s2m_rdata != 0xFEEDFACEu) {
        printf("FAIL: read-back after write-triggered BI got 0x%08x, expected 0xfeedface\n", dut->s2m_rdata);
        errors++;
    } else {
        printf("Write-triggered BI's data correctly read back\n");
    }
    host.step(dut);
    tick(dut, tfp);

    tfp->close();
#if VM_COVERAGE
    VerilatedCov::write("coverage_phase4.dat");
#endif
    delete dut;

    if (errors == 0) {
        printf("PHASE4 PASS: BI handshake, atomic bias flip, and global stall all verified\n");
        return 0;
    }
    printf("PHASE4 FAIL: %d errors\n", errors);
    return 1;
}
