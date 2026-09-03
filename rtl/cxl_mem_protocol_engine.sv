// Phase 2/3/4/5: CXL.mem protocol engine -- tag-indexed outstanding-
// transaction tracking (Phase 2), a bias table (Phase 3), the
// Back-Invalidate (BI) sequencer (Phase 4), and now (Phase 5) a
// direct-mapped, write-back memory-side cache sitting behind bias gating.
//
// ---- Phase 5: the cache -----------------------------------------------
// Direct-mapped, one word per line (matches this project's word-addressed
// model -- no sub-line masking needed), write-back + write-allocate.
// Deliberately simple per the manual's own guidance ("don't over-engineer
// this part -- it's not where the interview signal is").
//
// A crucial invariant falls out of the phase ordering for free: by the
// time a request reaches the cache-allocate logic below, Phase 4's bias
// gating has ALREADY forced it to be Device-Biased (m2s_ready is held low
// for a Host-Biased address until BI resolves it) -- so the cache never
// needs its own bias check. "Never cache a Host-Biased line" is enforced
// structurally by construction, one phase upstream, not re-checked here.
//
// Eviction: a MISS that lands on a currently-valid, dirty, different-tag
// line needs its old contents written back to backing memory before the
// new line can be allocated -- and with a single-ported backing-memory
// array, that writeback and the eventual fill read cannot happen on the
// same cycle. Handled with a small FLUSH_IDLE/FLUSH_ACTIVE FSM gating
// m2s_ready, directly reusing the exact same pattern Phase 4's BI
// sequencer already established: hold m2s_ready low, let the host hold
// m2s_valid/m2s_addr stable while stalled, resolve internally, then let
// the same still-pending request proceed through the normal accept path
// once resolved. Two independent instances of "stall, don't drop" in this
// module now, both built the same way on purpose.
//
// ---- Phase 4: the BI handshake (unchanged from Phase 4) ---------------
// Real CXL.mem's BI request travels device->host and BIRsp travels
// host->device (manual Part 1.2/1.4) -- modeled here as their own
// dedicated valid/ready channel pair (bi_req_*/bi_rsp_*). Concurrency:
// while a BI handshake is in flight, m2s_ready is held low GLOBALLY, a
// deliberate conservative scoping choice (see git history / bugs.md for
// the reasoning) -- ruled out an entire race class rather than handling
// it, on purpose.
//
// ---- Phase 2/3 notes (unchanged) ---------------------------------------
// One slot per possible tag value (2**TAG_W slots) for outstanding-
// transaction tracking (AXI4 ID-routing pattern, manual Part 1.6). All
// per-slot state driven from a single always_ff for-loop -- see bugs.md
// BUG-004 for why that discipline matters. Out-of-order completion comes
// from response-side latency: writes are always fast (0 extra latency);
// reads pay READ_EXTRA_LATENCY on a cache MISS, but a cache HIT is now
// also fast (0 extra latency) -- the actual latency benefit Phase 8's
// cache-on-vs-off experiment will measure.
module cxl_mem_protocol_engine #(
    parameter int ADDR_W = 8,
    parameter int DATA_W = 32,
    parameter int TAG_W  = 4,
    parameter int READ_EXTRA_LATENCY = 3,
    parameter int CACHE_IDX_W = 4   // 2**CACHE_IDX_W direct-mapped cache lines
) (
    input  logic              clk,
    input  logic              rst_n,

    // M2S request (host -> device)
    input  logic               m2s_valid,
    output logic               m2s_ready,
    input  logic               m2s_is_write,
    input  logic [ADDR_W-1:0]  m2s_addr,
    input  logic [TAG_W-1:0]   m2s_tag,
    input  logic [DATA_W-1:0]  m2s_wdata,

    // S2M response (device -> host)
    output logic               s2m_valid,
    input  logic                s2m_ready,
    output logic               s2m_is_data,   // 0 = NDR (write ack), 1 = DRS (read data)
    output logic [TAG_W-1:0]   s2m_tag,
    output logic [DATA_W-1:0]  s2m_rdata,

    // Bias table debug side-channel (Phase 3) -- see cxl_bias_table.sv
    input  logic               bias_wr_en,
    input  logic [ADDR_W-1:0]  bias_addr,
    input  logic               bias_wr_data,
    output logic               bias_rd_data,

    // BI request (device -> host) / BI response (host -> device) -- Phase 4
    output logic               bi_req_valid,
    input  logic                bi_req_ready,
    output logic [ADDR_W-1:0]  bi_req_addr,
    input  logic                bi_rsp_valid,
    output logic               bi_rsp_ready,

    // Debug: is a BI handshake currently in flight
    output logic               bi_active,

    // Cache bypass (Phase 8): when asserted, every access behaves exactly
    // as it did pre-Phase-5 -- reads always pay READ_EXTRA_LATENCY and are
    // served from mem[], writes go straight to mem[] instead of into the
    // cache. The cache arrays are left completely untouched (frozen, not
    // cleared) while bypassed, so disabling bypass later resumes with
    // whatever was cached before -- this measures "cache vs no cache" for
    // the SAME access pattern, not "cold cache vs warm cache".
    input  logic                cache_bypass_en,

    // Debug: cache hit/miss counters (Phase 5) -- free-running, read-only;
    // useful on their own for hardware bring-up and directly needed for
    // Phase 8's cache-enabled-vs-disabled latency experiment. During bypass,
    // every access correctly counts as a miss (cache_hit_eff below is
    // unconditionally 0), which is accurate bookkeeping, not a special case.
    output logic [31:0]        dbg_cache_hits,
    output logic [31:0]        dbg_cache_misses
);

    // ---- Bias table: debug port (A) + BI-gating query port (B) --------
    logic [ADDR_W-1:0] bias_int_wr_addr;
    logic              bias_int_wr_en;
    logic              bias_int_wr_data;
    logic              bias_query_rd_data;

    cxl_bias_table #(.ADDR_W(ADDR_W)) bias_table (
        .clk        (clk),
        .rst_n      (rst_n),
        .wr_en      (bias_int_wr_en),
        .wr_addr    (bias_int_wr_addr),
        .wr_data    (bias_int_wr_data),
        .rd_addr_a  (bias_addr),
        .rd_data_a  (bias_rd_data),
        .rd_addr_b  (m2s_addr),
        .rd_data_b  (bias_query_rd_data)
    );

    // ---- BI sequencer FSM (Phase 4, unchanged) ----------------------------
    typedef enum logic [1:0] { BI_IDLE, BI_REQ, BI_WAIT_RSP } bi_state_t;
    bi_state_t bi_state;

    wire want_bi = (bi_state == BI_IDLE) && m2s_valid && bias_query_rd_data;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            bi_state <= BI_IDLE;
        end else begin
            case (bi_state)
                BI_IDLE:     if (want_bi) bi_state <= BI_REQ;
                BI_REQ:      if (bi_req_valid && bi_req_ready) bi_state <= BI_WAIT_RSP;
                BI_WAIT_RSP: if (bi_rsp_valid) bi_state <= BI_IDLE;
                default:     bi_state <= BI_IDLE;
            endcase
        end
    end

    assign bi_active    = (bi_state != BI_IDLE);
    assign bi_req_valid = (bi_state == BI_REQ);
    assign bi_req_addr  = m2s_addr;
    assign bi_rsp_ready = (bi_state == BI_WAIT_RSP);

    wire bi_flip_now = (bi_state == BI_WAIT_RSP) && bi_rsp_valid;
    assign bias_int_wr_en   = bi_flip_now ? 1'b1        : bias_wr_en;
    assign bias_int_wr_addr = bi_flip_now ? m2s_addr    : bias_addr;
    assign bias_int_wr_data = bi_flip_now ? 1'b0        : bias_wr_data;

    // ---- Cache arrays (Phase 5) -------------------------------------------
    localparam int NUM_CACHE_LINES = 1 << CACHE_IDX_W;
    localparam int CACHE_TAG_W     = ADDR_W - CACHE_IDX_W;

    logic                    cache_valid [0:NUM_CACHE_LINES-1];
    logic                    cache_dirty [0:NUM_CACHE_LINES-1];
    logic [CACHE_TAG_W-1:0]  cache_tag   [0:NUM_CACHE_LINES-1];
    logic [DATA_W-1:0]       cache_data  [0:NUM_CACHE_LINES-1];

    wire [CACHE_IDX_W-1:0] m2s_idx            = m2s_addr[CACHE_IDX_W-1:0];
    wire [CACHE_TAG_W-1:0] m2s_cache_tag_field = m2s_addr[ADDR_W-1:CACHE_IDX_W];
    wire                   cache_hit           = cache_valid[m2s_idx] && (cache_tag[m2s_idx] == m2s_cache_tag_field);
    // Bypass forces every access to behave as a miss WITHOUT ever touching
    // the cache arrays (see cache_bypass_en comment above) -- so eviction
    // must never trigger either: there is nothing to evict FOR, since
    // bypassed accesses never allocate.
    wire                   cache_hit_eff        = cache_hit && !cache_bypass_en;
    wire                   need_evict_writeback = !cache_bypass_en && cache_valid[m2s_idx] && cache_dirty[m2s_idx] && !cache_hit;

    // ---- Eviction-writeback FSM (Phase 5) ----------------------------------
    typedef enum logic { FLUSH_IDLE, FLUSH_ACTIVE } flush_state_t;
    flush_state_t flush_state;

    wire want_flush = (flush_state == FLUSH_IDLE) && (bi_state == BI_IDLE) && !bias_query_rd_data
                       && m2s_valid && need_evict_writeback;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            flush_state <= FLUSH_IDLE;
        end else begin
            case (flush_state)
                FLUSH_IDLE:   if (want_flush) flush_state <= FLUSH_ACTIVE;
                FLUSH_ACTIVE: flush_state <= FLUSH_IDLE; // one cycle: a single backing-memory write
                default:      flush_state <= FLUSH_IDLE;
            endcase
        end
    end

    wire do_flush_write_this_cycle = (flush_state == FLUSH_ACTIVE);
    // Reconstruct the evicted line's original full address from its stored
    // tag + the (host-held-stable) index of the request that's forcing it out.
    wire [ADDR_W-1:0] flush_addr = {cache_tag[m2s_idx], m2s_idx};

    localparam int NUM_TAGS = 1 << TAG_W;
    localparam int CNT_W    = (READ_EXTRA_LATENCY <= 1) ? 1 : $clog2(READ_EXTRA_LATENCY + 1);

    logic [DATA_W-1:0] mem [0:(1<<ADDR_W)-1];

    logic               slot_valid    [0:NUM_TAGS-1];
    logic               slot_is_write [0:NUM_TAGS-1];
    logic [DATA_W-1:0]  slot_rdata    [0:NUM_TAGS-1];
    logic [CNT_W-1:0]   slot_cnt      [0:NUM_TAGS-1];

    integer i;
    initial begin
        for (i = 0; i < (1<<ADDR_W); i = i + 1) mem[i] = '0;
        for (i = 0; i < NUM_TAGS; i = i + 1) begin
            slot_valid[i]    = 1'b0;
            slot_is_write[i] = 1'b0;
            slot_rdata[i]    = '0;
            slot_cnt[i]      = '0;
        end
        for (i = 0; i < NUM_CACHE_LINES; i = i + 1) begin
            cache_valid[i] = 1'b0;
            cache_dirty[i] = 1'b0;
            cache_tag[i]   = '0;
            cache_data[i]  = '0;
        end
    end

    // ---- Response arbitration (unchanged from Phase 2) -------------------
    logic               arb_found;
    logic [TAG_W-1:0]   arb_tag;
    always_comb begin
        arb_found = 1'b0;
        arb_tag   = '0;
        for (int t = 0; t < NUM_TAGS; t++) begin
            if (!arb_found && slot_valid[t] && (slot_cnt[t] == '0)) begin
                arb_found = 1'b1;
                arb_tag   = t[TAG_W-1:0];
            end
        end
    end

    logic               resp_active;
    logic [TAG_W-1:0]   resp_tag_r;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            resp_active <= 1'b0;
            resp_tag_r  <= '0;
        end else begin
            if (!resp_active) begin
                if (arb_found) begin
                    resp_active <= 1'b1;
                    resp_tag_r  <= arb_tag;
                end
            end else if (s2m_ready) begin
                resp_active <= 1'b0;
            end
        end
    end

    wire resp_consumed_this_cycle = resp_active && s2m_ready;

    // ---- Accept path: gated by per-tag occupancy (Phase 2), bias/BI state
    // (Phase 4), AND now eviction-flush state (Phase 5). -------------------
    assign m2s_ready = !slot_valid[m2s_tag] && (bi_state == BI_IDLE) && !bias_query_rd_data
                        && (flush_state == FLUSH_IDLE) && !need_evict_writeback;
    wire accept_this_cycle = m2s_valid && m2s_ready;

    // ---- All per-slot state: one block, one driver per element ----------
    // Read HIT: fast path, data straight from cache, 0 extra latency.
    // Read MISS: existing backing-memory path, READ_EXTRA_LATENCY as before.
    always_ff @(posedge clk) begin
        for (int t = 0; t < NUM_TAGS; t++) begin
            if (accept_this_cycle && (m2s_tag == t[TAG_W-1:0])) begin
                slot_valid[t]    <= 1'b1;
                slot_is_write[t] <= m2s_is_write;
                if (!m2s_is_write) begin
                    slot_rdata[t] <= cache_hit_eff ? cache_data[m2s_idx] : mem[m2s_addr];
                end
                slot_cnt[t] <= (m2s_is_write || cache_hit_eff) ? '0 : CNT_W'(READ_EXTRA_LATENCY);
            end else if (resp_consumed_this_cycle && (resp_tag_r == t[TAG_W-1:0])) begin
                slot_valid[t] <= 1'b0;
            end else if (slot_valid[t] && slot_cnt[t] != '0) begin
                slot_cnt[t] <= slot_cnt[t] - CNT_W'(1);
            end
        end
    end

    // ---- Cache array updates: one block, one driver per element (same
    // multiple-driver discipline as the slot arrays above -- see BUG-004).
    // Mutually exclusive by construction: do_flush_write_this_cycle and
    // accept_this_cycle can never both be true on the same cycle (flush
    // gates m2s_ready, so accept can't happen until flush is done). -------
    always_ff @(posedge clk) begin
        if (do_flush_write_this_cycle) begin
            cache_dirty[m2s_idx] <= 1'b0; // written back; about to be replaced by the pending allocate
        end else if (accept_this_cycle && !cache_bypass_en) begin
            cache_valid[m2s_idx] <= 1'b1;
            cache_tag[m2s_idx]   <= m2s_cache_tag_field;
            if (m2s_is_write) begin
                cache_data[m2s_idx]  <= m2s_wdata;
                cache_dirty[m2s_idx] <= 1'b1; // write-back: backing memory updated later, on eviction
            end else if (!cache_hit) begin
                cache_data[m2s_idx]  <= mem[m2s_addr]; // read-miss fill, clean
                cache_dirty[m2s_idx] <= 1'b0;
            end
            // read HIT: cache_data/dirty already correct, untouched here
        end
        // cache_bypass_en: cache arrays are left completely untouched (frozen)
        // -- see cache_bypass_en port comment for why that's deliberate.
    end

    // ---- Backing memory: written by eviction writeback (Phase 5), OR
    // directly on every write while bypassed (Phase 8) -- exactly the
    // pre-Phase-5 behavior, since bypass mode has no cache to defer through.
    // Single write port, single driver, mutually exclusive by construction
    // (do_flush_write_this_cycle can't be true while bypassed: it requires
    // need_evict_writeback, which is forced false by cache_bypass_en above).
    always_ff @(posedge clk) begin
        if (do_flush_write_this_cycle) begin
            mem[flush_addr] <= cache_data[m2s_idx];
        end else if (accept_this_cycle && cache_bypass_en && m2s_is_write) begin
            mem[m2s_addr] <= m2s_wdata;
        end
    end

    // ---- Cache hit/miss debug counters (Phase 5) --------------------------
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            dbg_cache_hits   <= 32'b0;
            dbg_cache_misses <= 32'b0;
        end else if (accept_this_cycle) begin
            if (cache_hit_eff) dbg_cache_hits   <= dbg_cache_hits + 32'd1;
            else                dbg_cache_misses <= dbg_cache_misses + 32'd1;
        end
    end

    assign s2m_valid   = resp_active;
    assign s2m_tag     = resp_tag_r;
    assign s2m_is_data = !slot_is_write[resp_tag_r];
    assign s2m_rdata   = slot_rdata[resp_tag_r];

endmodule
