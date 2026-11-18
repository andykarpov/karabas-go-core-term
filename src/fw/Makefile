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
$(B)/%.o: %.c mmio.h | $(B)
	$(CROSS)gcc $(CFLAGS) -c $< -o $@
$(B)/crt0.o: crt0.S | $(B)
	$(CROSS)gcc $(ABI) -c $< -o $@
$(B)/bench_vterm.elf: $(B)/crt0.o $(B)/bench_vterm.o $(B)/syscalls.o $(VTOBJ) link.ld
	$(CROSS)gcc $(LDFLAGS) -T link.ld -Wl,-Map,$(B)/bench_vterm.map -o $@ $(B)/crt0.o $(B)/bench_vterm.o $(B)/syscalls.o $(VTOBJ)
	$(CROSS)size $@
$(B)/bench_state.elf: $(B)/crt0.o $(B)/bench_state.o $(B)/tscreen.o $(B)/syscalls.o $(VTOBJ) link.ld
	$(CROSS)gcc $(LDFLAGS) -T link.ld -Wl,-Map,$(B)/bench_state.map -o $@ $(B)/crt0.o $(B)/bench_state.o $(B)/tscreen.o $(B)/syscalls.o $(VTOBJ)
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

$(BH)/bench_vterm: bench_vterm.c $(B)/vtsrc/.stamp | $(BH)
	cc -O2 -DHOST -std=c99 -w -I$(VT)/include -o $@ bench_vterm.c $(addprefix $(B)/vtsrc/,$(addsuffix .c,$(VTSRC)))

$(BH)/bench_state: bench_state.c tscreen.c tscreen.h $(B)/vtsrc/.stamp | $(BH)
	cc -O2 -DHOST -std=c99 -w -I$(VT)/include -I. -o $@ bench_state.c tscreen.c $(addprefix $(B)/vtsrc/,$(addsuffix .c,$(VTSRC)))

$(BH)/bench_crypto: bench_crypto.c | $(BH)
	cc -O2 -DHOST -w -I$(MC) -I$(MC)/optional -o $@ bench_crypto.c $(MC)/monocypher.c $(MC)/optional/monocypher-ed25519.c

clean:
	rm -rf build-*

.PHONY: all clean
