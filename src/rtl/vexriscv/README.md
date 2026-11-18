VexRiscv (SpinalHDL/VexRiscv, MIT, see LICENSE), copied from karabas-go-core-agon src/rtl/vexriscv, generated with the wrapper of
litex-hub/pythondata-cpu-vexriscv 642ecfe (SpinalHDL 1.9.4, VexRiscv b6118e5c), JDK 17, sbt:

    git clone https://github.com/litex-hub/pythondata-cpu-vexriscv && cd pythondata-cpu-vexriscv
    git checkout 642ecfe && git submodule update --init pythondata_cpu_vexriscv/verilog/ext/VexRiscv
    cd pythondata_cpu_vexriscv/verilog
    sbt -batch compile "runMain vexriscv.GenCoreDefault --iCacheSize 16384 --dCacheSize 16384 --outputFile VexRiscv_16k"

The same run without options reproduces the VexRiscv.v of core-doom byte for byte (except the
"Git hash" comment line), so this core differs from it only in the cache sizes.

- VexRiscv_16k: rv32im (single-cycle mul), I$ 16K, D$ 16K (32-byte lines, 1 way, write-through),
  IO range = address bit 31 (uncached). XST xc6slx25-2 alone: 2249 LUT, 1396 FF, 19 RAMB16, 4 DSP48,
  115.6 MHz (2026-11-18; the 4K / 4K core: 2195 LUT, 6 RAMB16, 102.9 MHz).
