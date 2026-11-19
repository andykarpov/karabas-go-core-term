`timescale 1ns / 1ps
`default_nettype wire
/*
 term core for Karabas Mini before rev.G (HDMI, sigma-delta audio). Made by tools/mktops.py
 from karabas_minig_top.v (do not edit: change that one and run it): the same except the pins,
 the MCU link (MCU_SCK not on a clock pin) and the audio (AUDIO_L / AUDIO_R).
 term_soc (VexRiscv + libvterm firmware, docs/PLAN.md)
 with the board glue of karabas-go-modules (after the top of karabas-go-core-doom).
 Clocks: one PLL, VCO 650 MHz (50 * 13), as core-atari2600: clk_hdmi / clk_hdmi_n 325 MHz (the serialiser),
 clk_sys 65 MHz (the SoC, the pixels of 1024 x 768 at 60 Hz, the OSD, HDMI, the MCU link), clk_8 8.02 MHz
 (FT_CLK_OUT).
 Memory: the SRAM only. The SDRAM is never used (its activity disturbs USB, docs/PLAN.md): no clock, NOP.
 Serial: the USB serial port of the MCU = kg_mcu2 port 0; the ESP8266 on UART_RX / UART_TX / UART_CTS
 (term_uart in term_soc, CTS active 0).
 Soft switches (the index of the item in the osd of core.json): 0 Font (the FL item), 1 Reset.
*/
module karabas_mini_top (
	input wire CLK_50MHZ,

	inout wire UART_RX,
	inout wire UART_TX,
	inout wire UART_CTS,
	inout wire ESP_RESET_N,
	inout wire ESP_BOOT_N,

	output wire [20:0] MA,
	inout wire [15:0] MD,
	output wire [1:0] MWR_N,
	output wire [1:0] MRD_N,

	output wire [1:0] SDR_BA,
	output wire [12:0] SDR_A,
	output wire SDR_CLK,
	output wire [1:0] SDR_DQM,
	output wire SDR_WE_N,
	output wire SDR_CAS_N,
	output wire SDR_RAS_N,
	inout wire [15:0] SDR_DQ,

	output wire SD_CS_N,
	output wire SD_CLK,
	output wire SD_DI,
	input wire SD_DO,
	input wire SD_DET_N,

	input wire [7:0] VGA_R,
	input wire [7:0] VGA_G,
	input wire [7:0] VGA_B,
	input wire VGA_HS,
	input wire VGA_VS,

	output wire [3:0] TMDS_P,
	output wire [3:0] TMDS_N,

	output wire FT_SPI_CS_N,
	output wire FT_SPI_SCK,
	input wire FT_SPI_MISO,
	output wire FT_SPI_MOSI,
	input wire FT_INT_N,
	input wire FT_CLK,
	input wire FT_DE,
	input wire FT_AUDIO,
	input wire FT_DISP,
	output wire FT_RESET,
	output wire FT_CLK_OUT,

	output wire [2:0] WA,
	output wire [1:0] WCS_N,
	output wire WRD_N,
	output wire WWR_N,
	output wire WRESET_N,
	inout wire [15:0] WD,

	output wire TAPE_OUT,
	input wire TAPE_IN,

	output wire AUDIO_L,
	output wire AUDIO_R,

	output wire ADC_CLK,
	inout wire ADC_BCK,
	inout wire ADC_LRCK,
	input wire ADC_DOUT,

	input wire MCU_CS_N,
	input wire MCU_SCK,
	inout wire MCU_MOSI,
	output wire MCU_MISO,
	inout wire [3:0] MCU_IO, // [1] INT of link v2, [2] sd2 cs_n, [3] ft cs_n

	output wire MIDI_TX,

	output wire MIDI_CLK,
	output wire MIDI_RESET_N,

	output wire FLASH_CS_N,
	input wire FLASH_DO,
	output wire FLASH_DI,
	output wire FLASH_SCK,
	output wire FLASH_WP_N,
	output wire FLASH_HOLD_N
);

// ------------------------------------------------------------------ unused

