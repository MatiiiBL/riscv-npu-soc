// Board top for the Radiona ULX3S (LFE5U-85F, 25 MHz oscillator).
//
//   clk_25mhz --> [EHXPLLL, unless CLK_MHZ = 25] --> clk --> soc_top
//   btn_fire1 --+
//   PLL LOCK ---+--> synchronizers --> reset stretcher --> rst_n
//   soc_top.uart_txd --> ftdi_rxd (the FTDI chip's RX: USB serial, 115200 8N1)
//   soc_top.leds     --> led[7:0]
//
// The firmware is in the bitstream: IMEM_INIT and DMEM_INIT are the
// $readmemh images that tools/elf2hex.py makes from an ELF (IMEM_INIT a
// file, DMEM_INIT the path prefix of the NPU_DMA_BYTES / 4 DMEM bank
// images), so the block RAMs hold the program when configuration ends and
// the core starts from reset at PC 0. Port names are those of the board's official constraint file
// (ulx3s_v20.lpf), except btn_fire1, which is btn[1] there.
module ulx3s_top #(
  parameter int CLK_MHZ       = 25,   // core clock: 25, or a PLL setting below
  parameter int NPU_DMA_BYTES = 16,   // NPU DMA port width: 4, 8 or 16 bytes
  parameter     IMEM_INIT     = "",
  parameter     DMEM_INIT     = ""
) (
  input  logic       clk_25mhz,
  input  logic       btn_fire1,    // F1 button, high while pressed
  output logic       ftdi_rxd,
  output logic [7:0] led,
  output logic       wifi_gpio0
);

  // ------------------------------------------------------------------ clock
  // At 25 MHz the oscillator clocks the SoC directly. Any other CLK_MHZ needs
  // an entry below: the EHXPLLL settings `ecppll -i 25 -o <CLK_MHZ>` prints,
  // packed as {CLKI_DIV, CLKFB_DIV, CLKOP_DIV, CLKOP_CPHASE}. Each gives
  // exactly 25 MHz * CLKFB_DIV / CLKI_DIV, with feedback from CLKOP and the
  // VCO (output * CLKOP_DIV) inside its 400-800 MHz range.
  function automatic logic [31:0] pll_settings(int mhz);
    case (mhz)
      30:      pll_settings = {8'd5, 8'd6, 8'd20, 8'd9};
      35:      pll_settings = {8'd5, 8'd7, 8'd17, 8'd8};
      40:      pll_settings = {8'd5, 8'd8, 8'd15, 8'd7};
      45:      pll_settings = {8'd5, 8'd9, 8'd13, 8'd6};
      50:      pll_settings = {8'd1, 8'd2, 8'd12, 8'd5};
      default: pll_settings = '0;
    endcase
  endfunction

  logic clk, clk_locked;

  if (CLK_MHZ == 25) begin : g_osc
    assign clk        = clk_25mhz;
    assign clk_locked = 1'b1;
  end else begin : g_pll
    localparam logic [31:0] PLL = pll_settings(CLK_MHZ);

    if (PLL == '0) begin : g_no_setting
      $error("ulx3s_top: CLK_MHZ has no entry in pll_settings");
    end

    (* ICP_CURRENT = "12" *) (* LPF_RESISTOR = "8" *)
    (* MFG_ENABLE_FILTEROPAMP = "1" *) (* MFG_GMCREF_SEL = "2" *)
    EHXPLLL #(
      .CLKI_DIV      (int'(PLL[31:24])),
      .CLKFB_DIV     (int'(PLL[23:16])),
      .CLKOP_DIV     (int'(PLL[15:8])),
      .CLKOP_CPHASE  (int'(PLL[7:0])),
      .CLKOP_FPHASE  (0),
      .CLKOP_ENABLE  ("ENABLED"),
      .FEEDBK_PATH   ("CLKOP"),
      .PLLRST_ENA    ("DISABLED"),
      .INTFB_WAKE    ("DISABLED"),
      .STDBY_ENABLE  ("DISABLED"),
      .DPHASE_SOURCE ("DISABLED")
    ) u_pll (
      .CLKI         (clk_25mhz),
      .CLKFB        (clk),
      .PHASESEL1    (1'b0),
      .PHASESEL0    (1'b0),
      .PHASEDIR     (1'b1),
      .PHASESTEP    (1'b1),
      .PHASELOADREG (1'b1),
      .STDBY        (1'b0),
      .PLLWAKESYNC  (1'b0),
      .RST          (1'b0),
      .ENCLKOP      (1'b0),
      .ENCLKOS      (1'b0),
      .ENCLKOS2     (1'b0),
      .ENCLKOS3     (1'b0),
      .CLKOP        (clk),
      .CLKOS        (),
      .CLKOS2       (),
      .CLKOS3       (),
      .LOCK         (clk_locked),
      .INTLOCK      (),
      .REFCLK       (),
      .CLKINTFB     ()
    );
  end

  // ------------------------------------------------------------------ reset
  // The button and LOCK are asynchronous to clk, so each passes through two
  // flip-flops first. While either asks for reset the counter is cleared,
  // and rst_n rises 2^(RST_BITS-1) cycles after the last request: that holds
  // the SoC in reset until the PLL is stable and swallows button bounce. The
  // initial values are the flip-flops' state when configuration ends, which
  // makes the same counter the power-on reset.
  localparam int RST_BITS = 17;   // 65536 cycles: 2.6 ms at 25 MHz

  logic [1:0]          btn_sync  = '0;
  logic [1:0]          lock_sync = '0;
  logic [RST_BITS-1:0] rst_cnt   = '0;
  logic                rst_n;

  always_ff @(posedge clk) begin
    btn_sync  <= {btn_sync[0], btn_fire1};
    lock_sync <= {lock_sync[0], clk_locked};
    if (btn_sync[1] || !lock_sync[1]) begin
      rst_cnt <= '0;
    end else if (!rst_cnt[RST_BITS-1]) begin
      rst_cnt <= rst_cnt + 1'b1;
    end
  end

  assign rst_n = rst_cnt[RST_BITS-1];

  // -------------------------------------------------------------------- SoC
  localparam int CLK_HZ   = CLK_MHZ * 1_000_000;
  localparam int BAUD     = 115_200;
  localparam int UART_DIV = (CLK_HZ + BAUD / 2) / BAUD;   // rounded

  soc_top #(
    .UART_DIV      (UART_DIV),
    .SIM           (1'b0),
    .NPU_DMA_BYTES (NPU_DMA_BYTES),
    .IMEM_INIT     (IMEM_INIT),
    .DMEM_INIT     (DMEM_INIT)
  ) u_soc (
    .clk             (clk),
    .rst_n           (rst_n),
    .uart_txd        (ftdi_rxd),
    .leds            (led),
    .sim_uart_valid  (),
    .sim_uart_data   (),
    .sim_exit_valid  (),
    .sim_exit_code   (),
    .rvfi_valid      (),
    .rvfi_pc         (),
    .rvfi_insn       (),
    .rvfi_rd         (),
    .rvfi_rd_wdata   (),
    .rvfi_mem_addr   (),
    .rvfi_mem_wmask  (),
    .rvfi_mem_wdata  (),
    .halted          (),
    .perf_cycles     (),
    .perf_instret    (),
    .perf_load_use   (),
    .perf_mispredict (),
    .perf_div_stall  (),
    .perf_ctrl       ()
  );

  // The ESP32's GPIO0 is wired to the FPGA. Like the board's example
  // designs, hold it high: low while the ESP32 resets, it selects the ESP32's
  // serial bootloader instead of its firmware.
  assign wifi_gpio0 = 1'b1;

endmodule
