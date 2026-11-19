# Ф0 bench firmware for the ISS (src/sim/iss): make [ARCH=rv32im] [OPT=-O2]
#   build-$(ARCH)$(OPT)/bench_vterm.bin : libvterm 0.3.3 (upstream/libvterm) + bench_vterm.c, rv32 newlib-nano
#   build-$(ARCH)$(OPT)/bench_state.bin : libvterm state layer + our screen layer tscreen.c (same hash)
#   build-$(ARCH)$(OPT)/bench_crypto.bin: SSH crypto (Monocypher 4.0.3, upstream/monocypher)
#   build-host/bench_*                   : the same benches on the host (bench_vterm = the oracle of the hash)
XPACK   ?= $(HOME)/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin
CROSS   := $(XPACK)/riscv-none-elf-
ARCH    ?= rv32im
OPT     ?= -O2
VT      := ../../upstream/libvterm
B       := build-$(ARCH)$(subst -,,$(OPT))
BH      := build-host
VTSRC   := encoding keyboard mouse parser pen screen state unicode vterm
VTINC   := $(VT)/src/encoding/DECdrawing.inc $(VT)/src/encoding/uk.inc
ABI     := -march=$(ARCH) -mabi=ilp32
CFLAGS  := $(ABI) $(OPT) -g -std=c99 -ffunction-sections -fdata-sections -w -I$(VT)/include -I.
LDFLAGS := $(ABI) -nostartfiles --specs=nano.specs -Wl,--gc-sections

MC      := ../../upstream/monocypher/src

all: $(B)/bench_vterm.bin $(BH)/bench_vterm $(B)/bench_state.bin $(BH)/bench_state $(B)/bench_crypto.bin $(BH)/bench_crypto

# the encoding tables of libvterm are generated (perl), into the build dir, not the submodule
$(B) $(BH) $(B)/enc:
	mkdir -p $@
$(B)/enc/%.inc: $(VT)/src/encoding/%.tbl | $(B)/enc
	perl -CSD $(VT)/tbl2inc_c.pl $< > $@
