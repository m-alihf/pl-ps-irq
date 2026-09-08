// pl_irq_gen.v
// Periodic interrupt source for the PS, plus a data word for the ISR to consume.
//
//   0x00 CTRL    [0] enable
//   0x04 PERIOD  interrupt period in aclk cycles, minus 1
//   0x08 SEQ     RO, incremented on every interrupt (lets the ISR detect misses)
//   0x0C DATA    RO, new sample value produced with every interrupt
//
// irq is a 4-cycle high pulse -> connect to IRQ_F2P and configure the GIC
// entry as rising-edge, so the ISR needs no acknowledge write.

module pl_irq_gen #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 5
)(
    input  wire                              s_axi_aclk,
    input  wire                              s_axi_aresetn,

    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_awaddr,
    input  wire [2:0]                        s_axi_awprot,
    input  wire                              s_axi_awvalid,
    output wire                              s_axi_awready,
    input  wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  wire                              s_axi_wvalid,
    output wire                              s_axi_wready,
    output wire [1:0]                        s_axi_bresp,
    output wire                              s_axi_bvalid,
    input  wire                              s_axi_bready,

    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
    input  wire [2:0]                        s_axi_arprot,
    input  wire                              s_axi_arvalid,
    output wire                              s_axi_arready,
    output wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
    output wire [1:0]                        s_axi_rresp,
    output wire                              s_axi_rvalid,
    input  wire                              s_axi_rready,

    output wire                              irq
);

    reg        awready_r, wready_r, bvalid_r;
    reg        arready_r, rvalid_r;
    reg [31:0] rdata_r;

    reg        ctrl_en;
    reg [31:0] period;
    reg [31:0] cnt;
    reg [31:0] seq;
    reg [31:0] sample;
    reg [2:0]  pulse;

    assign s_axi_awready = awready_r;
    assign s_axi_wready  = wready_r;
    assign s_axi_bvalid  = bvalid_r;
    assign s_axi_bresp   = 2'b00;
    assign s_axi_arready = arready_r;
    assign s_axi_rvalid  = rvalid_r;
    assign s_axi_rresp   = 2'b00;
    assign s_axi_rdata   = rdata_r;
    assign irq           = (pulse != 3'd0);

    // write channel
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            awready_r <= 1'b0;
            wready_r  <= 1'b0;
            bvalid_r  <= 1'b0;
            ctrl_en   <= 1'b0;
            period    <= 32'd1249;      // 12.5 us at 100 MHz
        end else begin
            awready_r <= 1'b0;
            wready_r  <= 1'b0;
            if (!awready_r && !wready_r && !bvalid_r &&
                 s_axi_awvalid && s_axi_wvalid) begin
                awready_r <= 1'b1;
                wready_r  <= 1'b1;
                bvalid_r  <= 1'b1;
                case (s_axi_awaddr[4:2])
                    3'd0: ctrl_en <= s_axi_wdata[0];
                    3'd1: period  <= s_axi_wdata;
                    default: ;
                endcase
            end else if (bvalid_r && s_axi_bready) begin
                bvalid_r <= 1'b0;
            end
        end
    end

    // read channel
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            arready_r <= 1'b0;
            rvalid_r  <= 1'b0;
            rdata_r   <= 32'd0;
        end else begin
            arready_r <= 1'b0;
            if (!arready_r && !rvalid_r && s_axi_arvalid) begin
                arready_r <= 1'b1;
                rvalid_r  <= 1'b1;
                case (s_axi_araddr[4:2])
                    3'd0: rdata_r <= {31'd0, ctrl_en};
                    3'd1: rdata_r <= period;
                    3'd2: rdata_r <= seq;
                    3'd3: rdata_r <= sample;
                    default: rdata_r <= 32'd0;
                endcase
            end else if (rvalid_r && s_axi_rready) begin
                rvalid_r <= 1'b0;
            end
        end
    end

    // free-running period counter
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            cnt    <= 32'd0;
            seq    <= 32'd0;
            sample <= 32'd0;
            pulse  <= 3'd0;
        end else if (!ctrl_en) begin
            cnt   <= 32'd0;
            pulse <= 3'd0;
        end else if (cnt >= period) begin
            cnt    <= 32'd0;
            seq    <= seq + 32'd1;
            sample <= sample + 32'h9E3779B9;
            pulse  <= 3'd4;
        end else begin
            cnt <= cnt + 32'd1;
            if (pulse != 3'd0) pulse <= pulse - 3'd1;
        end
    end

endmodule
