#!/usr/bin/env python3
"""Ф0.1: bench_vterm on the ISS over the recorded streams (rec.py), hash checked against the host build.
usage: run_f0.py [streams dir = build/streams] [--bench vterm|state (state)] [--icache B --dcache B]
bench_state = our screen layer; bench_vterm = the libvterm screen layer (its hash is the oracle).
Cost model (core-doom Ф0.4 calibration on Verilator VexRiscv 4K/4K): CPI 1.7 without memory stalls,
33 clocks per cache line fill from the SDRAM; CPU clock 65 MHz (1024 x 768 pixel clock) and 80 MHz."""
import glob, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(HERE, '..', 'fw')
CPI, FILL = 1.7, 33


def main():
    args = sys.argv[1:]
    sd = args.pop(0) if args and not args[0].startswith('--') else os.path.join(HERE, 'build', 'streams')
    bench = 'state'
    if '--bench' in args:
        i = args.index('--bench'); bench = args[i + 1]; del args[i:i + 2]
    iss = os.path.join(HERE, 'iss')
    fw = os.path.join(FW, 'build-rv32imO2', f'bench_{bench}.bin')
    host = os.path.join(FW, 'build-host', 'bench_vterm')  # the oracle
    print(f'{"stream":8} {"bytes":>8} {"instr/B":>8} {"miss/kI":>8} {"KB/s@65":>8} {"KB/s@80":>8}  hash')
    ok = True
    for s in sorted(glob.glob(os.path.join(sd, '*.bin'))):
        r = subprocess.run([iss, fw, '--in', s] + args, capture_output=True, text=True)
        h = subprocess.run([host, s], capture_output=True, text=True).stdout.strip()
        marks = [l.split(',') for l in r.stdout.splitlines() if l.startswith('M,')]
        res = [l for l in r.stdout.splitlines() if l.startswith('bytes')]
        if r.returncode or len(marks) < 3 or not res:
            print(os.path.basename(s), 'FAILED', r.returncode, r.stderr[-300:], r.stdout[-300:])
            ok = False
            continue
        m = marks[2]  # counters of the stream (after the init mark)
        instr = int(m[2])
        miss = sum(int(v) for v in m[3:7])
        n = int(res[0].split()[1])
        cyc = instr * CPI + miss * FILL
        same = res[0].split()[-1] == h.split()[-1]
        ok &= same
        print(f'{os.path.basename(s)[:-4]:8} {n:8d} {instr / n:8.0f} {1000 * miss / instr:8.1f} '
              f'{n / (cyc / 65e6) / 1024:8.1f} {n / (cyc / 80e6) / 1024:8.1f}  {res[0].split()[-1]} {"= oracle" if same else "!= oracle " + h.split()[-1]}')
    sys.exit(0 if ok else 1)


main()
