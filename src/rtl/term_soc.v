`timescale 1ns / 1ps
`default_nettype none
/*
 term_soc.v

 The term machine without the board glue (docs/PLAN.md Ф1): VexRiscv (rv32im, I$ 16K, D$ 16K, wishbone),
 the SRAM (term_sram, everything: no SDRAM, its activity disturbs USB), the text screen (term_video), MMIO,
 the loader, the serial port 0 of kg_mcu2 (USB CDC). One clock (clk, 65 MHz = the pixel clock), after
 doom_soc.v of karabas-go-core-doom.

 Memory map of the CPU (src/fw/mmio_hw.h):
   0x00000000 SRAM 4 MB: program 0..1 MB, the row ring 0x100000, the alternate screen 0x1c0000, the font
              0x200000 (codepoint * 32 bytes)
   0x80000000 MMIO (uncached: VexRiscv IO range = bit 31), word registers:
     00 W  DBG: a byte of the debug console (kept as dbg_byte)     08 W  EXIT: the code (dbg_exit)
     0c R  CYCLES   1c R  MS   40 R  US (since reset)              44 W  TCMP   48 W  TEN (bit 0)
           the timer interrupt (VexRiscv timerInterrupt, level): TEN and US - TCMP >= 0
     28 R  KBD0 = kg_mcu2 KBD[31:0] { key 3, key 2, key 1, modifiers }   2c R  KBD1 { 0, key 6, 5, 4 }
     30 R  KBDN: reports seen        34 R  MOUSE (as core-doom)    50 R  SW: the OSD switches
     60 R  SER_RX: bit 8 = a byte, 7..0 the byte (the read takes it) of the USB serial port
     64 W  SER_TX: a byte to the host (dropped when SER_STAT bit 1)
     68 R  SER_STAT: bit 0 rx bytes waiting, bit 1 tx full          6c R  SER_CFG: kg_mcu2 PORT_CFG port 0
     80 W  DEF_FG   84 W  DEF_BG (0x00RRGGBB)
     88 W  CURSOR: { blink 24, shape 17..16, on 15, y 13..8, x 6..0 }
     8c W  SHOW (bit 0)              90 R  FRAMES (vsyncs)
     100..1bc W  ROWTAB[(a - 100) / 4] = the row of screen row n: SRAM byte address / 1024 (12 bits)
     400..7fc W  PAL[(a - 400) / 4] = 0x00RRGGBB
 Loader (kg_mcu2 LD_*, the CPU in reset; the video does not read the SRAM then: link v2 asks LD_READY once
 per 512-byte DATA packet, then a byte comes every 250 ns, a byte write here takes 5 clocks): kind 0 (the ROMs of the .kg: the program, the base glyphs) to the
 SRAM at their address; kind 2 slot 0 (the Font item: Unifont font_full.bin, 2 MB) to 0x200000 + its
 address. A byte per write (byte select), ld_ready low while one is pending.
*/
module term_soc #(
	parameter MS_DIV = 17'd65000,     // clocks per millisecond
	parameter [6:0] US_DIV = 7'd65,   // clocks per microsecond
	parameter [31:0] RESET_VECTOR = 32'h00000000
) (
	input wire clk,
	input wire reset,

	// loader (kg_mcu2)
	input wire [1:0] ld_kind,
	input wire [7:0] ld_slot,
	input wire ld_active,
	input wire [31:0] ld_addr,
	input wire [7:0] ld_data,
	input wire ld_wr,
	output wire ld_ready,

	// HID (kg_mcu2)
	input wire [55:0] kbd,
	input wire kbd_wr,
	input wire [7:0] ms_x,
	input wire [7:0] ms_y,
	input wire [7:0] ms_b,
	input wire ms_wr,
	input wire [31:0] sw,

	// serial port 0 (kg_mcu2 PORT: USB CDC)
	input wire [7:0] ser_out_data,    // host -> core
	input wire ser_out_wr,
	output wire ser_out_ready,
	output reg [7:0] ser_in_data = 8'd0,   // core -> host
	output reg ser_in_wr = 1'b0,
	input wire ser_in_full,
	input wire [31:0] ser_cfg,

	// video out (clk)
	output wire [23:0] rgb,
	output wire hs,
	output wire vs,
	output wire de,

	// debug
	output reg [7:0] dbg_byte = 8'd0,
	output reg dbg_wr = 1'b0,
	output reg [8:0] dbg_exit = 9'd0,
	output wire cpu_run,

	// SRAM pins
	output wire [20:0] MA,
	input wire [15:0] MD_I,
	output wire [15:0] MD_O,
	output wire [15:0] MD_OE_N,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N
);

