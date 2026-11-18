`timescale 1ns / 1ps
`default_nettype none
/*
 term_video.v

 The text screen of the term core (docs/PLAN.md): 1024 x 768 at 60 Hz (65 MHz, VESA: h 1024 + 24 + 136 +
 160, v 768 + 3 + 6 + 29, syncs negative), 128 x 48 cells of 8 x 16 pixels, everything at clk (the system
 clock = the pixel clock).

 Memory (the SRAM, halfword addresses, through the gather port of term_sram):
 - a row of 128 cells = 512 halfwords at rowtab[row] * 512 (rowtab: 48 entries of 12 bits, written by
   the CPU: the screen row -> its row in the ring / the alternate screen, the layout of src/fw/tscreen.h);
   a cell = 4 halfwords: h0, h1 = w0 (codepoint 20..0, wide 21, attrs 31..22: bold, underline 2 bits,
   italic, blink, reverse, conceal, strike), h2 = fg, h3 = bg (bit 15 RGB555, bit 8 default, else a
   palette index). Codepoint 0x1fffff = the right half of a wide glyph (its colours: those of the left half).
 - the font at FONT (halfword 0x100000 = byte 0x200000): codepoint * 16 + glyph row, a halfword per row
   (bit 15 = the left pixel; narrow glyphs in bits 15..8, wide ones all 16). Codepoints above U+FFFF show
   U+FFFD.

 Fetch, per scanline v (0..805), in this order, started at hcount 0:
   A: 32 halfwords = 8 cells (chunk k) of the text row R, u = (v + 16) mod 806, R = u / 16, k = u % 16
      (if u < 768): into the cell bank R % 2 (the attributes for the pixels) and the codepoint banks
   B: the 128 glyph rows of line T = v + 1 (mod 806, if T < 768) into the line buffer T % 2
 so a text row is complete one line before its first line is fetched (B of line 16R reads row R).
 Pixels: the active area is hcount 16..1039 (the syncs shifted with it); the colours and the 8 pixels of
 cell c are made during hcount 8c + 8 .. 8c + 15 (prep, one cell ahead) and shown from 8c + 16.
*/
module term_video (
	input wire clk,
	input wire reset,
	input wire show,                  // 0: black (before the firmware set up the screen)

	// the SRAM gather port (term_sram)
	output reg vid_pend = 1'b0,
	output reg [20:0] vid_adr = 21'd0,
	input wire vid_take,
	input wire vid_dv,
	input wire [15:0] vid_d,

	// registers (written by the SoC)
	input wire row_we,
	input wire [5:0] row_wa,
	input wire [11:0] row_wd,
	input wire pal_we,
	input wire [7:0] pal_wa,
	input wire [23:0] pal_wd,
	input wire [23:0] def_fg,
	input wire [23:0] def_bg,
	input wire [6:0] cur_x,
	input wire [5:0] cur_y,
	input wire cur_on,
	input wire [1:0] cur_shape,       // 0 block, 1 underline, 2 bar
	input wire cur_blink,
	output reg [15:0] frames = 16'd0, // frames shown (vsync count)

	output reg [23:0] rgb = 24'd0,
	output reg hs = 1'b1,             // negative
	output reg vs = 1'b1,             // negative
	output reg de = 1'b0
);

localparam [10:0] H_TOTAL = 11'd1344, H_ACT0 = 11'd16, H_ACT1 = 11'd1040;
localparam [10:0] H_SYNC0 = 11'd1064, H_SYNC1 = 11'd1200;          // 1040 + 24, + 136
localparam [9:0] V_TOTAL = 10'd806, V_ACT = 10'd768, V_SYNC0 = 10'd771, V_SYNC1 = 10'd777;
localparam [20:0] FONT = 21'h100000;

reg [10:0] h = 11'd0;
reg [9:0] v = 10'd0;
wire line_start = h == 11'd0;

// ------------------------------------------------------------------ memories

reg [11:0] rowtab [0:47];
reg [15:0] cellb [0:1023];        // { bank, cell 7 bits, halfword 2 bits }
reg [15:0] cp_lo [0:255];         // { bank, cell }: h0
reg [4:0] cp_hi [0:255];          //                 h1[4:0]
reg [15:0] glyph [0:255];         // { line % 2, cell }
reg [23:0] pal [0:255];

integer ii;
initial begin
	for (ii = 0; ii < 48; ii = ii + 1) rowtab[ii] = 12'd0;
end

