`timescale 1ns / 1ps
`default_nettype none
/*
 term_sram.v

 The SRAM of the board (2M x 16, 10 ns, asynchronous) at the system clock (clk, 65 MHz), from
 doom_sram.v of karabas-go-core-doom (the CPU port and the read / write timing are the same), two ports:

 - CPU, a wishbone slave as the VexRiscv buses use it: a line fill (CTI 010, 8 words) or a single read,
   a write of one word with byte selects (per halfword lane pair). The loader of the SoC uses this port too.
 - video, a gather port: while vid_pend, vid_adr is the halfword address of the next read. The
   controller takes it (vid_take one clock after the address went out to MA); the video must show the
   next address (or drop vid_pend) in the clock after vid_take. The halfwords come back in the order of
   the takes, vid_dv one clock with vid_d. Served between CPU transactions, up to VID_RUN halfwords at a
   time; the video goes first after a CPU transaction (cpu_last), the CPU after a run.

 Read: a halfword in two clocks (30.8 ns at 65 MHz): MA from an IOB flip-flop held two clocks, MD into an
 IOB flip-flop at the end of the second (TS_SRAM_OUT / TS_SRAM_IN of the ucf). Write: three clocks.
*/
module term_sram #(
	parameter [5:0] VID_RUN = 6'd32
) (
	input wire clk,

	// CPU (byte address of the 4 MB)
	input wire wb_cyc,
	input wire wb_stb,
	input wire wb_we,
	input wire [3:0] wb_sel,
	input wire [21:2] wb_adr,
	input wire [2:0] wb_cti,
	input wire [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o = 32'd0,
	output reg wb_ack = 1'b0,

	// video (halfword addresses)
	input wire vid_pend,
	input wire [20:0] vid_adr,
	output reg vid_take = 1'b0,
	output reg vid_dv = 1'b0,
	output reg [15:0] vid_d = 16'd0,

	// pins (the top makes the tristate of MD)
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

(* IOB = "TRUE" *) reg [20:0] ma_r = 21'd0;
(* IOB = "TRUE" *) reg [1:0] mwr_r = 2'b11;
(* IOB = "TRUE" *) reg [1:0] mrd_r = 2'b00;
(* IOB = "TRUE" *) reg [15:0] md_out = 16'd0;
(* IOB = "TRUE", equivalent_register_removal = "no" *) reg [15:0] md_oe_n = 16'hFFFF;
(* IOB = "TRUE" *) reg [15:0] md_in = 16'd0;

assign MA = ma_r;
assign MWR_N = mwr_r;
assign MRD_N = mrd_r;
assign MD_O = md_out;
assign MD_OE_N = md_oe_n;

localparam [2:0] S_IDLE = 3'd0, S_RD = 3'd1, S_W1 = 3'd2, S_W2 = 3'd3, S_W3 = 3'd4, S_VID = 3'd5;

reg [2:0] st = S_IDLE;
wire wb_req = wb_cyc & wb_stb & ~wb_ack;
reg [21:2] q_adr = 20'd0;
reg [31:0] q_dat = 32'd0;
reg [3:0] q_sel = 4'd0;
reg q_hi = 1'b0;                  // write: the halfword being written
reg [4:0] rd_n = 5'd0;            // read: halfwords done
reg [4:0] rd_last = 5'd0;         // read: halfwords of the request (2 or 16)
reg ph = 1'b0;                    // read: the second clock of a halfword
reg [15:0] lo_half = 16'd0;

reg cpu_last = 1'b0;              // the last transaction was the CPU's: video next if pending
reg v_ph = 1'b0;                  // video: the second clock of a halfword
reg v_out = 1'b0;                 // video: md_in holds a taken halfword to give out at ph 0
reg v_more = 1'b0;                // video: another halfword was taken at the last ph 1
reg [5:0] v_run = 6'd0;           // video: takes left in this run

always @(posedge clk) begin
	wb_ack <= 1'b0;
	vid_take <= 1'b0;
	vid_dv <= 1'b0;
	md_in <= MD_I;

	case (st)
	S_IDLE: begin
		md_oe_n <= 16'hFFFF;
		mrd_r <= 2'b00;
		if (vid_pend && (cpu_last || !wb_req)) begin
			cpu_last <= 1'b0;
			ma_r <= vid_adr;
			vid_take <= 1'b1;
			v_run <= VID_RUN - 6'd1;
			v_ph <= 1'b0;
			v_out <= 1'b0;
			v_more <= 1'b1;
			st <= S_VID;
		end else if (wb_req) begin
			cpu_last <= 1'b1;
			q_adr <= wb_adr;
			q_dat <= wb_dat_i;
			q_sel <= wb_sel;
			if (wb_we) begin
				if (wb_sel[1:0] != 2'b00) begin
					q_hi <= 1'b0;
					ma_r <= { wb_adr[21:2], 1'b0 };
					md_out <= wb_dat_i[15:0];
				end else begin
					q_hi <= 1'b1;
					ma_r <= { wb_adr[21:2], 1'b1 };
					md_out <= wb_dat_i[31:16];
				end
				md_oe_n <= 16'h0000;
				mrd_r <= 2'b11;
				st <= S_W1;
			end else begin
				ma_r <= { wb_adr[21:2], 1'b0 };
				rd_n <= 5'd0;
				ph <= 1'b0;
				rd_last <= (wb_cti == 3'b010) ? 5'd16 : 5'd2;
				st <= S_RD;
			end
		end
	end


	// a halfword every two clocks: the address of halfword rd_n is out (ph 0, 1); md_in takes it at
	// the end of ph 1 and is used a clock later (ph 0 of the next one, or here in S_IDLE's clock)
	S_RD: begin
		ph <= ~ph;
		if (ph) begin
			rd_n <= rd_n + 5'd1;
			if (rd_n != rd_last - 5'd1)
				ma_r <= { q_adr[21:5], q_adr[4:2] + rd_n[3:1] + { 2'b00, rd_n[0] }, ~rd_n[0] };
		end
		// md_in now holds halfword rd_n - 1 (taken at the end of its ph 1)
		if (!ph && rd_n != 5'd0) begin
			if (rd_n[0]) lo_half <= md_in;
			else begin
				wb_dat_o <= { md_in, lo_half };
				wb_ack <= 1'b1;
			end
			if (rd_n == rd_last) st <= S_IDLE;
		end
	end

	S_W1: begin
		mwr_r <= q_hi ? ~q_sel[3:2] : ~q_sel[1:0];
		st <= S_W2;
	end
	S_W2: begin
		mwr_r <= 2'b11;
		st <= S_W3;
	end
	S_W3: begin
		// the address and data held a clock after /WE; then the high half if it has lanes
		if (!q_hi && q_sel[3:2] != 2'b00) begin
			q_hi <= 1'b1;
			ma_r <= { q_adr[21:2], 1'b1 };
			md_out <= q_dat[31:16];
			st <= S_W1;
		end else begin
			wb_ack <= 1'b1;
			md_oe_n <= 16'hFFFF;
			mrd_r <= 2'b00;
			st <= S_IDLE;
		end
	end

	// video: a halfword every two clocks. ph 1: md_in takes the halfword whose address went out two
	// clocks ago, the next address goes out (if any); ph 0: the halfword taken is given out
	S_VID: begin
		v_ph <= ~v_ph;
		if (v_ph) begin
			v_out <= 1'b1;
			if (v_run != 6'd0 && vid_pend) begin
				ma_r <= vid_adr;
				vid_take <= 1'b1;
				v_run <= v_run - 6'd1;
				v_more <= 1'b1;
			end else
				v_more <= 1'b0;
		end else if (v_out) begin
			vid_dv <= 1'b1;
			vid_d <= md_in;
			if (!v_more) st <= S_IDLE;
		end
	end
	default: st <= S_IDLE;
	endcase
end

endmodule
