# sims of the term core: make -> iss (the Ф0 ISS), obj_soc/tb_soc (Ф1: term_soc RTL on an SRAM pin model)
all: iss obj_soc/tb_soc obj_video/tb_video

iss: iss.cpp
	g++ -O2 -march=native -o $@ $<

RTL := ../rtl/term_soc.v ../rtl/term_sram.v ../rtl/term_video.v ../rtl/vexriscv/VexRiscv_16k.v
obj_soc/tb_soc: tb_soc.cpp $(RTL)
	verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style \
	  --top-module term_soc -Mdir obj_soc -o tb_soc \
	  -CFLAGS "-O2" -MAKEFLAGS "OPT_FAST=-O3 OPT_SLOW=-O1" $(RTL) tb_soc.cpp >obj_soc.log 2>&1 || { tail -20 obj_soc.log; exit 1; }

obj_video/tb_video: tb_video.cpp ../rtl/term_video.v
	verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style \
	  --top-module term_video -Mdir obj_video -o tb_video \
	  -CFLAGS "-O2" -MAKEFLAGS "OPT_FAST=-O3 OPT_SLOW=-O1" ../rtl/term_video.v tb_video.cpp >obj_video.log 2>&1 || { tail -20 obj_video.log; exit 1; }

clean:
	rm -rf iss obj_* obj_*.log
.PHONY: all clean