assign WA = 3'b000;
assign WCS_N = 2'b11;
assign WRD_N = 1'b1;
assign WWR_N = 1'b1;
assign WRESET_N = 1'b1;
assign WD = 16'bz;
assign ESP_RESET_N = 1'bz;
assign ESP_BOOT_N = 1'bz;
assign TAPE_OUT = 1'b0;
assign FT_RESET = 1'b1;
assign MIDI_TX = 1'b1;
assign MIDI_CLK = 1'b0;      // the SAM2695 is not used: in reset
assign MIDI_RESET_N = 1'b0;
assign ADC_CLK = 1'b0;
assign ADC_BCK = 1'bz;
assign ADC_LRCK = 1'bz;
// the SPI flash: T3 (FLASH_CS_N) is the SDRAM ~CS on rev G2: driven 0 by all cores
assign FLASH_CS_N = 1'b0;
assign FLASH_DI = 1'b1;
assign FLASH_SCK = 1'b0;
assign FLASH_WP_N = 1'b1;
assign FLASH_HOLD_N = 1'b1;
// the SDRAM: no clock, NOP, data lines released
assign SDR_CLK = 1'b0;
assign SDR_BA = 2'b00;
assign SDR_A = 13'd0;
assign SDR_DQM = 2'b11;
assign SDR_WE_N = 1'b1;
assign SDR_CAS_N = 1'b1;
assign SDR_RAS_N = 1'b1;
assign SDR_DQ = 16'bz;

// ------------------------------------------------------------------ clocks

