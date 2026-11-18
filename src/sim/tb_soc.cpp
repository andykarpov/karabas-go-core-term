// Ф1 sim: term_soc (VexRiscv + term_sram + term_video) on a pin model of the SRAM (as core-doom tb_rtl:
// MD_I after edge k = the halfword at the MA of edge k-1; a write lane while /WE is low).
// usage: tb_soc <term.bin> <font_full.bin> [--in stream] [--rate clocks per byte (1000)] [--frames N (3)]
//               [--shot frame file.ppm]... [--start-ms M: the stream starts after M ms (20)]
//               [--host-out file: the bytes the core sent to the host]
//               [--hash-frames N: N frames after the stream was fed, SW bit 31 is set (the firmware prints
//               HASH of the visible cells) and the run stops a frame later]
// The firmware at SRAM 0, the font at 0x200000 are put in the model (on the board: the loader / SD).
// Prints the debug console bytes, at the end the frames and the bytes fed.
#include "Vterm_soc.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> sram(4u << 20);

static size_t load(const char *path, uint32_t at) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t n = fread(&sram[at], 1, sram.size() - at, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: tb_soc term.bin font_full.bin [--in f] [--rate c] [--frames n] [--shot f file]\n"); return 2; }
    // argv[1] ending in .kg: its ROMS go in through the loader port (as the MCU does), argv[2] "-" = no font
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> roms;
    size_t nfw = 0, nfont = 0;
    if (strstr(argv[1], ".kg")) {
        FILE *f = fopen(argv[1], "rb"); std::vector<uint8_t> kg; int c; while ((c = fgetc(f)) >= 0) kg.push_back((uint8_t)c); fclose(f);
        auto u16 = [&](size_t o) { return (uint32_t)kg[o] | kg[o + 1] << 8; };
        auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
        for (uint32_t i = 0; i < u16(8); i++) {
            size_t e = 32 + 16 * i;
            if (u16(e) != 8) continue;
            size_t p = u32(e + 4), end = p + u32(e + 8);
            while (p < end) { uint32_t ln = u32(p) & 0x7fffffff, ad = u32(p + 4); p += 8;
                roms.push_back({ad, std::vector<uint8_t>(kg.begin() + p, kg.begin() + p + ln)}); p += ln; nfw += ln; }
        }
    } else nfw = load(argv[1], 0);
    if (strcmp(argv[2], "-")) nfont = load(argv[2], 0x200000);
    std::vector<uint8_t> in;
    FILE *host_out = 0;
    int rate = 1000, frames_max = 3, start_ms = 20, hash_frames = -1, fed_frame = -1;
    std::vector<std::pair<int, std::string>> shots;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--in")) { FILE *f = fopen(argv[++i], "rb"); if (!f) { perror(argv[i]); return 2; } int c; while ((c = fgetc(f)) >= 0) in.push_back((uint8_t)c); fclose(f); }
        else if (!strcmp(argv[i], "--rate")) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames")) frames_max = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--start-ms")) start_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--host-out")) host_out = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--hash-frames")) { hash_frames = atoi(argv[++i]); frames_max = 1000000; }
        else if (!strcmp(argv[i], "--shot")) { int fn = atoi(argv[++i]); shots.push_back({fn, argv[++i]}); }
    }
    fprintf(stderr, "fw %zu bytes, font %zu bytes, stream %zu bytes\n", nfw, nfont, in.size());
    Vterm_soc *t = new Vterm_soc;
    t->clk = 0; t->reset = 1; t->ld_kind = 0; t->ld_slot = 0; t->ld_active = 0; t->ld_addr = 0; t->ld_data = 0; t->ld_wr = 0;
    t->kbd = 0; t->kbd_wr = 0; t->ms_x = t->ms_y = t->ms_b = 0; t->ms_wr = 0; t->sw = 0;
    t->ser_out_data = 0; t->ser_out_wr = 0; t->ser_in_full = 0; t->ser_cfg = 0; t->MD_I = 0;
    uint32_t prev_ma = 0;
    uint64_t k = 0;
    size_t fed = 0;
    int frame = 0, last_vs = 1, x = 0, y = 0, last_hs = 1;
    std::vector<uint8_t> img(1024 * 768 * 3);
    std::string line;
    uint64_t start_at = (uint64_t)start_ms * 65000, next_feed = start_at;
    size_t host_bytes = 0;
    size_t ri = 0, rj = 0;
    int pkt_left = 0;
    uint64_t ld_next = 0, ld_lost = 0;
    bool loading = !roms.empty();
    if (loading) t->ld_active = 1;
    while (frame < frames_max) {
        if (k == 20) t->reset = 0;
        // the loader as link v2 (kg_l2_load): LD_READY is asked once per 512-byte DATA packet, then a byte
        // every 16 clocks (250 ns at 65 MHz) without a handshake; a byte given while the core is not ready
        // is counted as lost
        t->ld_wr = 0;
        if (loading && k > 30 && k >= ld_next) {
            if (ri < roms.size()) {
                if (pkt_left == 0) { if (t->ld_ready) pkt_left = 512; }
                if (pkt_left) {
                    if (!t->ld_ready) ld_lost++;
                    t->ld_kind = 0; t->ld_addr = roms[ri].first + (uint32_t)rj; t->ld_data = roms[ri].second[rj]; t->ld_wr = 1;
                    pkt_left--; ld_next = k + 16;
                    if (++rj == roms[ri].second.size()) { ri++; rj = 0; pkt_left = 0; }
                }
            } else { loading = false; t->ld_active = 0; printf("LOADED at clock %llu, bytes lost %llu\n", (unsigned long long)k, (unsigned long long)ld_lost); }
        }
        t->clk = 1; t->eval();
        // SRAM pins
        uint32_t ma = t->MA & 0x1fffff;
        if (!(t->MWR_N & 1) && !(t->MD_OE_N & 0xff)) sram[ma * 2] = t->MD_O & 0xff;
        if (!(t->MWR_N & 2) && !(t->MD_OE_N & 0xff00)) sram[ma * 2 + 1] = t->MD_O >> 8;
        { uint16_t v; memcpy(&v, &sram[prev_ma * 2], 2); t->MD_I = v; }
        prev_ma = ma;
        // serial: a byte per `rate` clocks while the core is ready
        t->ser_out_wr = 0;
        if (fed < in.size() && k >= next_feed && t->ser_out_ready) {
            t->ser_out_data = in[fed++]; t->ser_out_wr = 1; next_feed = k + rate;
        }
        if (t->ser_in_wr) { host_bytes++; if (host_out) fputc(t->ser_in_data, host_out); }
        if (t->dbg_wr) { char c = (char)t->dbg_byte; if (c == '\n') { printf("DBG %s\n", line.c_str()); line.clear(); } else line += c; }
        // video capture (de / hs / vs of the clock after the edge)
        if (t->de) { if (x < 1024 && y < 768) { uint8_t *p = &img[(y * 1024 + x) * 3]; p[0] = t->rgb >> 16; p[1] = t->rgb >> 8; p[2] = t->rgb; } x++; }
        if (!t->hs && last_hs) { if (x > 0) y++; x = 0; }
        last_hs = t->hs;
        if (!t->vs && last_vs) {
            for (auto &s : shots) if (s.first == frame) {
                FILE *f = fopen(s.second.c_str(), "wb"); fprintf(f, "P6 1024 768 255\n"); fwrite(img.data(), 1, img.size(), f); fclose(f);
            }
            printf("FRAME %d lines %d cpu_run %d fed %zu host %zu\n", frame, y, t->cpu_run, fed, host_bytes);
            fflush(stdout);
            frame++; y = 0; x = 0;
            if (hash_frames >= 0) {
                if (fed == in.size() && fed_frame < 0) fed_frame = frame;
                if (fed_frame >= 0 && frame == fed_frame + hash_frames) t->sw = 0x80000000u;
                if (fed_frame >= 0 && frame == fed_frame + hash_frames + 2) frames_max = frame;
            }
        }
        last_vs = t->vs;
        t->clk = 0; t->eval();
        k++;
        if (t->dbg_exit & 0x100) { printf("EXIT %d\n", t->dbg_exit & 0xff); break; }
    }
    if (!line.empty()) printf("DBG %s\n", line.c_str());
    if (host_out) fclose(host_out);
    if (getenv("TB_DUMP")) { FILE *f = fopen(getenv("TB_DUMP"), "wb"); fwrite(sram.data(), 1, sram.size(), f); fclose(f); }
    delete t;
    return 0;
}
