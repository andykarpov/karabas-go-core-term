#!/bin/bash
# Ф1: the RTL SoC (obj_soc/tb_soc) over the recorded streams (rec.py) through the USB serial port: the
# screen hash printed by the firmware == the libvterm oracle (fw/build-host/bench_vterm) of the same
# bytes after the firmware's hello line. Streams cut to 8 KB (a run of seconds).
# usage: run_f1.sh [streams dir (build/streams)] [font_full.bin (build/font/font_full.bin)]
set -e -o pipefail
cd "$(dirname "$0")"
SD=${1:-build/streams}
FONT=${2:-build/font/font_full.bin}
FW=../fw/build-hw-rv32imO2/term.bin
W=build/f1
mkdir -p $W
printf '\033[1mterm\033[0m: xterm-256color, 128 x 48, USB serial\r\n' > $W/hello.bin
ok=1
for s in "$SD"/*.bin; do
	n=$(basename "$s" .bin)
	head -c 8000 "$s" > $W/$n.bin
	cat $W/hello.bin $W/$n.bin > $W/h_$n.bin
	want=$(../fw/build-host/bench_vterm $W/h_$n.bin | awk '{print $NF}')
	got=$(./obj_soc/tb_soc $FW $FONT --in $W/$n.bin --rate 300 --hash-frames 2 --shot 0 /dev/null 2>/dev/null | sed -n 's/^DBG HASH //p')
	if [ "$got" = "$want" ]; then echo "$n: $got = oracle"; else echo "$n: $got != oracle $want"; ok=0; fi
done
[ $ok = 1 ]
