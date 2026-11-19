`timescale 1ns / 1ps
`default_nettype wire
/*
 term core for Karabas Go (VGA, PCM5102). Made by tools/mktops.py from karabas_minig_top.v
 (do not edit: change that one and run it): the same except the pins, the MCU link (MCU_SCK not
 on a clock pin) and the video: 24 bit RGB to the VGA DAC with its pixel clock on V_CLK (clk_sys,
 65 MHz), the FT812 off that bus (FT_OE_N = 1), no HDMI. The floppy connector is not used.
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
module karabas_go_top (
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

	output wire [7:0] VGA_R,
	output wire [7:0] VGA_G,
	output wire [7:0] VGA_B,
	output wire VGA_HS,
	output wire VGA_VS,
	output wire V_CLK,

	output wire FT_SPI_CS_N,
	output wire FT_SPI_SCK,
	input wire FT_SPI_MISO,
	output wire FT_SPI_MOSI,
	input wire FT_INT_N,
	input wire FT_CLK,
	output wire FT_OE_N,

	output wire [2:0] WA,
	output wire [1:0] WCS_N,
	output wire WRD_N,
	output wire WWR_N,
	output wire WRESET_N,
	inout wire [15:0] WD,

	input wire FDC_INDEX,
	output wire [1:0] FDC_DRIVE,
	output wire FDC_MOTOR,
	output wire FDC_DIR,
	output wire FDC_STEP,
	output wire FDC_WDATA,
	output wire FDC_WGATE,
	input wire FDC_TR00,
	input wire FDC_WPRT,
	input wire FDC_RDATA,
	output wire FDC_SIDE_N,

	output wire TAPE_OUT,
	input wire TAPE_IN,
	output wire BEEPER,

	output wire DAC_LRCK,
	output wire DAC_DAT,
	output wire DAC_BCK,
	output wire DAC_MUTE,

	input wire MCU_CS_N,
	input wire MCU_SCK,
	inout wire MCU_MOSI,
	output wire MCU_MISO,
	input wire MCU_SPI_FT_CS_N,
	input wire MCU_SPI_SD2_CS_N,
	inout wire [1:0] MCU_SPI_IO, // [0] INT of link v2 (firmware IO1, GPIO2), [1] unused (IO0, GPIO14)

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
assign FT_OE_N = 1'b1;     // the FT812 does not drive the video DAC
assign BEEPER = 1'b0;
assign DAC_MUTE = 1'b1;    // soft mute of the PCM5102: 1 = sound on

// the floppy connector is not used: its outputs 0 (the board inverts them: high = active)
assign FDC_DRIVE = 2'b00;
assign FDC_MOTOR = 1'b0;
assign FDC_DIR = 1'b0;
assign FDC_STEP = 1'b0;
assign FDC_WDATA = 1'b0;
assign FDC_WGATE = 1'b0;
assign FDC_SIDE_N = 1'b0;
assign MIDI_TX = 1'b1;
assign MIDI_CLK = 1'b0;      // the SAM2695 is not used: in reset
assign MIDI_RESET_N = 1'b0;
// the flash chip (the SDRAM /CS is hard wired on Go): not selected
assign FLASH_CS_N = 1'b1;
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

// the pixel clock of the VGA DAC (no FT_CLK_OUT on Go: the FT812 has its crystal)
ODDR2 oddr_v_clk (.Q(V_CLK), .C0(clk_sys), .C1(~clk_sys), .CE(1'b1), .D0(1'b1), .D1(1'b0), .R(1'b0), .S(1'b0));

// ------------------------------------------------------------------ MCU (link v2)

// MCU_SCK is not on a clock pin before rev.G (CLOCK_DEDICATED_ROUTE = FALSE in the ucf)
wire mcu_sck = MCU_SCK, mcu_sck_ibufg = MCU_SCK;
wire mcu_int;
assign MCU_SPI_IO[0] = mcu_int;   // INT of link v2 on N9 (IO1 of the firmware)
assign MCU_SPI_IO[1] = 1'bz;

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
	.MCU_SPI_FT_SS(MCU_SPI_FT_CS_N),
	.MCU_SPI_SD2_SS(MCU_SPI_SD2_CS_N),
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

// ------------------------------------------------------------------ VGA

// 1024 x 768 at 60 Hz: the syncs of term_video (negative, as on HDMI), black outside de (the DAC has no
// blank input); the OSD output is registered with them
reg vga_hs = 1'b1, vga_vs = 1'b1;
reg [23:0] vga_rgb = 24'd0;
always @(posedge clk_sys) begin
	vga_hs <= ~osd_hs;
	vga_vs <= ~osd_vs;
	vga_rgb <= osd_de ? osd_rgb : 24'd0;
end
assign VGA_R = vga_rgb[23:16];
assign VGA_G = vga_rgb[15:8];
assign VGA_B = vga_rgb[7:0];
assign VGA_HS = vga_hs;
assign VGA_VS = vga_vs;

// the PCM5102: the bell
kg_pcm5102 #(.DIV_BITS(4)) dac (
	.clk(clk_sys),
	.reset(areset),
	.left(snd),
	.right(snd),
	.din(DAC_DAT),
	.bck(DAC_BCK),
	.lrck(DAC_LRCK)
);

// ------------------------------------------------------------------ debug
// DEBUG_ADDR { cpu_run, exit seen, 6'd0, the last debug console byte }, DEBUG_DATA { debug bytes count }
reg [15:0] dbg_n = 16'd0;
always @(posedge clk_sys) begin
	if (dbg_wr) dbg_n <= dbg_n + 16'd1;
	dbg_data <= dbg_n;
end
assign dbg_addr = { cpu_run, dbg_exit[8], 6'd0, dbg_byte };

endmodule
