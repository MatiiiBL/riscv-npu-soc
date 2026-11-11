// UART transmitter, 8N1, LSB first.
//
// A byte is accepted when `valid` and `ready` are both high; `ready` stays low
// for the 10 bit times of the frame. CLKS_PER_BIT is the baud divider
// (clock frequency / baud rate).
module uart_tx #(
  parameter int CLKS_PER_BIT = 217
) (
  input  logic       clk,
  input  logic       rst_n,
  input  logic       valid,
  input  logic [7:0] data,
  output logic       ready,
  output logic       txd
);

  localparam int CW = (CLKS_PER_BIT > 1) ? $clog2(CLKS_PER_BIT) : 1;
  localparam logic [CW-1:0] BIT_LAST = CW'(CLKS_PER_BIT - 1);

  logic [9:0]    frame;      // {stop, data[7:0], start}, shifted out from bit 0
  logic [3:0]    bits_left;
  logic [CW-1:0] baud_cnt;

  assign ready = (bits_left == 4'd0);
  assign txd   = frame[0];

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      frame     <= '1;          // line idles high
      bits_left <= '0;
      baud_cnt  <= '0;
    end else if (ready) begin
      if (valid) begin
        frame     <= {1'b1, data, 1'b0};
        bits_left <= 4'd10;
        baud_cnt  <= BIT_LAST;
      end
    end else if (baud_cnt != '0) begin
      baud_cnt <= baud_cnt - 1'b1;
    end else begin
      frame     <= {1'b1, frame[9:1]};
      bits_left <= bits_left - 4'd1;
      baud_cnt  <= BIT_LAST;
    end
  end

endmodule
