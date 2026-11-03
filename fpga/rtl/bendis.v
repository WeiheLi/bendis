// Bendis on FPGA. Emits the same digests as p4/bendis.p4 built with -DWINDOW_BY_COUNT (same CRC
// hashes and count windows), at one packet per clock cycle.
//
// One packet per clock cycle, eleven pipeline stages:
//   S0      window number, clearing index, bank rotation (packet counter)
//   S1      CRC hashes (bendis_hash.v): three counting-row indexes, two onset-table indexes, 16-bit fingerprint
//   S2..S4  counting row r (r = 0, 1, 2): conservative update of the counting bank, read of
//           the previous bank, zeroing of the clearing bank
//   S5..S6  c = min of updated counters, p = min of previous counters, onset test
//   S7..S8  onset-table row 0 and row 1: match or stale entry decides, otherwise pass
//   S9      interval and digest output (registered, latency 11 cycles)
// Every bank and table row is a simple dual-port RAM with a registered read (two-cycle latency)
// that is accessed in exactly one stage: the read of a packet is issued two stages earlier, so
// the writes of the two packets ahead of it are not yet in the RAM data and are forwarded.
`default_nettype none

module sdp_ram #(parameter AW = 11, parameter DW = 16, parameter [DW-1:0] INIT = {DW{1'b0}}) (
    input  wire          clk,
    input  wire          we,
    input  wire [AW-1:0] waddr,
    input  wire [DW-1:0] wdata,
    input  wire [AW-1:0] raddr,
    output reg  [DW-1:0] rdata     // mem[raddr] two clock edges after raddr is presented
);
    reg [DW-1:0] mem [0:(1<<AW)-1];
    reg [DW-1:0] rd;
    integer i;
    initial for (i = 0; i < (1 << AW); i = i + 1) mem[i] = INIT;
    always @(posedge clk) begin
        if (we) mem[waddr] <= wdata;
        rd <= mem[raddr];
        rdata <= rd;
    end
endmodule

module bendis #(
    parameter CM_BITS  = 11,   // entries per counting bank: 2^CM_BITS
    parameter TAB_BITS = 11,   // entries per onset-table row: 2^TAB_BITS
    parameter WIN_BITS = 15,   // count windows of 2^WIN_BITS packets
    parameter [15:0] H = 16'd80,
    parameter K_SHIFT  = 1,    // burst ratio k = 2^K_SHIFT
    parameter [15:0] R = 16'd1,     // intervals v are reported iff R < v <= VMAX = R + 2^REPORT_BITS
    parameter REPORT_BITS = 6
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        in_valid,
    input  wire [63:0] in_key,      // {IPv4 src, IPv4 dst}
    output reg         out_valid,   // digest: onset interval of out_key
    output reg  [63:0] out_key,
    output reg  [15:0] out_v
);
    function [15:0] sat_inc(input [15:0] x);
        sat_inc = (x == 16'hFFFF) ? x : x + 16'd1;
    endfunction
    function [1:0] next3(input [1:0] x);
        next3 = (x == 2'd2) ? 2'd0 : x + 2'd1;
    endfunction

    localparam [15:0] WIN_FLAG = 16'h8000;
    localparam [15:0] VMAX = R + (16'd1 << REPORT_BITS);

    // ---------------------------------------------------------------- S0: window and rotation
    reg [31:0] pkt_cnt;
    reg [15:0] rot_lo;
    reg [1:0]  rot_hi;
    // Window numbers keep WIN_FLAG and wrap modulo 2^15, like the time windows of the P4 program.
    wire [15:0] win_in = WIN_FLAG | {1'b0, pkt_cnt[WIN_BITS +: 15]};
    wire        new_win = rot_lo != win_in;
    wire [1:0]  rot_in = new_win ? next3(rot_hi) : rot_hi;

    reg        v0;
    reg [63:0] key0;
    reg [15:0] win0;
    reg [CM_BITS-1:0] zidx0;
    reg [1:0]  rot0;
    always @(posedge clk) begin
        key0 <= in_key;
        win0 <= win_in;
        zidx0 <= pkt_cnt[CM_BITS-1:0];
        rot0 <= rot_in;
        if (rst) begin
            pkt_cnt <= 0; rot_lo <= 0; rot_hi <= 0; v0 <= 0;
        end else begin
            v0 <= in_valid;
            if (in_valid) begin
                pkt_cnt <= pkt_cnt + 1;
                rot_lo <= win_in;
                rot_hi <= rot_in;
            end
        end
    end

    // ---------------------------------------------------------------- S1: hashes
    reg        v1;
    reg [63:0] key1;
    reg [15:0] win1, fp1;
    reg [CM_BITS-1:0] zidx1, idx1_0, idx1_1, idx1_2;
    reg [TAB_BITS-1:0] tidx1_0, tidx1_1;
    reg [1:0]  rot1;
    wire [31:0] h_cm0, h_cm1, h_cm2, h_tab0, h_tab1;
    wire [15:0] fp_raw;
    bendis_hash hash (.key(key0), .h_cm0(h_cm0), .h_cm1(h_cm1), .h_cm2(h_cm2), .h_tab0(h_tab0),
                   .h_tab1(h_tab1), .h_fp(fp_raw));
    always @(posedge clk) begin
        v1 <= rst ? 1'b0 : v0;
        key1 <= key0; win1 <= win0; zidx1 <= zidx0; rot1 <= rot0;
        
        idx1_0 <= h_cm0[CM_BITS-1:0];
        idx1_1 <= h_cm1[CM_BITS-1:0];
        idx1_2 <= h_cm2[CM_BITS-1:0];
        tidx1_0 <= h_tab0[TAB_BITS-1:0];
        tidx1_1 <= h_tab1[TAB_BITS-1:0];
        fp1 <= (fp_raw == 16'd0) ? 16'd1 : fp_raw;
    end

    // ---------------------------------------------------------------- S1x: read issue for row 0
    reg        v1x;
    reg [63:0] key1x;
    reg [15:0] win1x, fp1x;
    reg [CM_BITS-1:0] zidx1x, idx1x_0, idx1x_1, idx1x_2;
    reg [TAB_BITS-1:0] tidx1x_0, tidx1x_1;
    reg [1:0]  rot1x;
    always @(posedge clk) begin
        v1x <= rst ? 1'b0 : v1;
        key1x <= key1; win1x <= win1; fp1x <= fp1; zidx1x <= zidx1; rot1x <= rot1;
        idx1x_0 <= idx1_0; idx1x_1 <= idx1_1; idx1x_2 <= idx1_2;
        tidx1x_0 <= tidx1_0; tidx1x_1 <= tidx1_1;
    end

    // ---------------------------------------------------------------- S2..S4: count-sketch rows
    // Stage registers of row stage r hold the packet being processed by row r.
    reg        v2, v3, v4;
    reg [63:0] key2, key3, key4;
    reg [15:0] win2, win3, win4, fp2, fp3, fp4;
    reg [CM_BITS-1:0] zidx2, zidx3, zidx4, i2_0, i2_1, i2_2, i3_1, i3_2, i4_2;
    reg [TAB_BITS-1:0] t2_0, t2_1, t3_0, t3_1, t4_0, t4_1;
    reg [1:0]  rot2, rot3, rot4;
    reg [15:0] y3_0, y4_0, y4_1, p3_0, p4_0, p4_1;
    wire [15:0] y_0, y_1, y_2, q_0, q_1, q_2;
    always @(posedge clk) begin
        v2 <= rst ? 1'b0 : v1x; v3 <= rst ? 1'b0 : v2; v4 <= rst ? 1'b0 : v3;
        key2 <= key1x; key3 <= key2; key4 <= key3;
        win2 <= win1x; win3 <= win2; win4 <= win3;
        
        fp2 <= fp1x; fp3 <= fp2; fp4 <= fp3;
        zidx2 <= zidx1x; zidx3 <= zidx2; zidx4 <= zidx3;
        rot2 <= rot1x; rot3 <= rot2; rot4 <= rot3;
        i2_0 <= idx1x_0; i2_1 <= idx1x_1; i2_2 <= idx1x_2;
        i3_1 <= i2_1; i3_2 <= i2_2; i4_2 <= i3_2;
        t2_0 <= tidx1x_0; t2_1 <= tidx1x_1; t3_0 <= t2_0; t3_1 <= t2_1; t4_0 <= t3_0; t4_1 <= t3_1;
        y3_0 <= y_0; p3_0 <= q_0;
        y4_0 <= y3_0; p4_0 <= p3_0; y4_1 <= y_1; p4_1 <= q_1;
    end
    cm_row #(.CM_BITS(CM_BITS), .ROW(0)) row0 (
        .clk(clk), .rst(rst), .raddr(idx1_0), .idx_next(idx1x_0), .v(v2), .idx(i2_0), .zidx(zidx2), .rot(rot2),
        .ya(16'd0), .yb(16'd0), .y(y_0), .p(q_0));
    cm_row #(.CM_BITS(CM_BITS), .ROW(1)) row1 (
        .clk(clk), .rst(rst), .raddr(idx1x_1), .idx_next(i2_1), .v(v3), .idx(i3_1), .zidx(zidx3), .rot(rot3),
        .ya(y3_0), .yb(16'd0), .y(y_1), .p(q_1));
    cm_row #(.CM_BITS(CM_BITS), .ROW(2)) row2 (
        .clk(clk), .rst(rst), .raddr(i2_2), .idx_next(i3_2), .v(v4), .idx(i4_2), .zidx(zidx4), .rot(rot4),
        .ya(y4_0), .yb(y4_1), .y(y_2), .p(q_2));

    // ---------------------------------------------------------------- S5..S6: onset test
    reg        v5, v6, onset6;
    reg [63:0] key5, key6;
    reg [15:0] win5, win6, fp5, fp6;
    reg [TAB_BITS-1:0] t5_0, t5_1, t6_0, t6_1;
    reg [15:0] c5_01, c5_2, p5_01, p5_2;
    wire [15:0] c5 = (c5_2 < c5_01) ? c5_2 : c5_01;
    wire [15:0] p5 = (p5_2 < p5_01) ? p5_2 : p5_01;
    wire [16:0] kp5 = {1'b0, p5} << K_SHIFT;
    always @(posedge clk) begin
        v5 <= rst ? 1'b0 : v4; v6 <= rst ? 1'b0 : v5;
        key5 <= key4; key6 <= key5; win5 <= win4; win6 <= win5;
        fp5 <= fp4; fp6 <= fp5;
        t5_0 <= t4_0; t5_1 <= t4_1; t6_0 <= t5_0; t6_1 <= t5_1;
        c5_01 <= (y4_1 < y4_0) ? y4_1 : y4_0;
        p5_01 <= (p4_1 < p4_0) ? p4_1 : p4_0;
        c5_2 <= y_2;
        p5_2 <= q_2;
        onset6 <= v5 && c5 >= H && {1'b0, c5} >= kp5;
    end

    // ---------------------------------------------------------------- S7..S8: onset table
    // A row decides on a match or a stale entry and returns the old window; otherwise it passes (0).
    // Without an onset, row 0 is not accessed and returns 1; row 1 is accessed iff row 0 passed.
    reg        v7, v8, onset7;
    reg [63:0] key7, key8;
    reg [15:0] win7, win8, fp7, fp8, old0_8;
    reg [TAB_BITS-1:0] t7_0, t7_1, t8_1;
    wire        dec7, dec8;
    wire [15:0] e7, e8;
    tab_row #(.TAB_BITS(TAB_BITS), .VMAX(VMAX)) tab0 (
        .clk(clk), .rst(rst), .raddr(t5_0), .idx_next(t6_0), .en(v7 && onset7), .idx(t7_0), .fp(fp7),
        .win(win7), .decide(dec7), .old(e7));
    tab_row #(.TAB_BITS(TAB_BITS), .VMAX(VMAX)) tab1 (
        .clk(clk), .rst(rst), .raddr(t6_1), .idx_next(t7_1), .en(v8 && old0_8 == 16'd0), .idx(t8_1),
        .fp(fp8), .win(win8), .decide(dec8), .old(e8));
    always @(posedge clk) begin
        v7 <= rst ? 1'b0 : v6; v8 <= rst ? 1'b0 : v7;
        onset7 <= onset6;
        key7 <= key6; key8 <= key7; win7 <= win6; win8 <= win7;
        fp7 <= fp6; fp8 <= fp7;
        t7_0 <= t6_0; t7_1 <= t6_1; t8_1 <= t7_1;
        old0_8 <= !onset7 ? 16'd1 : dec7 ? e7 : 16'd0;
    end

    // ---------------------------------------------------------------- S9: interval and digest
    reg        v9;
    reg [63:0] key9;
    reg [15:0] win9, old0_9, old1_9;
    always @(posedge clk) begin
        v9 <= rst ? 1'b0 : v8;
        key9 <= key8; win9 <= win8; old0_9 <= old0_8;
        old1_9 <= dec8 ? e8 : 16'd0;
    end
    wire [15:0] old = (old0_9 == 16'd0) ? old1_9 : old0_9;
    wire [15:0] interval = win9 - old;
    wire [15:0] vr = win9 - (R + 16'd1) - old;
    always @(posedge clk) begin
        if (rst) out_valid <= 1'b0;
        else out_valid <= v9 && (vr >> REPORT_BITS) == 16'd0;
        out_key <= key9;
        out_v <= interval;
    end
endmodule

// One count-sketch row: three banks whose roles rotate with the window (counting bank rot,
// clearing bank rot+1, previous bank rot+2, mod 3). The row increments its counting bank iff
// ROW == 0 or its value is below the updated values ya (and yb) of the rows before it.
module cm_row #(parameter CM_BITS = 11, parameter ROW = 0) (
    input  wire               clk,
    input  wire               rst,
    input  wire [CM_BITS-1:0] raddr,   // index of the packet two stages before this one
    input  wire [CM_BITS-1:0] idx_next, // index of the packet in the stage before this one
    input  wire               v,       // packet in this stage
    input  wire [CM_BITS-1:0] idx,
    input  wire [CM_BITS-1:0] zidx,
    input  wire [1:0]         rot,
    input  wire [15:0]        ya,
    input  wire [15:0]        yb,
    output reg  [15:0]        y,       // updated counter of the counting bank
    output reg  [15:0]        p        // counter of the previous bank
);
    function [1:0] next3(input [1:0] x);
        next3 = (x == 2'd2) ? 2'd0 : x + 2'd1;
    endfunction
    wire [1:0] bc = rot, bz = next3(rot), bp = next3(next3(rot));
    wire [47:0] q;
    reg  [2:0]  we;
    reg  [3*CM_BITS-1:0] wa;
    reg  [47:0] wd;
    reg  [2:0]  lw_en, lw2_en;           // writes of the two packets ahead, newest first
    reg  [3*CM_BITS-1:0] lw_addr, lw2_addr;
    reg  [47:0] lw_data, lw2_data;
    // Forwarding selects for the packet in this stage, computed one cycle ahead: sel1 marks banks
    // written by the packet just ahead (now in lw), sel2 those written by the one before (in lw2).
    reg  [2:0]  sel1, sel2;
    genvar g;
    generate
        for (g = 0; g < 3; g = g + 1) begin : banks
            sdp_ram #(.AW(CM_BITS), .DW(16)) ram (
                .clk(clk), .we(we[g]), .waddr(wa[g*CM_BITS +: CM_BITS]), .wdata(wd[g*16 +: 16]),
                .raddr(raddr), .rdata(q[g*16 +: 16]));
        end
    endgenerate
    reg [15:0] val [0:2];
    reg [15:0] x, xi;
    reg        inc;
    integer j;
    always @* begin
        for (j = 0; j < 3; j = j + 1)
            val[j] = sel1[j] ? lw_data[j*16 +: 16] : sel2[j] ? lw2_data[j*16 +: 16] : q[j*16 +: 16];
        x = val[bc];
        p = val[bp];
        xi = (x == 16'hFFFF) ? x : x + 16'd1;
        inc = (ROW == 0) || (ROW == 1 && x < ya) || (ROW == 2 && x < ya && x < yb);
        y = inc ? xi : x;
        for (j = 0; j < 3; j = j + 1) begin
            we[j] = v && (j == bc || j == bz);
            wa[j*CM_BITS +: CM_BITS] = (j == bc) ? idx : zidx;
            wd[j*16 +: 16] = (j == bc) ? y : 16'd0;
        end
    end
    always @(posedge clk) begin
        lw_en <= rst ? 3'd0 : we;
        lw_addr <= wa;
        lw_data <= wd;
        lw2_en <= rst ? 3'd0 : lw_en;
        lw2_addr <= lw_addr;
        lw2_data <= lw_data;
        for (j = 0; j < 3; j = j + 1) begin
            sel1[j] <= we[j] && wa[j*CM_BITS +: CM_BITS] == idx_next;
            sel2[j] <= lw_en[j] && lw_addr[j*CM_BITS +: CM_BITS] == idx_next;
        end
    end
endmodule

// One onset-table row of {fingerprint, last onset window} entries. When en, a matching fingerprint
// or a stale entry ((win - window) mod 2^16 > VMAX) decides: the entry becomes {fp, win} and the
// old window is returned; otherwise the packet passes to the next row.
module tab_row #(parameter TAB_BITS = 11, parameter [15:0] VMAX = 16'd65) (
    input  wire                clk,
    input  wire                rst,
    input  wire [TAB_BITS-1:0] raddr,
    input  wire [TAB_BITS-1:0] idx_next,
    input  wire                en,
    input  wire [TAB_BITS-1:0] idx,
    input  wire [15:0]         fp,
    input  wire [15:0]         win,
    output wire                decide,
    output wire [15:0]         old
);
    wire [31:0] q;
    reg         lw_en, lw2_en;
    reg  [TAB_BITS-1:0] lw_addr, lw2_addr;
    reg  [31:0] lw_data, lw2_data;
    reg         sel1, sel2;
    wire [31:0] e = sel1 ? lw_data : sel2 ? lw2_data : q;
    wire [15:0] age = win - e[15:0];
    assign decide = e[31:16] == fp || age > VMAX;
    assign old = e[15:0];
    wire        we = en && decide;
    sdp_ram #(.AW(TAB_BITS), .DW(32), .INIT(32'd1)) ram (
        .clk(clk), .we(we), .waddr(idx), .wdata({fp, win}), .raddr(raddr), .rdata(q));
    always @(posedge clk) begin
        lw_en <= rst ? 1'b0 : we;
        lw_addr <= idx;
        lw_data <= {fp, win};
        lw2_en <= rst ? 1'b0 : lw_en;
        lw2_addr <= lw_addr;
        lw2_data <= lw_data;
        sel1 <= we && idx == idx_next;
        sel2 <= lw_en && lw_addr == idx_next;
    end
endmodule
`default_nettype wire
