// Avalon-MM slave wrapper around cxl_mem_protocol_engine, for hardware
// bring-up -- see FPGA_Implementation_Roadmap.md Phase 1H/2H/3H.
// Register-poke translation of the engine's valid/ready M2S/S2M interface,
// plus (Phase 3H) the bias table debug side-channel, into something the
// HPS can drive over the Lightweight HPS-to-FPGA bridge.
//
// Register map (word-addressed -- Platform Designer's addressUnits WORDS
// means avs_address is already a word index, not a byte address):
//   ADDR 0 (write) REQ_ADDR    : [ADDR_W-1:0] pending request address, latched
//   ADDR 1 (write) REQ_WDATA   : [DATA_W-1:0] pending write data, latched
//   ADDR 2 (write) REQ_FIRE    : [0]=is_write, [TAG_W+3:4]=tag -- ANY write
//                                here fires the request using the two
//                                latched registers above. Write stalls
//                                (avs_waitrequest) while the engine isn't
//                                idle -- same backpressure pattern as the
//                                Weibull-CFAR project's cfar_avalon_bridge
//                                pixel_ready/waitrequest handshake, so
//                                software can't fire a second request before
//                                the first is even accepted.
//   ADDR 3 (read)  STATUS      : bit0=s2m_valid (response ready to read),
//                                bit1=s2m_is_data (DRS vs NDR)
//   ADDR 4 (read)  RESP_TAG    : [TAG_W-1:0] tag of the ready response
//   ADDR 5 (read)  RESP_RDATA  : [DATA_W-1:0] read data of the ready response
//   ADDR 6 (write) RESP_ACK    : ANY write pulses s2m_ready for one cycle,
//                                consuming the response and returning the
//                                engine to IDLE.
//   ADDR 7 (write) BIAS_ADDR   : [ADDR_W-1:0] address for the next bias
//                                set/get, latched (same latch-then-trigger
//                                pattern as REQ_ADDR/REQ_FIRE).
//   ADDR 8 (write) BIAS_SET    : [0]=new bias value (0=Device-Biased,
//                                1=Host-Biased) -- ANY write here commits
//                                it at BIAS_ADDR.
//   ADDR 9 (read)  BIAS_GET    : bit0 = current bias value at BIAS_ADDR.
//   ADDR 10 (read) BI_STATUS   : bit0 = bi_req_valid (a BI request is
//                                pending, software must service it).
//   ADDR 11 (read) BI_REQ_ADDR : the address the device wants invalidated.
//   ADDR 12 (write) BI_REQ_ACK  : ANY write pulses bi_req_ready for one
//                                cycle -- software acknowledges receipt of
//                                the BI request (after reading BI_REQ_ADDR).
//   ADDR 13 (write) BI_RSP_SEND : ANY write pulses bi_rsp_valid for one
//                                cycle -- software signals its "invalidate
//                                / writeback" is done; completes the
//                                handshake and atomically flips bias.
//   ADDR 14 (read) CACHE_HITS  : free-running hit counter (Phase 5).
//   ADDR 15 (read) CACHE_MISSES: free-running miss counter (Phase 5) --
//                                needed for Phase 8's cache-enabled-vs-
//                                disabled latency experiment.
//   ADDR 16 (read/write) CACHE_BYPASS : bit0 -- write 1 to disable the
//                                cache (every access behaves exactly as it
//                                did pre-Phase-5: writes go straight to
//                                backing memory, reads always pay
//                                READ_EXTRA_LATENCY), write 0 to re-enable
//                                it. Cache contents are left untouched
//                                while bypassed, not cleared -- toggling
//                                this measures "cache vs no cache" for
//                                identical access patterns, not "warm
//                                cache vs cold cache". Read returns the
//                                current setting. (Phase 8.)
module cxl_avalon_shim #(
    parameter int ADDR_W = 8,
    parameter int DATA_W = 32,
    parameter int TAG_W  = 4
) (
    input  wire        clk,
    input  wire        rstn,

    input  wire [4:0]  avs_address,
    input  wire        avs_write,
    input  wire [31:0] avs_writedata,
    /* verilator lint_off UNUSEDSIGNAL */
    input  wire        avs_read,  // required by the Avalon-MM slave interface contract; reads here are always 0-wait-state with no side effect, so this qualifier isn't consumed
    /* verilator lint_on UNUSEDSIGNAL */
    output reg  [31:0] avs_readdata,
    output wire        avs_waitrequest,

    // Hardware bring-up debug conduit -- see cxl_de10_standard_top.v for
    // how these land on HEX0, same pattern as the Weibull-CFAR reference's
    // cfar_bridge_0_dbg_* signals.
    output wire         dbg_heartbeat,
    output wire         dbg_seen_request,
    output wire         dbg_seen_response
);

    localparam logic [4:0] ADDR_REQ_ADDR   = 5'd0;
    localparam logic [4:0] ADDR_REQ_WDATA  = 5'd1;
    localparam logic [4:0] ADDR_REQ_FIRE   = 5'd2;
    localparam logic [4:0] ADDR_STATUS     = 5'd3;
    localparam logic [4:0] ADDR_RESP_TAG   = 5'd4;
    localparam logic [4:0] ADDR_RESP_RDATA = 5'd5;
    localparam logic [4:0] ADDR_RESP_ACK   = 5'd6;
    localparam logic [4:0] ADDR_BIAS_ADDR  = 5'd7;
    localparam logic [4:0] ADDR_BIAS_SET   = 5'd8;
    localparam logic [4:0] ADDR_BIAS_GET   = 5'd9;
    localparam logic [4:0] ADDR_BI_STATUS  = 5'd10;
    localparam logic [4:0] ADDR_BI_REQ_ADDR= 5'd11;
    localparam logic [4:0] ADDR_BI_REQ_ACK = 5'd12;
    localparam logic [4:0] ADDR_BI_RSP_SEND= 5'd13;
    localparam logic [4:0] ADDR_CACHE_HITS = 5'd14;
    localparam logic [4:0] ADDR_CACHE_MISS = 5'd15;
    localparam logic [4:0] ADDR_CACHE_BYPASS = 5'd16;

    reg [ADDR_W-1:0] req_addr_r;
    reg [DATA_W-1:0] req_wdata_r;
    reg [ADDR_W-1:0] bias_addr_r;

    // One-deep "pending request" holding register, decoupling the Avalon
    // bus transaction (REQ_FIRE) from however long the engine actually
    // takes to accept it. This matters as of Phase 4: m2s_ready now also
    // depends on bias/BI state, which can only ever be resolved by
    // SOFTWARE issuing further Avalon transactions (BI_REQ_ACK,
    // BI_RSP_SEND) -- if REQ_FIRE's avs_waitrequest held the whole bus
    // hostage until the engine's m2s_ready went high (the original Phase
    // 1H design, `m2s_valid = write_fire && m2s_ready`), software could
    // NEVER get bus access again to service the very BI handshake that
    // m2s_ready is waiting on: a real deadlock, not just a testbench
    // issue, caught while writing the Phase 4H testbench. Now REQ_FIRE
    // only stalls if this ONE-DEEP register is still occupied by an
    // earlier unconsumed request; once latched here, the Avalon
    // transaction completes immediately and m2s_valid is asserted to the
    // engine independently, however long it takes the engine to actually
    // assert m2s_ready back.
    reg               pending_valid_r;
    reg               pending_is_write_r;
    reg [TAG_W-1:0]   pending_tag_r;

    wire               m2s_ready;
    wire               s2m_valid;
    wire               s2m_is_data;
    wire [TAG_W-1:0]   s2m_tag;
    wire [DATA_W-1:0]  s2m_rdata;
    wire               bias_rd_data;
    wire               bi_req_valid;
    wire [ADDR_W-1:0]  bi_req_addr;
    wire               bi_active;
    wire [31:0]        cache_hits;
    wire [31:0]        cache_misses;

    wire write_fire    = avs_write && (avs_address == ADDR_REQ_FIRE);
    wire write_ack     = avs_write && (avs_address == ADDR_RESP_ACK);
    wire write_bias    = avs_write && (avs_address == ADDR_BIAS_SET);
    wire write_bi_ack  = avs_write && (avs_address == ADDR_BI_REQ_ACK);
    wire write_bi_rsp  = avs_write && (avs_address == ADDR_BI_RSP_SEND);
    wire write_bypass  = avs_write && (avs_address == ADDR_CACHE_BYPASS);

    // Persistent setting, not a one-cycle pulse like the ack/fire registers
    // above -- stays whatever it was last set to until written again.
    reg cache_bypass_en_r;

    assign avs_waitrequest = write_fire && pending_valid_r;

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            req_addr_r  <= '0;
            req_wdata_r <= '0;
            bias_addr_r <= '0;
            cache_bypass_en_r <= 1'b0; // reset: cache enabled (matches cache_valid's own all-invalid reset)
        end else begin
            if (avs_write && avs_address == ADDR_REQ_ADDR)  req_addr_r  <= avs_writedata[ADDR_W-1:0];
            if (avs_write && avs_address == ADDR_REQ_WDATA) req_wdata_r <= avs_writedata[DATA_W-1:0];
            if (avs_write && avs_address == ADDR_BIAS_ADDR) bias_addr_r <= avs_writedata[ADDR_W-1:0];
            if (write_bypass) cache_bypass_en_r <= avs_writedata[0];
        end
    end

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            pending_valid_r <= 1'b0;
        end else begin
            if (write_fire && !pending_valid_r) begin
                pending_valid_r    <= 1'b1;
                pending_is_write_r <= avs_writedata[0];
                pending_tag_r      <= avs_writedata[TAG_W+3:4];
            end else if (pending_valid_r && m2s_ready) begin
                pending_valid_r <= 1'b0; // consumed by the engine
            end
        end
    end

    cxl_mem_protocol_engine #(
        .ADDR_W (ADDR_W),
        .DATA_W (DATA_W),
        .TAG_W  (TAG_W)
    ) engine (
        .clk          (clk),
        .rst_n        (rstn),

        .m2s_valid    (pending_valid_r),
        .m2s_ready    (m2s_ready),
        .m2s_is_write (pending_is_write_r),
        .m2s_addr     (req_addr_r),
        .m2s_tag      (pending_tag_r),
        .m2s_wdata    (req_wdata_r),

        .s2m_valid    (s2m_valid),
        .s2m_ready    (write_ack),
        .s2m_is_data  (s2m_is_data),
        .s2m_tag      (s2m_tag),
        .s2m_rdata    (s2m_rdata),

        .bias_wr_en   (write_bias),
        .bias_addr    (bias_addr_r),
        .bias_wr_data (avs_writedata[0]),
        .bias_rd_data (bias_rd_data),

        .bi_req_valid (bi_req_valid),
        .bi_req_ready (write_bi_ack),
        .bi_req_addr  (bi_req_addr),
        .bi_rsp_valid (write_bi_rsp),
        /* verilator lint_off PINCONNECTEMPTY */
        .bi_rsp_ready (), // informational only at the register interface -- software just fires BI_RSP_SEND once
        /* verilator lint_on PINCONNECTEMPTY */
        .bi_active    (bi_active),

        .cache_bypass_en  (cache_bypass_en_r),

        .dbg_cache_hits   (cache_hits),
        .dbg_cache_misses (cache_misses)
    );

    always @(*) begin
        case (avs_address)
            ADDR_STATUS:      avs_readdata = {30'b0, s2m_is_data, s2m_valid};
            ADDR_RESP_TAG:    avs_readdata = {{(32-TAG_W){1'b0}}, s2m_tag};
            ADDR_RESP_RDATA:  avs_readdata = s2m_rdata;
            ADDR_BIAS_GET:    avs_readdata = {31'b0, bias_rd_data};
            ADDR_BI_STATUS:   avs_readdata = {30'b0, bi_active, bi_req_valid};
            ADDR_BI_REQ_ADDR: avs_readdata = {{(32-ADDR_W){1'b0}}, bi_req_addr};
            ADDR_CACHE_HITS:  avs_readdata = cache_hits;
            ADDR_CACHE_MISS:  avs_readdata = cache_misses;
            ADDR_CACHE_BYPASS: avs_readdata = {31'b0, cache_bypass_en_r};
            default:          avs_readdata = 32'h0;
        endcase
    end

    // 26-bit counter's MSB: ~2.68s full period at 50MHz, slow enough to see
    // toggling on an LED/HEX segment with the naked eye -- proves clk/rstn
    // are alive independent of anything else in this module working.
    reg [25:0] hb_counter;
    always @(posedge clk or negedge rstn) begin
        if (!rstn) hb_counter <= 26'b0;
        else       hb_counter <= hb_counter + 1'b1;
    end
    assign dbg_heartbeat = hb_counter[25];

    reg seen_request_r, seen_response_r;
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            seen_request_r  <= 1'b0;
            seen_response_r <= 1'b0;
        end else begin
            if (pending_valid_r && m2s_ready) seen_request_r  <= 1'b1;
            if (s2m_valid)                seen_response_r <= 1'b1;
        end
    end
    assign dbg_seen_request  = seen_request_r;
    assign dbg_seen_response = seen_response_r;

endmodule