always @(posedge clk) begin
	if (row_we && row_wa < 6'd48) rowtab[row_wa] <= row_wd;
	if (pal_we) pal[pal_wa] <= pal_wd;
end

// ------------------------------------------------------------------ fetch

wire [10:0] u_raw = { 1'b0, v } + 11'd16;
wire [9:0] u = (u_raw >= { 1'b0, V_TOTAL }) ? u_raw[9:0] - V_TOTAL : u_raw[9:0];
wire [9:0] t = (v == V_TOTAL - 10'd1) ? 10'd0 : v + 10'd1;

reg a_on = 1'b0;                  // job A of this line still to issue
reg [5:0] a_row = 6'd0;           // row R of job A
reg [3:0] a_k = 4'd0;             // chunk
reg [4:0] a_i = 5'd0;             // halfword of the chunk on vid_adr
reg [11:0] a_base = 12'd0;        // rowtab[R]
reg [9:0] b_line = 10'd0;         // line T of job B
reg [2:0] b_st = 3'd0;            // 0 idle, 1..3 start, 4 issuing, 5 waiting for job A
reg [6:0] b_i = 7'd0;             // cell of the address on vid_adr
reg [15:0] cpn_lo = 16'd0;        // the codepoint of cell b_i + 1
reg [4:0] cpn_hi = 5'd0;
reg [20:0] b_prev = 21'd0;        // the glyph address of the last cell (for a right half)

// returns: the first ones after a start of job A go to it
reg [5:0] a_ret = 6'd0;           // returns of job A still expected
reg [4:0] a_ret_i = 5'd0;
reg a_ret_bank = 1'b0;
reg [3:0] a_ret_k = 4'd0;
reg [6:0] b_ret = 7'd0;           // cell of the next glyph return
reg b_ret_bank = 1'b0;

function [20:0] gaddr(input [15:0] lo, input [4:0] hi, input [3:0] gr, input [20:0] prev);
	reg [20:0] cp;
	begin
		cp = { hi, lo };
		if (cp == 21'h1fffff) gaddr = prev;                 // the right half of a wide glyph
		else if (cp[20:16] != 5'd0) gaddr = FONT + { 1'b0, 16'hfffd, gr };
		else gaddr = FONT + { 1'b0, cp[15:0], gr };
	end
endfunction

// the codepoint banks for job B: cp_ra set at an edge, cp_*_q valid two edges later
reg [7:0] cp_ra = 8'd0;
reg [15:0] cp_lo_q = 16'd0;
reg [4:0] cp_hi_q = 5'd0;
always @(posedge clk) begin
	cp_lo_q <= cp_lo[cp_ra];
	cp_hi_q <= cp_hi[cp_ra];
end
wire [20:0] g_first = gaddr(cp_lo_q, cp_hi_q, b_line[3:0], FONT);
wire [20:0] g_next = gaddr(cpn_lo, cpn_hi, b_line[3:0], b_prev);

always @(posedge clk) begin
	if (reset) begin
		vid_pend <= 1'b0;
		a_on <= 1'b0;
		b_st <= 3'd0;
		a_ret <= 6'd0;
	end else if (line_start) begin
		vid_pend <= 1'b0;
		a_on <= u < V_ACT;
		a_row <= u[9:4];
		a_k <= u[3:0];
		a_i <= 5'd0;
		a_base <= rowtab[u[9:4]];
		b_line <= t;
		b_st <= (t < V_ACT) ? 3'd5 : 3'd0;
	end else begin
		// job A: 32 halfwords in order
		if (a_on) begin
			if (!vid_pend) begin
				vid_pend <= 1'b1;
				vid_adr <= { a_base, 9'd0 } + { 12'd0, a_k, 5'd0 };
				a_ret <= 6'd32;
				a_ret_i <= 5'd0;
				a_ret_bank <= a_row[0];
				a_ret_k <= a_k;
			end else if (vid_take) begin
				if (a_i == 5'd31) begin
					a_on <= 1'b0;
					vid_pend <= 1'b0;
				end else begin
					a_i <= a_i + 5'd1;
					vid_adr <= { a_base, 9'd0 } + { 12'd0, a_k, a_i + 5'd1 };
				end
			end
		end

		// job B: the 128 glyph rows, after job A
		case (b_st)
		3'd5: if (!a_on) begin cp_ra <= { b_line[4], 7'd0 }; b_st <= 3'd1; end
		3'd1: begin cp_ra <= { b_line[4], 7'd1 }; b_st <= 3'd2; end
		3'd2: begin
			// cp_*_q = cell 0
			vid_pend <= 1'b1;
			vid_adr <= g_first;
			b_prev <= g_first;
			b_i <= 7'd0;
			b_ret <= 7'd0;
			b_ret_bank <= b_line[0];
			cp_ra <= { b_line[4], 7'd2 };
			b_st <= 3'd3;
		end
		3'd3: begin cpn_lo <= cp_lo_q; cpn_hi <= cp_hi_q; b_st <= 3'd4; end   // cell 1
		3'd4: if (vid_take) begin
			if (b_i == 7'd127) begin
				vid_pend <= 1'b0;
				b_st <= 3'd0;
			end else begin
				b_i <= b_i + 7'd1;
				vid_adr <= g_next;
				b_prev <= g_next;
				cpn_lo <= cp_lo_q;        // cell b_i + 2 (its read went out at the last take, or in state 2)
				cpn_hi <= cp_hi_q;
				cp_ra <= { b_line[4], b_i + 7'd3 };
			end
		end
		default: ;
		endcase
	end
	// the returns (also in the clock of line_start); a start of job A sets a_ret (one clock after line_start)
	if (!reset && vid_dv && !(a_on && !vid_pend && !line_start)) begin
		if (a_ret != 6'd0) begin
			a_ret <= a_ret - 6'd1;
			a_ret_i <= a_ret_i + 5'd1;
		end else
			b_ret <= b_ret + 7'd1;
	end
end

// writes of the returns
always @(posedge clk) begin
	if (vid_dv) begin
		if (a_ret != 6'd0) begin
			cellb[{ a_ret_bank, a_ret_k, a_ret_i }] <= vid_d;
			if (a_ret_i[1:0] == 2'd0) cp_lo[{ a_ret_bank, a_ret_k, a_ret_i[4:2] }] <= vid_d;
			if (a_ret_i[1:0] == 2'd1) cp_hi[{ a_ret_bank, a_ret_k, a_ret_i[4:2] }] <= vid_d[4:0];
		end else
			glyph[{ b_ret_bank, b_ret }] <= vid_d;
	end
end

// ------------------------------------------------------------------ timing

reg [5:0] blink_cnt = 6'd0;
always @(posedge clk) begin
	if (h == H_TOTAL - 11'd1) begin
		h <= 11'd0;
		if (v == V_TOTAL - 10'd1) v <= 10'd0;
		else v <= v + 10'd1;
		if (v == V_ACT - 10'd1) begin
			frames <= frames + 16'd1;
			blink_cnt <= blink_cnt + 6'd1;
		end
	end else
		h <= h + 11'd1;
end
wire blink_on = blink_cnt[5];     // ~1 Hz: 32 frames on, 32 off
wire cur_ph = blink_cnt[4];       // cursor ~2 Hz

// ------------------------------------------------------------------ cell prep (one cell ahead)

wire [10:0] p = h - 11'd8;        // prep position: cell p / 8, phase p % 8
wire prep_on = (h >= 11'd8) && (h < 11'd1032) && (v < V_ACT);
wire [6:0] pc = p[9:3];
wire [2:0] pp = p[2:0];
wire [5:0] row = v[9:4];
wire [3:0] g = v[3:0];

// reads: the address set at the edge of phase n is in *_q after the edge of phase n + 1, used at n + 2
//   pp 0: cell bank h0, the glyph row         pp 2: c_h0, c_gl      pp 5: the palette (fg, bg copies)
//   pp 1: h1   pp 2: fg   pp 3: bg            pp 3: c_h1  pp 4: c_fg  pp 5: c_bg    pp 7: the cell is made
reg [9:0] cb_ra = 10'd0;
reg [15:0] cb_q = 16'd0;
reg [7:0] gb_ra = 8'd0;
reg [15:0] gb_q = 16'd0;
reg [7:0] palf_ra = 8'd0, palb_ra = 8'd0;
reg [23:0] palf_q = 24'd0, palb_q = 24'd0;
reg [23:0] pal_b [0:255];         // a second copy: fg and bg are read in the same clock
always @(posedge clk) begin
	if (pal_we) pal_b[pal_wa] <= pal_wd;
	cb_q <= cellb[cb_ra];
	gb_q <= glyph[gb_ra];
	palf_q <= pal[palf_ra];
	palb_q <= pal_b[palb_ra];
end

reg [15:0] c_h0 = 16'd0, c_h1 = 16'd0, c_fg = 16'd0, c_bg = 16'd0, c_gl = 16'd0;
reg [9:0] prev_attr = 10'd0;      // of the last cell that was not a right half
reg [15:0] prev_fg = 16'd0, prev_bg = 16'd0;
reg [23:0] n_fg = 24'd0, n_bg = 24'd0;   // the next cell, made
reg [7:0] n_pat = 8'd0;

function [23:0] rgb555(input [14:0] c);
	rgb555 = { c[14:10], c[14:12], c[9:5], c[9:7], c[4:0], c[4:2] };
endfunction

wire c_cont = { c_h1[4:0], c_h0 } == 21'h1fffff;
reg [9:0] attr;
reg [15:0] fgc, bgc;
reg [23:0] f, b, tmp;
reg [7:0] pat;
reg [3:0] sh;
always @(posedge clk) begin
	if (prep_on) begin
		case (pp)
		3'd0: begin cb_ra <= { row[0], pc, 2'd0 }; gb_ra <= { v[0], pc }; end
		3'd1: cb_ra <= { row[0], pc, 2'd1 };
		3'd2: begin cb_ra <= { row[0], pc, 2'd2 }; c_h0 <= cb_q; c_gl <= gb_q; end
		3'd3: begin cb_ra <= { row[0], pc, 2'd3 }; c_h1 <= cb_q; end
		3'd4: c_fg <= cb_q;
		3'd5: begin
			c_bg <= cb_q;
			// a right half takes the colours / attributes of the left half
			palf_ra <= c_cont ? prev_fg[7:0] : c_fg[7:0];
			palb_ra <= c_cont ? prev_bg[7:0] : cb_q[7:0];
		end
		3'd6: ;
		3'd7: begin
			attr = c_cont ? prev_attr : c_h1[15:6];
			fgc = c_cont ? prev_fg : c_fg;
			bgc = c_cont ? prev_bg : c_bg;
			if (!c_cont) begin
				prev_attr <= c_h1[15:6];
				prev_fg <= c_fg;
				prev_bg <= c_bg;
			end
			f = fgc[15] ? rgb555(fgc[14:0]) : fgc[8] ? def_fg : palf_q;
			b = bgc[15] ? rgb555(bgc[14:0]) : bgc[8] ? def_bg : palb_q;
			// attr: 0 bold, 2..1 underline, 3 italic, 4 blink, 5 reverse, 6 conceal, 7 strike
			pat = c_cont ? c_gl[7:0] : c_gl[15:8];
			if (attr[3]) begin
				sh = (4'd15 - g) >> 2;
				pat = pat >> sh;
			end
			if (attr[0]) pat = pat | (pat >> 1);
			if ((attr[2:1] == 2'd1 || attr[2:1] == 2'd3) && g == 4'd14) pat = 8'hff;
			if (attr[2:1] == 2'd2 && (g == 4'd13 || g == 4'd15)) pat = 8'hff;
			if (attr[7] && g == 4'd8) pat = 8'hff;
			if (attr[6] || (attr[4] && !blink_on)) pat = 8'h00;
			if (attr[5]) begin tmp = f; f = b; b = tmp; end
			if (cur_on && row == cur_y && pc == cur_x && (!cur_blink || cur_ph)) begin
				case (cur_shape)
				2'd1: if (g >= 4'd14) pat = 8'hff;
				2'd2: pat = pat | 8'h80;
				default: begin tmp = f; f = b; b = tmp; end
				endcase
			end
			n_fg <= f;
			n_bg <= b;
			n_pat <= pat;
		end
		endcase
	end
end

// ------------------------------------------------------------------ pixels

reg [23:0] s_fg = 24'd0, s_bg = 24'd0;
reg [7:0] s_pat = 8'd0;
wire act = (h >= H_ACT0) && (h < H_ACT1) && (v < V_ACT);
always @(posedge clk) begin
	if (act && h[2:0] == 3'd0) begin
		s_fg <= n_fg;
		s_bg <= n_bg;
		s_pat <= n_pat;
	end else
		s_pat <= { s_pat[6:0], 1'b0 };
end
// s_* hold the cell from the clock after its first pixel position: one register stage, the syncs too
reg act_d = 1'b0, hs_d = 1'b1, vs_d = 1'b1;
always @(posedge clk) begin
	act_d <= act;
	hs_d <= ~((h >= H_SYNC0) && (h < H_SYNC1));
	vs_d <= ~((v >= V_SYNC0) && (v < V_SYNC1));
	de <= act_d;
	hs <= hs_d;
	vs <= vs_d;
	rgb <= (act_d && show) ? (s_pat[7] ? s_fg : s_bg) : 24'd0;
end

endmodule
