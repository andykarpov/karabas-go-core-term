`timescale 1ns / 1ps
`default_nettype none
/*
 term_sd.v (from doom_sd.v of karabas-go-core-doom, the registers moved to 0xc0..0xd0)

 The save file of the term core (the phonebook, docs/PLAN.md Ф2): the CPU asks the firmware for it with the backup
 RAM request of the MCU link v2 (BK_MOUNT, kind 0: <save_dir>/<the FL file name>.sav = /term/font.sav, made
 with the asked size; karabas-go-firmware bk_save.c), then reads and writes its sectors through the
 mounter of kg_mcu2 (SDC_*, slot SLOT). The firmware (src/fw/book.c) keeps the phonebook in it. Every write goes to the file at once, so a flush of the MCU
 (IMG_FLUSH: before another core or game) is answered as soon as no sector is in progress.

 MMIO word registers (offsets of term_soc):
   0xc0 W  SECTOR  the sector of the next command
   0xc4 W  CMD     1: read the sector into the buffer, 2: write the buffer to the sector
   0xc8 R  STAT    bit 0 busy, bit 1 the last command failed, bit 2 mounted (a file of size > 0)
   0xcc R  SIZE    the size of the mounted file in bytes (low 32 bits)
   0xd0 W  BK_REQ  ask for the save file: the value = its size in sectors
   0x1000 + 4 * i  R/W  byte i of the 512-byte sector buffer (bits 7..0)
 The buffer: 512 x 8 distributed dual-port RAM: port A writes (the CPU, or the bytes of a read
 from the MCU) and reads for the CPU, port B reads the byte of a write for the MCU (two clocks
 after sdc_byte_addr: with the address register of kg_l2_img that is kg_mcu2 IMG_OUT_LATENCY 3).
*/
module term_sd #(
	parameter [2:0] SLOT = 3'd1
) (
	input wire clk,
	input wire reset,

	input wire io_we,
	input wire [13:0] io_off,
	input wire [31:0] io_wd,
	output reg [31:0] io_rd,

	// kg_mcu2 (ENABLE_IMG, CAPS bit 1)
	input wire [7:0] sdc_img_mounted,
	input wire [63:0] sdc_img_size,
	output wire [7:0] sdc_rd,
	output wire [7:0] sdc_wr,
	output wire [31:0] sdc_sector,
	input wire sdc_busy,
	input wire sdc_done,
	input wire sdc_error,
	input wire [2:0] sdc_slot,
	input wire sdc_byte_in_strobe,
	input wire [8:0] sdc_byte_addr,
	input wire [7:0] sdc_byte_in_data,
	output reg [7:0] sdc_byte_out_data = 8'd0,
	input wire [7:0] img_flush,
	output reg bk_mount = 1'b0,
	output reg bk_flushed = 1'b0,
	output reg [31:0] bk_size = 32'd0,
	output wire [7:0] bk_kind,
	output wire [7:0] bk_result,
	output wire [7:0] dbg              // { rd_req, wr_req, busy, wait_done, err, mounted, sdc_busy, sdc_slot is SLOT }
);

assign bk_kind = 8'd0;
assign bk_result = 8'h01;                 // done: nothing is held back

wire mine = sdc_slot == SLOT;
assign dbg = { rd_req, wr_req, busy, wait_done, err, size != 64'd0, sdc_busy, mine };
wire buf_sel = io_off[13:11] == 3'b010;   // 0x1000..0x17ff

reg [7:0] sbuf [0:511];
reg [31:0] sector = 32'd0;
reg rd_req = 1'b0, wr_req = 1'b0;
reg busy = 1'b0, wait_done = 1'b0, err = 1'b0;
reg [63:0] size = 64'd0;
reg flush_pend = 1'b0;

assign sdc_rd = { 7'd0, rd_req } << SLOT;
assign sdc_wr = { 7'd0, wr_req } << SLOT;
assign sdc_sector = sector;

// port A (write and an asynchronous read): a byte of a read from the MCU, else the CPU (it reads and
// writes the buffer only while no command runs); port B (read): the byte of a write for the MCU.
// Two ports only, or XST makes the buffer of flip-flops (16k LUTs)
wire mcu_w = busy && wait_done && sdc_byte_in_strobe && mine;
wire [8:0] a_addr = mcu_w ? sdc_byte_addr : io_off[10:2];
wire [7:0] a_q = sbuf[a_addr];
always @(posedge clk) begin
	if (mcu_w)
		sbuf[a_addr] <= sdc_byte_in_data;
	else if (io_we && buf_sel && !busy)
		sbuf[a_addr] <= io_wd[7:0];
end

// the byte for the MCU (a write): two clocks after its address
reg [7:0] out1 = 8'd0;
always @(posedge clk) begin
	out1 <= sbuf[sdc_byte_addr];
	sdc_byte_out_data <= out1;
end

always @(*) begin
	if (buf_sel) io_rd = { 24'd0, a_q };
	else case (io_off)
		14'h00c8: io_rd = { 29'd0, size != 64'd0, err, busy };
		14'h00cc: io_rd = size[31:0];
		default: io_rd = 32'd0;
	endcase
end

always @(posedge clk) begin
	bk_mount <= 1'b0;
	bk_flushed <= 1'b0;
	if (img_flush[SLOT]) flush_pend <= 1'b1;
	if (sdc_img_mounted[SLOT]) size <= sdc_img_size;
	if (reset) begin
		busy <= 1'b0;
		wait_done <= 1'b0;
		rd_req <= 1'b0;
		wr_req <= 1'b0;
		err <= 1'b0;
		size <= 64'd0;
	end else begin
		if (io_we && io_off == 14'h00c0) sector <= io_wd;
		if (io_we && io_off == 14'h00d0) begin
			bk_size <= io_wd;
			bk_mount <= 1'b1;
		end
		if (io_we && io_off == 14'h00c4 && !busy && size != 64'd0) begin
			busy <= 1'b1;
			wait_done <= 1'b0;
			err <= 1'b0;
			rd_req <= io_wd[0];
			wr_req <= io_wd[1];
		end
		// the request is held until the MCU takes it (busy of this slot), then its end (done)
		if (busy && !wait_done && sdc_busy && mine) begin
			rd_req <= 1'b0;
			wr_req <= 1'b0;
			wait_done <= 1'b1;
		end
		if (busy && wait_done && sdc_done && mine) begin
			busy <= 1'b0;
			wait_done <= 1'b0;
			err <= sdc_error;
		end
		// a flush: every write is in the file already; answer when no sector is in progress
		if (flush_pend && !busy) begin
			flush_pend <= 1'b0;
			bk_flushed <= 1'b1;
		end
	end
end

endmodule