wire [5:0] pll_clk;
wire pll_locked;
kg_pll #(
	.CLKIN_PERIOD(20.0),
	.DIVCLK_DIVIDE(1),
	.CLKFBOUT_MULT(13),      // VCO 650 MHz
	.CLKOUT0_DIVIDE(2),      // clk_hdmi 325 MHz
	.CLKOUT1_DIVIDE(2),      // clk_hdmi_n 325 MHz, 180 degrees
	.CLKOUT1_PHASE(180.0),
	.CLKOUT2_DIVIDE(10),     // clk_sys 65 MHz
	.CLKOUT3_DIVIDE(81),     // clk_8 8.02 MHz
	.CLKOUT4_DIVIDE(10),
	.CLKOUT5_DIVIDE(10),
	.BUFG_MASK(6'b001111)
) pll (
	.clk_in(CLK_50MHZ),
	.rst(1'b0),
	.clk_out(pll_clk),
	.locked(pll_locked)
);
wire clk_hdmi = pll_clk[0];
wire clk_hdmi_n = pll_clk[1];
wire clk_sys = pll_clk[2];
wire clk_8 = pll_clk[3];

(* KEEP = "TRUE" *) wire areset_async = ~pll_locked;
wire areset;
kg_sync #(.W(1), .STAGES(2), .INIT(1'b1)) sync_areset (.clk(clk_sys), .d(areset_async), .q(areset));

ODDR2 oddr_ft_clk (.Q(FT_CLK_OUT), .C0(clk_8), .C1(~clk_8), .CE(1'b1), .D0(1'b1), .D1(1'b0), .R(1'b0), .S(1'b0));

// ------------------------------------------------------------------ MCU (link v2)

// MCU_SCK is not on a clock pin before rev.G (CLOCK_DEDICATED_ROUTE = FALSE in the ucf)
wire mcu_sck = MCU_SCK, mcu_sck_ibufg = MCU_SCK;
wire mcu_int;
assign MCU_IO[1] = mcu_int;
assign MCU_IO[0] = 1'bz;
assign MCU_IO[3:2] = 2'bzz;

wire [55:0] kbd;
wire kbd_wr;
wire [7:0] ms_x, ms_y, ms_z, ms_b;
wire ms_wr;
wire [1:0] ld_kind;
wire [7:0] ld_slot, ld_data;
wire [31:0] ld_addr;
wire ld_active, ld_wr, ld_ready;
wire [8*2-1:0] sw;
wire dvi_only;
wire ft_cs_n, ft_mosi, ft_spi_on, sd2_cs_n, sd2_mosi;
wire [15:0] dbg_addr;
reg [15:0] dbg_data = 16'd0;
wire osd_wr, osd_en, osd_zoom;
wire [11:0] osd_waddr;
wire [7:0] osd_wdata;
wire [23:0] osd_fg, osd_bg;
wire [1:0] osd_alpha, osd_place;
wire [4:0] osd_px, osd_ph;
wire [3:0] osd_py;
wire [5:0] osd_pw;
wire [31:0] port_out_data;
wire [3:0] port_out_wr, port_in_full;
wire ser_out_ready, ser_in_wr;
wire [7:0] ser_in_data;
wire [127:0] port_cfg;
wire [7:0] sdc_img_mounted, sdc_rd, sdc_wr, sdc_byte_in_data, sdc_byte_out_data, img_flush, bk_kind, bk_result;
wire [63:0] sdc_img_size;
wire [31:0] sdc_sector, bk_size;
wire sdc_busy, sdc_done, sdc_error, sdc_byte_in_strobe, bk_mount, bk_flushed;
wire [2:0] sdc_slot;
wire [8:0] sdc_byte_addr;

kg_mcu2 #(
	.SW_N(2),
	.CS_FILTER(3),
	.CAPS(16'h0012),     // bit 4: the core drives LD_READY, bit 1: backup RAM (the phonebook file, term_sd)
	.ENABLE_OSD(1),
	.ENABLE_IMG(1),
	.IMG_OUT_LATENCY(3), // the address register of kg_l2_img + term_sd (two registers), as core-doom
	.ENABLE_PORTS(4'b0001), // port 0: the USB serial port
	.PORT_OUT_AW(6),
	.PORT_IN_AW(6),
	.ENABLE_AUX(1),      // DEBUG_*: the state of the core (AT+KARABAS=debug:1, kg_board.py log)
	.ENABLE_FT(1),
	.ENABLE_SD2(1)
) mcu (
	.CLK(clk_sys),
	.N_RESET(~areset),
	.MCU_SCK(mcu_sck),
	.MCU_MOSI(MCU_MOSI),
	.MCU_MISO(MCU_MISO),
	.MCU_SS(MCU_CS_N),
	.MCU_INT(mcu_int),
	.MCU_SPI_FT_SS(MCU_IO[3]),
	.MCU_SPI_SD2_SS(MCU_IO[2]),
	.SW(sw), .SW_WR(), .SW_INDEX(), .SW_VALUE(), .HW_ID(), .DVI_ONLY(dvi_only), .CORE_BUILD(), .FW_BUILD(),
	.BUSY(), .BTNS(), .RELOAD_REQ(1'b0), .RELOAD_CORE(8'd0),
	.KBD(kbd), .KBD_WR(kbd_wr),
	.PS2_CODE(), .PS2_KIND(), .PS2_WR(),
	.MS_X(ms_x), .MS_Y(ms_y), .MS_Z(ms_z), .MS_B(ms_b), .MS_WR(ms_wr),
	.JOY_L(), .JOY_R(), .JOY_WR(),
	.PS2_CMD(16'h0000), .PS2_CMD_WR(1'b0),
	.LD_KIND(ld_kind), .LD_SLOT(ld_slot), .LD_SIZE(), .LD_EXT(), .LD_START(), .LD_ACTIVE(ld_active), .LD_END(),
	.LD_ADDR(ld_addr), .LD_DATA(ld_data), .LD_WR(ld_wr), .LD_READY(ld_ready),
	.OSD_WR(osd_wr), .OSD_WADDR(osd_waddr), .OSD_WDATA(osd_wdata), .OSD_EN(osd_en),
	.OSD_FG(osd_fg), .OSD_BG(osd_bg), .OSD_ALPHA(osd_alpha), .OSD_PX(osd_px), .OSD_PY(osd_py),
	.OSD_PW(osd_pw), .OSD_PH(osd_ph), .OSD_PLACE(osd_place), .OSD_ZOOM(osd_zoom),
	.SDC_IMG_MOUNTED(sdc_img_mounted), .SDC_IMG_SIZE(sdc_img_size), .SDC_RD(sdc_rd), .SDC_WR(sdc_wr),
	.SDC_SECTOR(sdc_sector), .SDC_BUSY(sdc_busy), .SDC_DONE(sdc_done), .SDC_ERROR(sdc_error), .SDC_SLOT(sdc_slot),
	.SDC_BYTE_IN_STROBE(sdc_byte_in_strobe), .SDC_BYTE_ADDR(sdc_byte_addr), .SDC_BYTE_IN_DATA(sdc_byte_in_data),
	.SDC_BYTE_OUT_DATA(sdc_byte_out_data), .IMG_WP(), .IMG_PHYS_WR(), .IMG_PHYS(), .IMG_FLUSH(img_flush), .FDD_CHNG_WR(), .FDD_CHNG(),
	.BK_MOUNT(bk_mount), .BK_FLUSHED(bk_flushed), .BK_SLOT(3'd1), .BK_SIZE(bk_size), .BK_KIND(bk_kind), .BK_RESULT(bk_result),
	.FDD_CHNG_REQ(1'b0), .FDD_CHNG_DS(2'd0), .IMG_ACTIVE(),
	.PORT_OUT_DATA(port_out_data), .PORT_OUT_WR(port_out_wr), .PORT_OUT_READY({ 3'b111, ser_out_ready }),
	.PORT_IN_DATA({ 24'd0, ser_in_data }), .PORT_IN_WR({ 3'b000, ser_in_wr }),
	.PORT_CFG(128'd0), .PORT_IN_FULL(port_in_full), .PORT_STAT(), .PORT_STAT_WR(),
	.RTC_A(8'd0), .RTC_DI(8'd0), .RTC_CS(1'b0), .RTC_WR_N(1'b1), .MATRIX_CMD(24'd0), .MATRIX_CMD_WR(1'b0),
	.DEBUG_ADDR(dbg_addr), .DEBUG_DATA(dbg_data), .RTC_DO(),
	.FT_SPI_ON(ft_spi_on), .FT_VGA_ON(), .FT_RESET(), .FT_SCK(), .FT_MOSI(ft_mosi), .FT_MISO(FT_SPI_MISO),
	.FT_CS_N(ft_cs_n),
	.SD2_SCK(), .SD2_MOSI(sd2_mosi), .SD2_MISO(SD_DO), .SD2_CS_N(sd2_cs_n)
);

assign FT_SPI_CS_N = ft_spi_on ? ft_cs_n : 1'b1;
assign FT_SPI_SCK = ft_spi_on ? mcu_sck_ibufg : 1'b1;
assign FT_SPI_MOSI = ft_spi_on ? ft_mosi : 1'b1;
assign SD_CS_N = sd2_cs_n;
assign SD_DI = sd2_mosi;
assign SD_CLK = mcu_sck_ibufg;

wire sw_reset = sw[1*8];

// ------------------------------------------------------------------ the SoC

wire [15:0] md_o, md_oe_n;
genvar gi;
generate
	for (gi = 0; gi < 16; gi = gi + 1) begin : g_io
		assign MD[gi] = md_oe_n[gi] ? 1'bz : md_o[gi];
	end
endgenerate

wire esp_txd, esp_cts_n;
assign UART_TX = esp_txd;
assign UART_CTS = esp_cts_n;
wire [23:0] vid_rgb;
wire vid_hs, vid_vs, vid_de, cpu_run, dbg_wr, bell;
wire [7:0] dbg_byte;
wire [8:0] dbg_exit;

term_soc soc (
	.clk(clk_sys),
	.reset(areset | sw_reset),
	.ld_kind(ld_kind), .ld_slot(ld_slot), .ld_active(ld_active),
	.ld_addr(ld_addr), .ld_data(ld_data), .ld_wr(ld_wr), .ld_ready(ld_ready),
	.kbd(kbd), .kbd_wr(kbd_wr), .ms_x(ms_x), .ms_y(ms_y), .ms_z(ms_z), .ms_b(ms_b), .ms_wr(ms_wr),
	.sw({ 16'd0, sw }),
	.ser_out_data(port_out_data[7:0]), .ser_out_wr(port_out_wr[0]), .ser_out_ready(ser_out_ready),
	.ser_in_data(ser_in_data), .ser_in_wr(ser_in_wr), .ser_in_full(port_in_full[0]), .ser_cfg(32'd0),
	.esp_rxd(UART_RX), .esp_txd(esp_txd), .esp_cts_n(esp_cts_n),
	.sdc_img_mounted(sdc_img_mounted), .sdc_img_size(sdc_img_size), .sdc_rd(sdc_rd), .sdc_wr(sdc_wr),
	.sdc_sector(sdc_sector), .sdc_busy(sdc_busy), .sdc_done(sdc_done), .sdc_error(sdc_error), .sdc_slot(sdc_slot),
	.sdc_byte_in_strobe(sdc_byte_in_strobe), .sdc_byte_addr(sdc_byte_addr), .sdc_byte_in_data(sdc_byte_in_data),
	.sdc_byte_out_data(sdc_byte_out_data), .img_flush(img_flush),
	.bk_mount(bk_mount), .bk_flushed(bk_flushed), .bk_size(bk_size), .bk_kind(bk_kind), .bk_result(bk_result),
	.rgb(vid_rgb), .hs(vid_hs), .vs(vid_vs), .de(vid_de),
	.dbg_byte(dbg_byte), .dbg_wr(dbg_wr), .dbg_exit(dbg_exit), .cpu_run(cpu_run), .bell(bell),
	.MA(MA), .MD_I(MD), .MD_O(md_o), .MD_OE_N(md_oe_n), .MWR_N(MWR_N), .MRD_N(MRD_N)
);

// ------------------------------------------------------------------ OSD, HDMI

// term_video gives negative syncs; the overlay wants them high active (inverted here, back for HDMI)
wire [23:0] osd_rgb;
wire osd_hs, osd_vs, osd_de;
kg_l2_osd_overlay osd (
	.wclk(clk_sys),
	.osd_wr(osd_wr), .osd_waddr(osd_waddr), .osd_wdata(osd_wdata), .osd_en(osd_en),
	.osd_fg(osd_fg), .osd_bg(osd_bg), .osd_alpha(osd_alpha), .osd_px(osd_px), .osd_py(osd_py),
	.osd_pw(osd_pw), .osd_ph(osd_ph), .osd_place(osd_place), .osd_zoom(osd_zoom),
	.icon_on(8'd0), .icon_act(8'd0),
	.clk(clk_sys),
	.rgb_i(vid_rgb), .hs_i(~vid_hs), .vs_i(~vid_vs), .de_i(vid_de),
	.rgb_o(osd_rgb), .hs_o(osd_hs), .vs_o(osd_vs), .de_o(osd_de)
);

// the bell: a square wave of 880 Hz (a half period of 36932 clocks at 65 MHz), 120 ms
reg [16:0] bell_div = 17'd0;
reg [23:0] bell_left = 24'd0;
reg bell_sq = 1'b0;
always @(posedge clk_sys) begin
	if (bell) bell_left <= 24'd7_800_000;
	else if (bell_left != 24'd0) bell_left <= bell_left - 24'd1;
	if (bell_div == 17'd36931) begin bell_div <= 17'd0; bell_sq <= ~bell_sq; end
	else bell_div <= bell_div + 17'd1;
end
wire signed [15:0] snd = bell_left == 24'd0 ? 16'sd0 : (bell_sq ? 16'sd6000 : -16'sd6000);

// 48 kHz audio strobes
reg [16:0] aud_acc = 17'd0;
reg aud_stb = 1'b0;
always @(posedge clk_sys) begin
	aud_stb <= 1'b0;
	if (aud_acc >= 17'd64952) begin
		aud_acc <= aud_acc + 17'd48 - 17'd65000;
		aud_stb <= 1'b1;
	end else
		aud_acc <= aud_acc + 17'd48;
end
reg dvi_only_p = 1'b0;
always @(posedge clk_sys)
	dvi_only_p <= dvi_only;

reg hdmi_hs = 1'b1, hdmi_vs = 1'b1, hdmi_de = 1'b0;
reg [23:0] hdmi_rgb = 24'd0;
always @(posedge clk_sys) begin
	hdmi_hs <= ~osd_hs;
	hdmi_vs <= ~osd_vs;
	hdmi_de <= osd_de;
	hdmi_rgb <= osd_rgb;
end

hdmi_tx #(.SAMPLE_FREQ(48000), .INPUT_REG(1), .OUTPUT_REG(1)) hdmi (
	.clk(clk_sys),
	.sclk(clk_hdmi),
	.sclk_n(clk_hdmi_n),
	.reset(areset),
	.rgb(hdmi_rgb),
	.vsync(hdmi_vs),
	.hsync(hdmi_hs),
	.de(hdmi_de),
	.audio_en(~dvi_only_p),
	.audio_l({ snd, 8'd0 }),
	.audio_r({ snd, 8'd0 }),
	.audio_clk(aud_stb),
	.tx_clk_n(TMDS_N[3]),
	.tx_clk_p(TMDS_P[3]),
	.tx_d_n(TMDS_N[2:0]),
	.tx_d_p(TMDS_P[2:0])
);

// no I2S DAC on this board: the bell as sigma-delta into an RC filter (offset binary)
kg_sigma_delta_dac dac_l (.clk(clk_sys), .reset(areset), .din(snd ^ 16'h8000), .dout(AUDIO_L));
kg_sigma_delta_dac dac_r (.clk(clk_sys), .reset(areset), .din(snd ^ 16'h8000), .dout(AUDIO_R));

// ------------------------------------------------------------------ debug
// DEBUG_ADDR { cpu_run, exit seen, 6'd0, the last debug console byte }, DEBUG_DATA { debug bytes count }
reg [15:0] dbg_n = 16'd0;
always @(posedge clk_sys) begin
	if (dbg_wr) dbg_n <= dbg_n + 16'd1;
	dbg_data <= dbg_n;
end
assign dbg_addr = { cpu_run, dbg_exit[8], 6'd0, dbg_byte };

endmodule
