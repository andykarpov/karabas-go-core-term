// Ф1 unit sim of term_video: a screen image (cells in an SRAM model laid out as term_soc / tscreen),
// the gather port served like term_sram (a take every 2 clocks in runs, a CPU-like gap between runs),
// a frame captured to PPM; ref_video.py renders the same screen in Python and compares.
// usage: tb_video <sram image 4 MB> <regs file> <out.ppm> [frames 2]
//   regs file: lines "row n v", "pal i rgb", "deffg rgb", "defbg rgb", "cursor v" (hex)
#include "Vterm_video.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 4) return 2;
    std::vector<uint8_t> sram(4u << 20);
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2; size_t r = fread(sram.data(), 1, sram.size(), f); (void)r; fclose(f);
    int frames_max = argc > 4 ? atoi(argv[4]) : 2;
    Vterm_video *t = new Vterm_video;
    t->clk = 0; t->reset = 1; t->show = 1; t->vid_take = 0; t->vid_dv = 0; t->vid_d = 0;
    t->row_we = 0; t->pal_we = 0; t->def_fg = 0xd0d0d0; t->def_bg = 0; t->cur_x = 0; t->cur_y = 0; t->cur_on = 0;
    t->cur_shape = 0; t->cur_blink = 0;
    struct W { int kind; uint32_t a, v; };
    std::vector<W> writes;
    FILE *rf = fopen(argv[2], "r"); char kind[16]; unsigned a, v;
    while (fscanf(rf, "%15s", kind) == 1) {
        if (!strcmp(kind, "row")) { fscanf(rf, "%x %x", &a, &v); writes.push_back({0, a, v}); }
        else if (!strcmp(kind, "pal")) { fscanf(rf, "%x %x", &a, &v); writes.push_back({1, a, v}); }
        else if (!strcmp(kind, "deffg")) { fscanf(rf, "%x", &v); t->def_fg = v; }
        else if (!strcmp(kind, "defbg")) { fscanf(rf, "%x", &v); t->def_bg = v; }
        else if (!strcmp(kind, "cursor")) { fscanf(rf, "%x", &v); t->cur_x = v & 0x7f; t->cur_y = (v >> 8) & 0x3f; t->cur_on = (v >> 15) & 1; t->cur_shape = (v >> 16) & 3; t->cur_blink = 0; }
    }
    fclose(rf);
    // the gather port model: state like term_sram S_VID
    // as term_sram: take / dv are registers (seen by the video in the clock after the model decided them)
    int st = 0, ph = 0, run = 0, gap = 0; bool out = false, more = false;
    int take_r = 0, dv_r = 0; uint16_t d_r = 0;
    uint32_t ma = 0; int pend_dv = 0; uint16_t md_in = 0;
    std::vector<uint8_t> img(1024 * 768 * 3);
    int frame = 0, x = 0, y = 0, last_hs = 1, last_vs = 1;
    size_t wi = 0;
    uint64_t k = 0;
    while (frame < frames_max) {
        if (k == 10) t->reset = 0;
        // register writes, one per clock at the start
        t->row_we = 0; t->pal_we = 0;
        if (k > 10 && wi < writes.size()) {
            W &w = writes[wi++];
            if (w.kind == 0) { t->row_we = 1; t->row_wa = w.a; t->row_wd = w.v; }
            else { t->pal_we = 1; t->pal_wa = w.a; t->pal_wd = w.v; }
        }
        // the gather port (inputs for this edge, from the state of the last one)
        bool pend = t->vid_pend; uint32_t adr = t->vid_adr;
        int take = 0, dv = 0; uint16_t d = 0;
        uint16_t pins; memcpy(&pins, &sram[(ma & 0x1fffff) * 2], 2);
        if (st == 0) {
            if (gap) gap--;
            else if (pend) { ma = adr; take = 1; run = 31; ph = 0; out = false; more = true; st = 1; }
        } else {
            if (ph) {
                md_in = pins; out = true;
                if (run && pend) { ma = adr; take = 1; run--; more = true; } else more = false;
            } else if (out) { dv = 1; d = md_in; if (!more) { st = 0; gap = 34; } }
            ph ^= 1;
        }
        t->vid_take = take_r; t->vid_dv = dv_r; t->vid_d = d_r;
        take_r = take; dv_r = dv; d_r = d;
        t->clk = 1; t->eval();
        if (t->de) { if (x < 1024 && y < 768) { uint8_t *p = &img[(y * 1024 + x) * 3]; p[0] = t->rgb >> 16; p[1] = t->rgb >> 8; p[2] = t->rgb; } x++; }
        if (!t->hs && last_hs) { if (x > 0) y++; x = 0; }
        last_hs = t->hs;
        if (!t->vs && last_vs) {
            if (frame == frames_max - 1) { FILE *o = fopen(argv[3], "wb"); fprintf(o, "P6 1024 768 255\n"); fwrite(img.data(), 1, img.size(), o); fclose(o); }
            printf("FRAME %d lines %d\n", frame, y); frame++; x = 0; y = 0;
        }
        last_vs = t->vs;
        t->clk = 0; t->eval();
        k++;
    }
    delete t;
    return 0;
}
