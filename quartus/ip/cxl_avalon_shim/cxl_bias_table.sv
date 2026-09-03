// Phase 3/4: per-line bias table (Host-Biased / Device-Biased), one bit per
// address. Reset state: all Device-Biased -- "the fast path most accesses
// should end up on," per the manual's own framing of why bias exists
// (Part 1.4).
//
// Two independent read ports (genuine dual-port M10K capability on Cyclone
// V, not a shared/muxed port): port A is the Avalon debug read/write
// side-channel (Phase 3H); port B is a read-only query port the BI
// sequencer (Phase 4) uses to check the CURRENT M2S request's address
// every cycle, independent of whatever the debug port happens to be doing.
//
// A single write port, since only one write source should ever act in a
// given cycle: the CALLER (cxl_mem_protocol_engine) arbitrates between the
// debug write and the BI sequencer's own bias flip and presents exactly
// one to wr_en/wr_addr/wr_data -- this table doesn't need to know which.
module cxl_bias_table #(
    parameter int ADDR_W = 8
) (
    input  logic              clk,
    input  logic              rst_n,      // BUG-009: the `initial` block below is sim-only --
                                           // Cyclone V gives no guarantee an inferred array's
                                           // power-on/reset content matches it, so an address
                                           // never explicitly bias_write'd could read back
                                           // Host-Biased by chance on real hardware, silently
                                           // triggering a real BI request with no responder
                                           // ready for it. A real synchronous reset is required.

    input  logic               wr_en,
    input  logic [ADDR_W-1:0]  wr_addr,
    input  logic               wr_data,        // 0 = Device-Biased, 1 = Host-Biased

    input  logic [ADDR_W-1:0]  rd_addr_a,      // debug port (Avalon shim)
    output logic               rd_data_a,

    input  logic [ADDR_W-1:0]  rd_addr_b,      // query port (BI-gating logic)
    output logic               rd_data_b
);

    // Flat packed vector, not an unpacked array -- at 256 bits (ADDR_W=8) this was
    // never going to map to block RAM anyway, and a packed vector gets a real
    // single-cycle synchronous reset (`bias <= '0`) for free, with no sweep and no
    // window where a real wr_en could collide with in-progress reset clearing.
    // An earlier version of this fix used a multi-cycle sweep counter instead --
    // functionally fine given real access timescales, but it could silently drop
    // (or, in a refined form, permanently strand) a write that happened to land
    // during the sweep, which is exactly the kind of bug this fix exists to avoid
    // introducing. This form has no such window.
    logic [(1<<ADDR_W)-1:0] bias;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)      bias <= '0; // all Device-Biased
        else if (wr_en)  bias[wr_addr] <= wr_data;
    end

    assign rd_data_a = bias[rd_addr_a];
    assign rd_data_b = bias[rd_addr_b];

endmodule
