# term — a modern terminal core for Karabas Go / Mini / Mini G

[Русская версия](README.ru.md)

A terminal for the Karabas Go board family (Xilinx Spartan-6 XC6SLX25): a VexRiscv (RV32IM) soft CPU at 65 MHz runs
the terminal firmware in C ([libvterm](https://www.leonerd.org.uk/code/libvterm/) and our own screen layer), the RTL
draws the text screen. Telnet and SSH go through the ESP8266 module of the board (its stock AT firmware); the core
serial port of the board (its second USB serial port) connects it to a PC.

| Board | Core file | Video | Bell |
|---|---|---|---|
| Karabas Go | `term.kg` | VGA 1024 x 768 | I2S DAC (PCM5102) |
| Karabas Mini | `term.kg` | HDMI 1024 x 768 | HDMI, analog output (sigma-delta) |
| Karabas Mini G | `term.kg` | HDMI 1024 x 768 | HDMI, I2S DAC |

Status: checked on Mini G (USB serial, telnet, SSH, charsets, phonebook, tabs, the mouse). The Go and Mini builds have not been run on a
board yet.

## Features

- **Emulation:** `TERM=xterm-256color`: 256 colours and truecolor, bold / italic / underline / blink / reverse /
  strike, the alternate screen, scroll regions, xterm keys, bracketed paste, xterm mouse tracking.
- **Screen:** 1024 x 768 at 60 Hz, cells of 8 x 16 pixels: a tab bar and a session of 128 x 47; ~200 rows of
  scrollback per tab.
- **Tabs:** 4 sessions at once (one of them can be USB serial), each with its own screen and history.
- **Mouse:** a pointer, clicks on the tab bar, xterm mouse tracking of the applications, the wheel.
- **Unicode:** UTF-8, wide (CJK) characters, combining marks; glyphs of [GNU Unifont](https://unifoundry.com)
  (the whole Basic Multilingual Plane with the font file on the SD card; Latin, Greek, Cyrillic, box drawing, blocks,
  braille and more without it).
- **Connections:**
  - **SSH** through the ESP8266: our own small client on [Monocypher](https://monocypher.org): curve25519-sha256 key
    exchange (strict kex), ssh-ed25519 host keys, chacha20-poly1305 cipher; log in by key (ed25519), by password or
    keyboard-interactive; known_hosts and the user key are kept on the SD card;
  - **telnet** through the ESP8266 (terminal type, window size, binary mode);
  - **raw TCP** through the ESP8266;
  - **USB serial**: the core serial port of the board, `-if02` (a getty or a serial console on the PC).
- **Charsets** of old hosts and BBS: UTF-8, CP437, CP866, KOI8-R, CP1251, ISO-8859-1.
- **Phonebook** on the SD card: the connections that worked (64), with the protocol, port, user and charset.

Not done: RSA / ECDSA host keys (only ssh-ed25519: OpenSSH since 2014 and Dropbear since 2020 have it), port
forwarding, selecting text with the mouse.

## Getting started

1. Copy `src/term.kg` (one file for Go, Mini and Mini G) to the root of the SD card, and the font
   `sd/term/font.bin` to `/term/font.bin` on the card.
2. For telnet and SSH: connect the ESP8266 to your Wi-Fi once in the boot core (**Setup Wi-Fi**). The term core uses
   that connection; it never changes the settings stored in the module.
3. Choose term in the core menu. The connection screen opens; the tab bar above it shows the IP address of the module.

The OSD (**Menu+Esc**; "Menu" is the Windows key) has the font file (**Font**, Menu+F1, loaded at the start) and
**Reset**.

## The connection screen

| Key | Action |
|---|---|
| ↑ ↓ / Tab | the next field |
| ← → | change Protocol / Charset |
| Enter | connect (on a phonebook entry: connect to it) |
| Del | remove the phonebook entry |
| Esc | back to the session |
| Scroll Lock | open this screen from the session (and back) |
| F8 | disconnect |
| F2 | type your SSH public key into the session |

Fields: **Protocol** (SSH / telnet / raw TCP / USB serial), **Host** (a name or an IP address), **Port** (22 / 23 by
the protocol), **User** (SSH; empty = asked at the login), **Charset**.

In the session, **Shift+PgUp / Shift+PgDn** scroll back through the history; any other key returns to the screen.

## Tabs and the mouse

| Key | Action |
|---|---|
| Ctrl+Shift+T | a new tab (its connection screen opens) |
| Ctrl+Shift+W | close the tab (with a session: asked first, Y / N) |
| Ctrl+PgUp / Ctrl+PgDn | the tab before / after |
| Ctrl+Shift+1..4 | tab 1..4 |

The tab bar shows each tab (its number and host; `•`: new output or a bell while not shown), `×` closes a tab, `+`
opens one; a click on a tab shows it, the wheel over the bar goes through the tabs. The pointer hides after 3 s
without a move and while typing. The wheel in a session: to the application if it tracks the mouse; else on the
main screen it scrolls the history, on the alternate screen (less, vim, man) it sends the arrow keys.

## SSH

- The first connection to a host shows the fingerprint of its key, as OpenSSH does (`ED25519 key fingerprint is
  SHA256:...`): compare it with `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the server and answer `yes`.
  A host whose key changed later gets the OpenSSH warning; only the word `replace` accepts the new key.
- Passwords and keyboard-interactive answers are typed on the session screen without echo; Ctrl+C cancels.
- **Log in by key:** the core makes its ed25519 key at the first SSH connection; the connection screen shows its
  fingerprint. Log in once by password, then on the shell prompt type `echo '`, press **Scroll Lock**, **F2** (the key
  line is typed), then `' >> ~/.ssh/authorized_keys` and Enter. The next logins need no password.
- The keys live in the save file of the core, `/term/font.sav` on the SD card (with the phonebook), unencrypted, like
  an `~/.ssh/id_ed25519` without a passphrase: keep the card to yourself.
- The randomness for the keys comes from the timing of the keyboard, the mouse and the network, mixed with a seed
  kept in the same file; on the very first start press a few keys before the first SSH connection.

## USB serial

The board shows two USB serial ports on the PC (firmware v26111901 or newer). The core talks on the second one,
`/dev/serial/by-id/usb-Karabas_Go_*-if02` on Linux, a clean byte stream. A login prompt on the PC, for example:

```
sudo agetty -L 115200 /dev/serial/by-id/usb-Karabas_Go_<serial>-if02 xterm-256color
```

(the speed does not matter on a USB port). Choose **USB serial** on the connection screen.

## Building

ISE 14.7 (`src/Makefile`: `make go`, `make mini`, `make minig`), the RISC-V GCC of xPack for the firmware
(`make -C src/fw hw`, `make rom`). Tests without the board: `src/sim/run_f3_host.sh` (the SSH client against a test
server, python3-asyncssh), `src/sim/run_f1.sh`, `run_f2.py`, `run_f3.py` (the SoC in Verilator). The Go and Mini tops
are made from the Mini G top by `src/tools/mktops.py`. The plan and the decisions, in Russian: [docs/PLAN.md](docs/PLAN.md).

## Licence

GPL-3 (see LICENSE) for our files; imported code keeps its own licence and headers: libvterm (MIT), Monocypher
(BSD-2 / CC0), VexRiscv (MIT), GNU Unifont (GPL-2+ with the font embedding exception / OFL-1.1).
