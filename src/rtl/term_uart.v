`timescale 1ns / 1ps
`default_nettype none
/*
 term_uart.v

 The UART of the ESP8266 module (docs/PLAN.md Ф2): 8N1 at clk / CLKS_PER_BIT (65 MHz / 564 = 115248 baud,
 0.04 % off 115200), a receive fifo of 2048 bytes in a block RAM, CTS to the module (active 0: the module
 sends while it is 0; 1 when more than 1792 bytes wait, as esp8266.vhd of core-menu), a transmit holding
 register (tx_busy while a byte is out).
 Read side as the serial port 0 of term_soc: rd_req one clock asks; rd_valid / rd_data come two clocks
 later, taken from the state of the first clock (rd_valid 0: the fifo was empty).
*/
module term_uart #(
	parameter [9:0] CLKS_PER_BIT = 10'd564
) (
	input wire clk,
	input wire reset,

	input wire rxd,                   // from the module (the board pin UART_RX)
	output reg txd = 1'b1,            // to the module (UART_TX)
	output wire cts_n,                // to the module: 0 = send

	input wire tx_wr,
	input wire [7:0] tx_data,
	output wire tx_busy,

	input wire rd_req,
	output reg rd_valid = 1'b0,
	output reg [7:0] rd_data = 8'd0,
	output wire [11:0] rx_count,
	output reg rx_overrun = 1'b0      // a byte came with the fifo full (sticky until reset)
);

// ------------------------------------------------------------------ receive

reg [2:0] rx_s = 3'b111;          // synchroniser
always @(posedge clk) rx_s <= { rx_s[1:0], rxd };
wire rx = rx_s[2];

reg [1:0] r_st = 2'd0;            // 0 idle, 1 start, 2 data, 3 stop
reg [9:0] r_cnt = 10'd0;
reg [2:0] r_bit = 3'd0;
reg [7:0] r_sh = 8'd0;
reg r_done = 1'b0;

always @(posedge clk) begin
	r_done <= 1'b0;
	if (reset) r_st <= 2'd0;
	else case (r_st)
	2'd0: if (!rx) begin r_st <= 2'd1; r_cnt <= { 1'b0, CLKS_PER_BIT[9:1] }; end
	2'd1: if (r_cnt == 10'd0) begin
			if (!rx) begin r_st <= 2'd2; r_cnt <= CLKS_PER_BIT - 10'd1; r_bit <= 3'd0; end
			else r_st <= 2'd0;               // a glitch
		end else r_cnt <= r_cnt - 10'd1;
	2'd2: if (r_cnt == 10'd0) begin
			r_sh <= { rx, r_sh[7:1] };
			r_cnt <= CLKS_PER_BIT - 10'd1;
			if (r_bit == 3'd7) r_st <= 2'd3;
			r_bit <= r_bit + 3'd1;
		end else r_cnt <= r_cnt - 10'd1;
	2'd3: if (r_cnt == 10'd0) begin
			r_st <= 2'd0;
			r_done <= rx;                    // a stop bit 0 (framing error): the byte is dropped
		end else r_cnt <= r_cnt - 10'd1;
	endcase
end

reg [7:0] mem [0:2047];
reg [10:0] wp = 11'd0, rp = 11'd0;
wire [11:0] n = { 1'b0, wp } - { 1'b0, rp };
wire [10:0] n11 = wp - rp;
assign rx_count = { 1'b0, n11 };
assign cts_n = n11 > 11'd1792;
reg [7:0] q = 8'd0;
reg rd_av = 1'b0, rd_p = 1'b0;

always @(posedge clk) begin
	q <= mem[rp];
	rd_valid <= 1'b0;
	if (reset) begin
		wp <= 11'd0;
		rp <= 11'd0;
		rx_overrun <= 1'b0;
		rd_p <= 1'b0;
	end else begin
		if (r_done) begin
			if (n11 != 11'd2047) begin
				mem[wp] <= r_sh;
				wp <= wp + 11'd1;
			end else
				rx_overrun <= 1'b1;
		end
		// a read: the state of this clock (q is mem[rp] of this clock in the next one)
		rd_p <= rd_req;
		if (rd_req) rd_av <= n11 != 11'd0;
		if (rd_p) begin
			rd_valid <= rd_av;
			rd_data <= q;
			if (rd_av) rp <= rp + 11'd1;
		end
	end
end

// ------------------------------------------------------------------ transmit

reg [9:0] t_cnt = 10'd0;
reg [3:0] t_bit = 4'd0;
reg [8:0] t_sh = 9'h1ff;
reg t_on = 1'b0;
assign tx_busy = t_on;

always @(posedge clk) begin
	if (reset) begin
		t_on <= 1'b0;
		txd <= 1'b1;
	end else if (!t_on) begin
		txd <= 1'b1;
		if (tx_wr) begin
			t_on <= 1'b1;
			t_sh <= { tx_data, 1'b0 };       // start bit first
			t_bit <= 4'd0;
			t_cnt <= 10'd0;
		end
	end else if (t_cnt == 10'd0) begin
		if (t_bit == 4'd10) t_on <= 1'b0;    // after the stop bit
		else begin
			txd <= t_bit == 4'd9 ? 1'b1 : t_sh[0];
			t_sh <= { 1'b1, t_sh[8:1] };
			t_bit <= t_bit + 4'd1;
			t_cnt <= CLKS_PER_BIT - 10'd1;
		end
	end else
		t_cnt <= t_cnt - 10'd1;
end

endmodule
