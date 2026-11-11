// ARR x ARR output-stationary systolic array.
//
// Row i of A enters on the west edge and moves east one PE per cycle; column
// j of B (row j of the N x K matrix) enters on the north edge and moves south.
// The feeders (npu_opbuf) skew the edges so that A[i][k] enters row i at
// cycle k + i and B[j][k] enters column j at cycle k + j: both then reach
// PE (i, j) at cycle k + i + j, which accumulates C[i][j] += A[i][k] * B[j][k].
//
//              b_feed[0]  b_feed[1]       b_feed[ARR-1]
//                  |          |                |
//   a_feed[0] -> PE(0,0) -> PE(0,1) -> ... -> PE(0,ARR-1) ->
//                  |          |                |
//   a_feed[1] -> PE(1,0) -> PE(1,1) -> ... -> PE(1,ARR-1) ->
//                  :          :                :
module npu_array #(
  parameter int ARR = 8
) (
  input  logic                  clk,
  input  logic                  rst_n,
  input  logic                  clr,     // start new sums in every PE
  input  logic [ARR*8-1:0]      a_feed,  // row i operand at [8*i +: 8]
  input  logic [ARR*8-1:0]      b_feed,  // column j operand at [8*j +: 8]
  output logic [ARR*ARR*32-1:0] acc      // PE (i, j) at [32*(i*ARR + j) +: 32]
);

  // a_h[i][j]: A operand entering PE (i, j) from the west; column ARR is the
  // east edge. b_v[i][j]: B operand entering PE (i, j) from the north; row ARR
  // is the south edge.
  logic [ARR-1:0][ARR:0][7:0] a_h;
  logic [ARR:0][ARR-1:0][7:0] b_v;

  // Operands leaving the east and south edges are not needed. The *_unused
  // names are exempt from the lint unused-signal check, and synthesis removes
  // the dead forwarding registers.
  logic [ARR-1:0][7:0] a_east_unused;
  logic [ARR-1:0][7:0] b_south_unused;

  for (genvar e = 0; e < ARR; e++) begin : g_edge
    assign a_h[e][0]          = a_feed[8*e +: 8];
    assign b_v[0][e]          = b_feed[8*e +: 8];
    assign a_east_unused[e]   = a_h[e][ARR];
    assign b_south_unused[e]  = b_v[ARR][e];
  end

  for (genvar i = 0; i < ARR; i++) begin : g_row
    for (genvar j = 0; j < ARR; j++) begin : g_col
      npu_pe u_pe (
        .clk,
        .rst_n,
        .clr,
        .a_in  (a_h[i][j]),
        .b_in  (b_v[i][j]),
        .a_out (a_h[i][j+1]),
        .b_out (b_v[i+1][j]),
        .acc   (acc[32*(i*ARR+j) +: 32])
      );
    end
  end

endmodule
