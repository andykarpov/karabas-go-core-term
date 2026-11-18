# ROMs of the term core (the ROMS of the .kg)

- `term.bin`: the firmware (src/fw `make hw`, SRAM address 0).
- `base_XXXX.bin`: the base glyph blocks (Latin, Greek, Cyrillic, punctuation, symbols, arrows, box
  drawing, blocks, braille, U+FFFD) at SRAM 0x200000 + codepoint * 32, made by `src/tools/mkfont.py` from
  **GNU Unifont 17.0.05** (https://unifoundry.com, GPL-2+ with the GNU font embedding exception; OFL-1.1
  for the font files). The whole font (`font_full.bin`, 2 MB) is a file on the SD card (OSD item Font),
  not in the repository.