ENCINC := $(B)/enc/DECdrawing.inc $(B)/enc/uk.inc
# encoding.c includes "encoding/DECdrawing.inc": give it a copy of src with the generated tables
$(B)/vtsrc/.stamp: $(ENCINC) | $(B)
	mkdir -p $(B)/vtsrc/encoding
	cp $(VT)/src/*.c $(VT)/src/*.h $(VT)/src/*.inc $(B)/vtsrc/
	cp $(ENCINC) $(B)/vtsrc/encoding/
	touch $@

VTOBJ := $(addprefix $(B)/vt_,$(addsuffix .o,$(VTSRC)))
$(B)/vt_%.o: $(B)/vtsrc/.stamp
	$(CROSS)gcc $(CFLAGS) -c $(B)/vtsrc/$*.c -o $@
$(B)/%.o: %.c mmio.h compose_tab.h | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/crt0.o: crt0.S | $(B)
	$(CROSS)gcc $(ABI) -c $< -o $@
$(B)/bench_vterm.elf: $(B)/crt0.o $(B)/bench_vterm.o $(B)/vtfeed.o $(B)/compose.o $(B)/syscalls.o $(VTOBJ) link.ld
	$(CROSS)gcc $(LDFLAGS) -T link.ld -Wl,-Map,$(B)/bench_vterm.map -o $@ $(B)/crt0.o $(B)/bench_vterm.o $(B)/vtfeed.o $(B)/compose.o $(B)/syscalls.o $(VTOBJ)
	$(CROSS)size $@
$(B)/bench_state.elf: $(B)/crt0.o $(B)/bench_state.o $(B)/tscreen.o $(B)/vtfeed.o $(B)/compose.o $(B)/syscalls.o $(VTOBJ) link.ld
	$(CROSS)gcc $(LDFLAGS) -T link.ld -Wl,-Map,$(B)/bench_state.map -o $@ $(B)/crt0.o $(B)/bench_state.o $(B)/tscreen.o $(B)/vtfeed.o $(B)/compose.o $(B)/syscalls.o $(VTOBJ)
	$(CROSS)size $@
$(B)/bench_state.o $(B)/tscreen.o: tscreen.h
$(B)/mc_%.o: $(MC)/%.c | $(B)
	$(CROSS)gcc $(CFLAGS) -I$(MC) -c $< -o $@
$(B)/mc_monocypher-ed25519.o: $(MC)/optional/monocypher-ed25519.c | $(B)
	$(CROSS)gcc $(CFLAGS) -I$(MC) -c $< -o $@
$(B)/bench_crypto.o: bench_crypto.c mmio.h | $(B)
	$(CROSS)gcc $(CFLAGS) -I$(MC) -I$(MC)/optional -c $< -o $@
$(B)/bench_crypto.elf: $(B)/crt0.o $(B)/bench_crypto.o $(B)/mc_monocypher.o $(B)/mc_monocypher-ed25519.o $(B)/syscalls.o link.ld
	$(CROSS)gcc $(LDFLAGS) -T link.ld -Wl,-Map,$(B)/bench_crypto.map -o $@ $(B)/crt0.o $(B)/bench_crypto.o $(B)/mc_monocypher.o $(B)/mc_monocypher-ed25519.o $(B)/syscalls.o
	$(CROSS)size $@
$(B)/%.bin: $(B)/%.elf
	$(CROSS)objcopy -O binary $< $@

$(BH)/bench_vterm: bench_vterm.c vtfeed.c vtfeed.h compose.c compose_tab.h $(B)/vtsrc/.stamp | $(BH)
	cc -O2 -DHOST -DROWS=47 -std=c99 -w -I$(VT)/include -I. -o $@ bench_vterm.c vtfeed.c compose.c $(addprefix $(B)/vtsrc/,$(addsuffix .c,$(VTSRC)))

$(BH)/bench_state: bench_state.c tscreen.c tscreen.h vtfeed.c vtfeed.h compose.c compose_tab.h $(B)/vtsrc/.stamp | $(BH)
	cc -O2 -DHOST -std=c99 -w -I$(VT)/include -I. -o $@ bench_state.c tscreen.c vtfeed.c compose.c $(addprefix $(B)/vtsrc/,$(addsuffix .c,$(VTSRC)))

$(BH)/bench_crypto: bench_crypto.c | $(BH)
	cc -O2 -DHOST -w -I$(MC) -I$(MC)/optional -o $@ bench_crypto.c $(MC)/monocypher.c $(MC)/optional/monocypher-ed25519.c

# ------------------------------------------------------------------ the board firmware (roms/term.bin)
# make hw: build-hw-$(ARCH)$(OPT)/term.bin, program at SRAM 0 (term_soc.v); 4 tabs of 128 x 47 under the tab
# bar, a ring of 256 rows each (docs/PLAN.md Ф5)
BW      := build-hw-$(ARCH)$(subst -,,$(OPT))
HWDEFS  := -DTS_RING=256 -DTS_ROWS=47
HWCFLAGS := -march=$(ARCH)_zicsr -mabi=ilp32 $(OPT) -g -std=c99 -ffunction-sections -fdata-sections -w $(HWDEFS) -I$(VT)/include -I. -I$(MC) -I$(MC)/optional
HWOBJ   := $(addprefix $(BW)/,crt0.o term_main.o kbd.o tscreen.o vtfeed.o compose.o esp.o telnet.o charset.o ui.o book.o store.o rng.o sha256.o ssh.o sshkeys.o mc_monocypher.o mc_monocypher-ed25519.o syscalls_hw.o) $(addprefix $(BW)/vt_,$(addsuffix .o,$(VTSRC)))
hw: $(BW)/term.bin
# the board test firmware (hwtest.c): make hwtest -> build-hw-test/term.bin
hwtest:
	$(MAKE) BW=build-hw-test HWTEST=1 build-hw-test/term.bin
ifeq ($(HWTEST),1)
HWOBJ   := $(addprefix $(BW)/,crt0.o hwtest.o syscalls_hw.o)
endif
$(BW):
	mkdir -p $@
$(BW)/vt_%.o: $(B)/vtsrc/.stamp | $(BW)
	$(CROSS)gcc $(HWCFLAGS) -c $(B)/vtsrc/$*.c -o $@
$(BW)/mc_%.o: $(MC)/%.c | $(BW)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BW)/mc_monocypher-ed25519.o: $(MC)/optional/monocypher-ed25519.c | $(BW)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BW)/%.o: %.c mmio_hw.h tscreen.h kbd.h vtfeed.h esp.h telnet.h charset.h charset_tab.h ui.h book.h compose_tab.h store.h rng.h sha256.h ssh.h sshkeys.h | $(BW)
	$(CROSS)gcc $(HWCFLAGS) -c $< -o $@
$(BW)/crt0.o: crt0.S | $(BW)
	$(CROSS)gcc $(ABI) -c $< -o $@
$(BW)/term.elf: $(HWOBJ) link_hw.ld
	$(CROSS)gcc $(LDFLAGS) -T link_hw.ld -Wl,-Map,$(BW)/term.map -o $@ $(HWOBJ)
	$(CROSS)size $@
$(BW)/term.bin: $(BW)/term.elf
	$(CROSS)objcopy -O binary $< $@

clean:
	rm -rf build-*

.PHONY: all clean hw hwtest
