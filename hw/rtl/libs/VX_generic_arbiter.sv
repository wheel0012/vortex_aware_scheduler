// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

`include "VX_platform.vh"

`TRACING_OFF
module VX_generic_arbiter #(
    parameter NUM_REQS     = 1,
    parameter `STRING TYPE = "P", // P: priority, R: round-robin, M: matrix, C: cyclic, G: GTO, A: gCAWS
    parameter STICKY       = 0,   // hold the grant until its request is deasserted
    parameter ORDERW       = 1,
    parameter PRIORITYW    = 1,
    parameter LOG_NUM_REQS = `LOG2UP(NUM_REQS)
) (
    input  wire                     clk,
    input  wire                     reset,
    input  wire [NUM_REQS-1:0]      requests,
    input  wire [NUM_REQS-1:0][ORDERW-1:0] request_order,
    input  wire [NUM_REQS-1:0][PRIORITYW-1:0] request_priority,
    output wire [LOG_NUM_REQS-1:0]  grant_index,
    output wire [NUM_REQS-1:0]      grant_onehot,
    output wire                     grant_valid,
    input  wire                     grant_ready
);
    `STATIC_ASSERT((TYPE == "P" || TYPE == "R" || TYPE == "M" || TYPE == "C" || TYPE == "G" || TYPE == "A"), ("invalid parameter"))

    if (TYPE == "P") begin : g_priority

        `UNUSED_VAR (clk)
        `UNUSED_VAR (reset)
        `UNUSED_VAR (grant_ready)
        `UNUSED_VAR (request_order)
        `UNUSED_VAR (request_priority)

        VX_priority_arbiter #(
            .NUM_REQS (NUM_REQS),
            .STICKY   (STICKY)
        ) priority_arbiter (
            .clk          (clk),
            .reset        (reset),
            .requests     (requests),
            .grant_valid  (grant_valid),
            .grant_index  (grant_index),
            .grant_onehot (grant_onehot),
            .grant_ready  (grant_ready)
        );

    end else if (TYPE == "R") begin : g_round_robin

        `UNUSED_VAR (request_order)
        `UNUSED_VAR (request_priority)

        VX_rr_arbiter #(
            .NUM_REQS (NUM_REQS),
            .STICKY   (STICKY)
        ) rr_arbiter (
            .clk          (clk),
            .reset        (reset),
            .requests     (requests),
            .grant_valid  (grant_valid),
            .grant_index  (grant_index),
            .grant_onehot (grant_onehot),
            .grant_ready  (grant_ready)
        );

    end else if (TYPE == "M") begin : g_matrix

        `UNUSED_VAR (request_order)
        `UNUSED_VAR (request_priority)

        VX_matrix_arbiter #(
            .NUM_REQS (NUM_REQS),
            .STICKY   (STICKY)
        ) matrix_arbiter (
            .clk          (clk),
            .reset        (reset),
            .requests     (requests),
            .grant_valid  (grant_valid),
            .grant_index  (grant_index),
            .grant_onehot (grant_onehot),
            .grant_ready  (grant_ready)
        );

    end else if (TYPE == "C") begin : g_cyclic

        `UNUSED_VAR (request_order)
        `UNUSED_VAR (request_priority)

        VX_cyclic_arbiter #(
            .NUM_REQS (NUM_REQS),
            .STICKY   (STICKY)
        ) cyclic_arbiter (
            .clk          (clk),
            .reset        (reset),
            .requests     (requests),
            .grant_valid  (grant_valid),
            .grant_index  (grant_index),
            .grant_onehot (grant_onehot),
            .grant_ready  (grant_ready)
        );

    end else if (TYPE == "G" || TYPE == "A") begin : g_ordered

        localparam USE_PRIORITY = (TYPE == "A");

        reg current_valid;
        reg [LOG_NUM_REQS-1:0] current_index;

        reg [LOG_NUM_REQS-1:0] grant_index_w;
        reg [NUM_REQS-1:0] grant_onehot_w;
        reg grant_valid_w;

        always @(*) begin
            grant_index_w  = '0;
            grant_onehot_w = '0;
            grant_valid_w  = 1'b0;

            if (current_valid && requests[current_index]) begin
                grant_index_w = current_index;
                grant_onehot_w[current_index] = 1'b1;
                grant_valid_w = 1'b1;
            end else begin
                for (integer i = 0; i < NUM_REQS; ++i) begin
                    if (requests[i]) begin
                        if (!grant_valid_w
                         || (USE_PRIORITY && (request_priority[i] > request_priority[grant_index_w]))
                         || ((!USE_PRIORITY || (request_priority[i] == request_priority[grant_index_w]))
                          && (request_order[i] < request_order[grant_index_w]))) begin
                            grant_index_w = LOG_NUM_REQS'(i);
                            grant_onehot_w = '0;
                            grant_onehot_w[i] = 1'b1;
                            grant_valid_w = 1'b1;
                        end
                    end
                end
            end
        end

        always @(posedge clk) begin
            if (reset) begin
                current_valid <= 1'b0;
                current_index <= '0;
            end else if (grant_valid && grant_ready) begin
                current_valid <= 1'b1;
                current_index <= grant_index;
            end else if (current_valid && !requests[current_index]) begin
                current_valid <= 1'b0;
            end
        end

        assign grant_index  = grant_index_w;
        assign grant_onehot = grant_onehot_w;
        assign grant_valid  = grant_valid_w;

    end

    `RUNTIME_ASSERT (((~(| requests) != 1) || (grant_valid && (requests[grant_index] != 0) && (grant_onehot == (NUM_REQS'(1) << grant_index)))), ("%t: invalid arbiter grant!", $time))

endmodule
`TRACING_ON
