// Peripheral registers in the MMIO window (docs/spec.md 3):
//
//   0x000 UART_TX      W   transmit the low byte
//   0x004 UART_STATUS  R   bit 0: transmitter ready
//   0x010 SIM_EXIT     W   end the simulation with this value
//   0x020 LEDS         RW  low 8 bits drive the board LEDs
//
// Undefined offsets and the write-only registers read 0. A write must
// include byte lane 0, so `sw` (or a narrower store at the register's own
// address) is what reaches a register.
//
// The simulation harness takes UART bytes and the exit value straight from
// the write strobes (tx_valid, exit_valid) instead of decoding the serial
// line; with SIM set, UART_STATUS reports ready unconditionally so firmware
// never waits on the baud rate.
module soc_mmio #(
  parameter int UART_DIV = 217,
  parameter bit SIM      = 1'b0
) (
  input  logic        clk,
  input  logic        rst_n,

  input  logic        req,
  input  logic        we,
  input  logic [11:0] addr,
  input  logic [3:0]  be,
  input  logic [31:0] wdata,
  output logic [31:0] rdata,

  output logic        uart_txd,
  output logic [7:0]  leds,

  output logic        tx_valid,
  output logic [7:0]  tx_data,
  output logic        exit_valid,
  output logic [31:0] exit_code
);

  localparam logic [9:0] REG_UART_TX     = 10'h000;  // word index, offset >> 2
  localparam logic [9:0] REG_UART_STATUS = 10'h001;
  localparam logic [9:0] REG_SIM_EXIT    = 10'h004;
  localparam logic [9:0] REG_LEDS        = 10'h008;

  logic [9:0] word;
  logic       wr, tx_ready;

  assign word = addr[11:2];
  assign wr   = req && we && be[0];

  assign tx_valid   = wr && (word == REG_UART_TX);
  assign tx_data    = wdata[7:0];
  assign exit_valid = wr && (word == REG_SIM_EXIT);
  assign exit_code  = wdata;

  uart_tx #(
    .CLKS_PER_BIT (UART_DIV)
  ) u_uart_tx (
    .clk   (clk),
    .rst_n (rst_n),
    .valid (tx_valid),
    .data  (tx_data),
    .ready (tx_ready),
    .txd   (uart_txd)
  );

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      leds <= '0;
    end else if (wr && word == REG_LEDS) begin
      leds <= wdata[7:0];
    end
  end

  always_ff @(posedge clk) begin
    if (req && !we) begin
      case (word)
        REG_UART_STATUS: rdata <= {31'b0, SIM || tx_ready};
        REG_LEDS:        rdata <= {24'b0, leds};
        default:         rdata <= '0;
      endcase
    end
  end

  logic unused;
  assign unused = ^{addr[1:0], be[3:1]};

endmodule
