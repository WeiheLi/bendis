# FPGA flow for rtl/bendis.v
#   make sim BITS=11          Verilator testbench (bit-exact against the P4 golden model)
#   make ecp5 BITS=11         Yosys + nextpnr place and route on a Lattice ECP5-85F
#   make xcup BITS=11         Yosys synthesis for Xilinx UltraScale+ (resource estimate only)
BITS ?= 11
WIN_BITS ?= 15
FREQ ?= 90
OUT ?= build

sim: $(OUT)/tb_b$(BITS)/Vbendis

$(OUT)/tb_b$(BITS)/Vbendis: rtl/bendis.v rtl/bendis_hash.v tb/tb_bendis.cpp ../src/*.h
	@mkdir -p $(OUT)/tb_b$(BITS)
	verilator --cc --exe --build -O3 -j 4 -Wno-fatal -Wno-WIDTH --top-module bendis \
		-GCM_BITS=$(BITS) -GTAB_BITS=$(BITS) -GWIN_BITS=$(WIN_BITS) \
		-CFLAGS "-O2 -std=c++17 -I$(CURDIR)/../src -DCM_BITS=$(BITS) -DWIN_BITS=$(WIN_BITS)" \
		-Mdir $(OUT)/tb_b$(BITS) $(CURDIR)/rtl/bendis.v $(CURDIR)/rtl/bendis_hash.v $(CURDIR)/tb/tb_bendis.cpp

ecp5: rtl/bendis.v rtl/bendis_hash.v
	@mkdir -p $(OUT)
	yosys -q -l $(OUT)/ecp5_b$(BITS).yosys.log -p "read_verilog rtl/bendis.v rtl/bendis_hash.v; \
		chparam -set CM_BITS $(BITS) -set TAB_BITS $(BITS) -set WIN_BITS $(WIN_BITS) bendis; \
		synth_ecp5 -top bendis -json $(OUT)/ecp5_b$(BITS).json; stat"
	nextpnr-ecp5 --85k --package CABGA756 --json $(OUT)/ecp5_b$(BITS).json --freq $(FREQ) \
		--lpf-allow-unconstrained --timing-allow-fail --seed 1 --report $(OUT)/ecp5_b$(BITS).report.json \
		--log $(OUT)/ecp5_b$(BITS).nextpnr.log

xcup: rtl/bendis.v rtl/bendis_hash.v
	@mkdir -p $(OUT)
	yosys -q -l $(OUT)/xcup_b$(BITS).yosys.log -p "read_verilog rtl/bendis.v rtl/bendis_hash.v; \
		chparam -set CM_BITS $(BITS) -set TAB_BITS $(BITS) -set WIN_BITS $(WIN_BITS) bendis; \
		synth_xilinx -family xcup -top bendis; stat"

.PHONY: sim ecp5 xcup
