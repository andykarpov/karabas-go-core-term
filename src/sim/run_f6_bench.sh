#!/bin/bash
# Ф6: the time of the host-key signature checks (fw/pk.c) at 65 MHz on the ISS (16K / 16K caches; the cost model of
# run_f0.py: CPI 1.7, 33 clocks a line fill). usage: run_f6_bench.sh
set -e -o pipefail
cd "$(dirname "$0")/.."
B=fw/build-rv32imO2; X=$HOME/opt/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf-; MC=../upstream/monocypher/src
make -C fw build-rv32imO2/crt0.o build-rv32imO2/syscalls.o > /dev/null
python3 -W ignore tools/pk_bench_vec.py > $B/bench_pk_vec.h
F="-march=rv32im -mabi=ilp32 -O2 -std=c99 -ffunction-sections -fdata-sections -w -Ifw -I$B -I$MC -I$MC/optional"
${X}gcc $F -nostartfiles --specs=nano.specs -Wl,--gc-sections -T fw/link.ld -o $B/bench_pk.elf $B/crt0.o fw/bench_pk.c \
   fw/pk.c fw/sha256.c $MC/monocypher.c $MC/optional/monocypher-ed25519.c $B/syscalls.o 2> /dev/null
${X}objcopy -O binary $B/bench_pk.elf $B/bench_pk.bin
sim/iss $B/bench_pk.bin --icache 16384 --dcache 16384 2>&1 | grep '^M\|^ok' | python3 -c "
import sys
names = ['', 'RSA 2048', 'RSA 3072', 'RSA 4096', 'P-256', 'P-384', 'P-521']
for l in sys.stdin:
    if l.startswith('ok'): print(l.strip()); continue
    m = l.strip().split(','); i = int(m[1])
    if i: ins = int(m[2]); miss = sum(int(v) for v in m[3:7]); print(f'{names[i]:9} {ins / 1e6:7.1f} M instr {(ins * 1.7 + miss * 33) / 65e3:7.0f} ms @65')
"
