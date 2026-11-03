CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable

BINS = bin/example bin/gen_synth bin/pcap2tuple bin/p4_equiv bin/p4_expect
# the AVX-512 version (src/bendis_simd.h) builds only on x86-64
ifeq ($(shell uname -m),x86_64)
SIMD = bin/simd_check
endif

all: $(BINS) $(SIMD)

bin/example: src/example.cpp src/bendis.h src/pbo_common.h
bin/gen_synth: src/gen_synth.cpp src/pbo_common.h
bin/pcap2tuple: src/pcap2tuple.cpp
bin/example bin/gen_synth bin/pcap2tuple:
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<

bin/simd_check bin/p4_equiv bin/p4_expect: bin/%: tests/%.cpp src/*.h
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $<

# A synthetic trace of 3M packets (100 windows) with planted periodic bursts, then the checks:
# the P4 behavioral model reports exactly what src/bendis.h reports, and the AVX-512 version
# reports exactly what the scalar version reports (skipped without AVX-512).
data/synth.dat: bin/gen_synth
	@mkdir -p data
	./bin/gen_synth --packets 3000000 --alpha 1.0 --out $@

test: bin/example bin/p4_equiv $(SIMD) data/synth.dat
	./bin/example data/synth.dat 32 10 pbs16
	./bin/p4_equiv data/synth.dat --fmt pbs16 --bits 10,11
	$(if $(SIMD),./bin/simd_check --data data/synth.dat --fmt pbs16 --mem 32 --seeds 1 --reps 1)

clean:
	rm -rf bin data

.PHONY: all test clean
