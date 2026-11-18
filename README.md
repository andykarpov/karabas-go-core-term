# term — a modern terminal core for Karabas Go / Mini / Mini G

Work in progress (phase 0). A terminal core for the Karabas Go, Mini and Mini G boards:

- `TERM=xterm-256color` level emulation (libvterm), 256 colours and truecolor, Unicode (UTF-8, wide characters);
- 1024 x 768 output, 128 x 48 cells, GNU Unifont glyphs;
- connections: telnet and SSH through the on-board ESP8266 (stock AT firmware), USB CDC through the board MCU;
- legacy encodings for BBS and old hosts (CP437, CP866, KOI8-R, CP1251, ISO-8859-1).

A VexRiscv (RV32IM) soft CPU runs the terminal firmware in C; the RTL draws the text screen.

The plan, in Russian: [docs/PLAN.md](docs/PLAN.md).

## Licence

GPL-3 (see LICENSE) for our files; imported code keeps its own licence and headers.