// ------------------------------------------------------------------ reset / loader

reg cpu_reset = 1'b1;
assign cpu_run = ~cpu_reset;
always @(posedge clk)
	cpu_reset <= reset | ld_active;

reg ld_pend = 1'b0;
reg [21:0] ld_a = 22'd0;
reg [7:0] ld_d = 8'd0;
assign ld_ready = ~ld_pend;

// ------------------------------------------------------------------ CPU

wire i_cyc, i_stb, d_cyc, d_stb, d_we;
wire [29:0] i_adr, d_adr;
wire [31:0] d_dat_o;
wire [3:0] d_sel;
wire [2:0] i_cti, d_cti;
reg [31:0] i_dat_i, d_dat_i;
reg i_ack, d_ack;
reg timer_irq = 1'b0;

VexRiscv cpu (
	.externalResetVector(RESET_VECTOR),
	.timerInterrupt(timer_irq),
	.softwareInterrupt(1'b0),
	.externalInterruptArray(32'd0),
	.iBusWishbone_CYC(i_cyc),
	.iBusWishbone_STB(i_stb),
	.iBusWishbone_ACK(i_ack),
	.iBusWishbone_WE(),
	.iBusWishbone_ADR(i_adr),
	.iBusWishbone_DAT_MISO(i_dat_i),
	.iBusWishbone_DAT_MOSI(),
	.iBusWishbone_SEL(),
	.iBusWishbone_ERR(1'b0),
	.iBusWishbone_CTI(i_cti),
	.iBusWishbone_BTE(),
	.dBusWishbone_CYC(d_cyc),
	.dBusWishbone_STB(d_stb),
	.dBusWishbone_ACK(d_ack),
	.dBusWishbone_WE(d_we),
	.dBusWishbone_ADR(d_adr),
	.dBusWishbone_DAT_MISO(d_dat_i),
	.dBusWishbone_DAT_MOSI(d_dat_o),
	.dBusWishbone_SEL(d_sel),
	.dBusWishbone_ERR(1'b0),
	.dBusWishbone_CTI(d_cti),
	.dBusWishbone_BTE(),
	.clk(clk),
	.reset(cpu_reset)
);

wire d_req = d_cyc & d_stb;
wire i_req = i_cyc & i_stb;
wire d_io = d_adr[29];                      // 0x80000000
wire d_sr = ~d_io;

// ------------------------------------------------------------------ SRAM and its owner

localparam [1:0] O_NONE = 2'd0, O_D = 2'd1, O_I = 2'd2, O_LD = 2'd3;
reg [1:0] own = O_NONE;
wire sr_ack;
wire [31:0] sr_dat;
reg sr_cyc, sr_we;
reg [3:0] sr_sel;
reg [21:2] sr_adr;
reg [2:0] sr_cti;
reg [31:0] sr_wdat;

always @(*) begin
	sr_cyc = 1'b0; sr_we = 1'b0; sr_sel = 4'hF; sr_adr = 20'd0; sr_cti = 3'd0; sr_wdat = 32'd0;
	case (own)
	O_D: begin sr_cyc = d_req; sr_we = d_we; sr_sel = d_sel; sr_adr = d_adr[19:0]; sr_cti = d_cti; sr_wdat = d_dat_o; end
	O_I: begin sr_cyc = i_req; sr_we = 1'b0; sr_sel = 4'hF; sr_adr = i_adr[19:0]; sr_cti = i_cti; end
	O_LD: begin sr_cyc = ld_pend; sr_we = 1'b1; sr_sel = 4'b0001 << ld_a[1:0]; sr_adr = ld_a[21:2]; sr_cti = 3'd0; sr_wdat = {4{ld_d}}; end
	default: ;
	endcase
end

// the owner: kept to the last ack of a transaction (an ack with CTI not 010)
always @(posedge clk) begin
	if (cpu_reset) begin
		own <= ld_pend ? O_LD : O_NONE;
		if (own == O_LD && sr_ack) own <= O_NONE;
	end else if (own == O_NONE) begin
		if (d_req && d_sr) own <= O_D;
		else if (i_req) own <= O_I;
	end else if (sr_ack && sr_cti != 3'b010)
		own <= O_NONE;
end

wire vid_pend, vid_take, vid_dv;
wire [20:0] vid_adr;
wire [15:0] vid_d;

term_sram sram (
	.clk(clk),
	.wb_cyc(sr_cyc), .wb_stb(sr_cyc), .wb_we(sr_we), .wb_sel(sr_sel), .wb_adr(sr_adr), .wb_cti(sr_cti),
	.wb_dat_i(sr_wdat), .wb_dat_o(sr_dat), .wb_ack(sr_ack),
	.vid_pend(vid_pend), .vid_adr(vid_adr), .vid_take(vid_take), .vid_dv(vid_dv), .vid_d(vid_d),
	.MA(MA), .MD_I(MD_I), .MD_O(MD_O), .MD_OE_N(MD_OE_N), .MWR_N(MWR_N), .MRD_N(MRD_N)
);

// ------------------------------------------------------------------ video

reg row_we = 1'b0, pal_we = 1'b0;
reg [5:0] row_wa = 6'd0;
reg [11:0] row_wd = 12'd0;
reg [7:0] pal_wa = 8'd0;
reg [23:0] pal_wd = 24'd0;
reg [23:0] def_fg = 24'hc0c0c0, def_bg = 24'h000000;
reg [6:0] cur_x = 7'd0;
reg [5:0] cur_y = 6'd0;
reg cur_on = 1'b0, cur_blink = 1'b0, show = 1'b0;
reg [1:0] cur_shape = 2'd0;
wire [15:0] frames;

term_video video (
	.clk(clk),
	.reset(reset),
	.show(show & cpu_run),
	.fetch_en(cpu_run),
	.vid_pend(vid_pend), .vid_adr(vid_adr), .vid_take(vid_take), .vid_dv(vid_dv), .vid_d(vid_d),
	.row_we(row_we), .row_wa(row_wa), .row_wd(row_wd),
	.pal_we(pal_we), .pal_wa(pal_wa), .pal_wd(pal_wd),
	.def_fg(def_fg), .def_bg(def_bg),
	.cur_x(cur_x), .cur_y(cur_y), .cur_on(cur_on), .cur_shape(cur_shape), .cur_blink(cur_blink),
	.frames(frames),
	.rgb(rgb), .hs(hs), .vs(vs), .de(de)
);

// ------------------------------------------------------------------ serial port 0: receive fifo (512 bytes)

reg [7:0] rx_mem [0:511];
reg [8:0] rx_wp = 9'd0, rx_rp = 9'd0;
wire [8:0] rx_n = rx_wp - rx_rp;
assign ser_out_ready = rx_n < 9'd500;        // a margin for the bytes in flight in kg_mcu2
reg [7:0] rx_q = 8'd0;
always @(posedge clk) begin
	if (ser_out_wr && rx_n != 9'd511) begin
		rx_mem[rx_wp] <= ser_out_data;
		rx_wp <= rx_wp + 9'd1;
	end
	rx_q <= rx_mem[rx_rp];
end

// ------------------------------------------------------------------ MMIO

reg [31:0] cycles = 32'd0;
reg [31:0] ms = 32'd0;
reg [16:0] ms_cnt = 17'd0;
reg [31:0] kbd_n = 32'd0;
reg signed [11:0] ms_dx = 12'sd0, ms_dy = 12'sd0;
reg [7:0] ms_btn = 8'd0;
reg io_ack = 1'b0;
reg io_wait = 1'b0;                          // SER_RX: one clock for rx_q
reg [31:0] io_dat = 32'd0;
wire [11:0] io_off = { d_adr[9:0], 2'b00 };
reg [31:0] us = 32'd0, tcmp = 32'd0;
reg [6:0] us_div = 7'd0;
reg ten = 1'b0;

function signed [11:0] sat_add(input signed [11:0] a, input [7:0] d);
	reg signed [12:0] s;
	begin
		s = { a[11], a } + { {5{d[7]}}, d };
		sat_add = (s > 13'sd2047) ? 12'sd2047 : (s < -13'sd2048) ? -12'sd2048 : s[11:0];
	end
endfunction

wire io_req = d_req && d_io && !io_ack && !cpu_reset;

always @(posedge clk) begin
	io_ack <= 1'b0;
	dbg_wr <= 1'b0;
	row_we <= 1'b0;
	pal_we <= 1'b0;
	ser_in_wr <= 1'b0;
	timer_irq <= ten && !cpu_reset && $signed(us - tcmp) >= 0;
	if (cpu_reset) begin
		us <= 32'd0;
		us_div <= 7'd0;
		ten <= 1'b0;
		cycles <= 32'd0;
		ms <= 32'd0;
		ms_cnt <= 17'd0;
		show <= 1'b0;
		cur_on <= 1'b0;
	end else begin
		if (us_div == US_DIV - 7'd1) begin us_div <= 7'd0; us <= us + 32'd1; end
		else us_div <= us_div + 7'd1;
		cycles <= cycles + 32'd1;
		if (ms_cnt == MS_DIV - 17'd1) begin ms_cnt <= 17'd0; ms <= ms + 32'd1; end
		else ms_cnt <= ms_cnt + 17'd1;
	end
	if (kbd_wr) kbd_n <= kbd_n + 32'd1;
	if (ms_wr) begin
		ms_dx <= sat_add(ms_dx, ms_x);
		ms_dy <= sat_add(ms_dy, ms_y);
		ms_btn <= ms_b;
	end

	if (io_req && d_we) begin
		io_ack <= 1'b1;
		if (io_off[11:10] == 2'b01) begin            // 0x400..0x7fc
			pal_we <= 1'b1;
			pal_wa <= io_off[9:2];
			pal_wd <= d_dat_o[23:0];
		end else if (io_off[11:8] == 4'h1) begin     // 0x100..0x1fc
			row_we <= 1'b1;
			row_wa <= io_off[7:2];
			row_wd <= d_dat_o[11:0];
		end else case (io_off[7:0])
			8'h00: begin dbg_byte <= d_dat_o[7:0]; dbg_wr <= 1'b1; end
			8'h08: dbg_exit <= { 1'b1, d_dat_o[7:0] };
			8'h44: tcmp <= d_dat_o;
			8'h48: ten <= d_dat_o[0];
			8'h64: if (!ser_in_full) begin ser_in_data <= d_dat_o[7:0]; ser_in_wr <= 1'b1; end
			8'h80: def_fg <= d_dat_o[23:0];
			8'h84: def_bg <= d_dat_o[23:0];
			8'h88: begin
				cur_x <= d_dat_o[6:0];
				cur_y <= d_dat_o[13:8];
				cur_on <= d_dat_o[15];
				cur_shape <= d_dat_o[17:16];
				cur_blink <= d_dat_o[24];
			end
			8'h8c: show <= d_dat_o[0];
			default: ;
		endcase
	end else if (io_req && io_off[11:8] == 4'h0 && io_off[7:0] == 8'h60 && !io_wait) begin
		io_wait <= 1'b1;                             // rx_q = rx_mem[rx_rp] in the next clock
	end else if (io_req) begin
		io_ack <= 1'b1;
		io_wait <= 1'b0;
		case (io_off[7:0])
			8'h0c: io_dat <= cycles;
			8'h1c: io_dat <= ms;
			8'h28: io_dat <= kbd[31:0];
			8'h2c: io_dat <= { 8'd0, kbd[55:32] };
			8'h30: io_dat <= kbd_n;
			8'h34: begin
				io_dat <= { ms_btn, ms_dy, ms_dx };
				if (ms_wr) begin
					ms_dx <= sat_add(12'sd0, ms_x);
					ms_dy <= sat_add(12'sd0, ms_y);
				end else begin
					ms_dx <= 12'sd0;
					ms_dy <= 12'sd0;
				end
			end
			8'h40: io_dat <= us;
			8'h50: io_dat <= sw;
			8'h60: begin
				if (rx_n != 9'd0) begin
					io_dat <= { 23'd0, 1'b1, rx_q };
					rx_rp <= rx_rp + 9'd1;
				end else
					io_dat <= 32'd0;
			end
			8'h68: io_dat <= { 30'd0, ser_in_full, rx_n != 9'd0 };
			8'h6c: io_dat <= ser_cfg;
			8'h90: io_dat <= { 16'd0, frames };
			default: io_dat <= 32'd0;
		endcase
	end

	// loader
	if (ld_wr && !ld_pend && ld_kind == 2'd0) begin
		ld_pend <= 1'b1;
		ld_d <= ld_data;
		ld_a <= ld_addr[21:0];
	end else if (ld_wr && !ld_pend && ld_kind == 2'd2 && ld_slot == 8'd0 && ld_addr[31:21] == 11'd0) begin
		ld_pend <= 1'b1;
		ld_d <= ld_data;
		ld_a <= { 1'b1, ld_addr[20:0] };          // 0x200000 + the address in the file
	end
	if (own == O_LD && sr_ack) ld_pend <= 1'b0;
end

// ------------------------------------------------------------------ CPU bus answers

always @(*) begin
	i_ack = (own == O_I) & sr_ack;
	i_dat_i = sr_dat;
	d_ack = ((own == O_D) & sr_ack) | io_ack;
	d_dat_i = d_io ? io_dat : sr_dat;
end

endmodule
