# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A modern terminal core for the Karabas Go, Mini and Mini G boards (Xilinx Spartan-6 XC6SLX25, ISE 14.7, XST): a VexRiscv
RV32IM soft CPU (as in karabas-go-core-doom) runs C firmware (libvterm = xterm-256color emulation, UTF-8, telnet, SSH,
AT driver of the ESP8266, connection screen); the RTL renders the cell screen (1024 x 768, 8 x 16 Unifont glyphs, 256
colours + truecolor). Transports: ESP8266 (stock AT firmware) and USB CDC through the MCU. Docs, in Russian:
`docs/PLAN.md` (decisions, phases). Follow the phases; record a changed decision in PLAN.md.

User decisions (2026-11-18): xterm-256color + Unicode + soft CPU; telnet, SSH, legacy encodings, USB CDC; core id `term`,
PRIVATE repo. Do not use core-vt100 or pdp2026 (vt10x) as a base or a reference.

## Rules of the core

The rules of the other karabas-go cores apply: standard video timing with syncs always present, `FT_CLK_OUT` about 8 MHz
on Mini / Mini G, Go `FDC_*` outputs 0 when unused, Mini / Mini G drive T3 (`FLASH_CS_N`) = 0, OSD switch index = item
position unless `"index"`, an OSD value shows 10 characters at most, FL slots 0..7 only, Verilog-2001 for XST (`sv2v`
for SV), VHDL-93 for XST, imported code marked `karabas:`, builds through `src/Makefile`.

- ESP8266: never change saved settings in the module (`AT+UART_DEF`, `AT+CWJAP` storage, `AT+RESTORE`): other cores
  and the boot core OSD Wi-Fi app share it. Leave it in AT command mode at 115200 whenever possible.
- SSH: host keys are checked (known_hosts on SD), randomness from a seeded entropy pool; never a fixed seed.
- Licences: our files GPL-3; keep every imported header as it is.
- **Ask the user before every check on the board** (and the HDMI capture, via `hwq`). Ask before a firmware flash.
- The connection screen and the OSD look need a frame shown to the user and their yes before merge.
- One branch per phase of `docs/PLAN.md`, one PR at its end.
- Heavy builds: Mini G only while working on one feature, all boards for a release; map with cost table 2 first.
- Simulations run in seconds; minutes-long runs only after asking.

## Memory

The Claude Code memory of this repo is shared with all karabas-go repos: `~/.claude/projects/<this project>/memory` is a
symlink to `../karabas-go-notes` (private repo `andykarpov/karabas-go-notes`). Its `MEMORY.md` is split by repo; commit
and push it after every memory change.
