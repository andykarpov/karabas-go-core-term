// Ф0 bench ISS of the term core: RV32IM + M-mode CSRs, no timing (a cost model is applied by run_f0.py).
// Forked from karabas-go-core-doom src/sim/iss.cpp (same decoder, same direct-mapped 32-byte-line cache model).
// Memory: SRAM 4 MB at 0x00000000, SDRAM 32 MB at 0x10000000, MMIO at 0x80000000 (uncached).
// usage: iss <fw.bin> [--org A (0x10000000)] [--in file: loaded at IN_BASE 0x11000000, size in MMIO 0x20]
//            [--max Minstr] [--icache B] [--dcache B] [--prof file: pc + ra every 256 instr]
// MMIO (fw/mmio.h): 0x00 write console byte, 0x08 write exit code, 0x0c read instructions (low 32),
//            0x10 write mark: prints "M,<v>,<instr>,<im_sram>,<im_sdram>,<dm_sram>,<dm_sdram>,<st_sram>,<st_sdram>"
//            (counters since the previous mark), 0x20 read input size.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

static const uint32_t SRAM_SIZE = 4u << 20, SDRAM_BASE = 0x10000000u, SDRAM_SIZE = 32u << 20, IN_BASE = 0x11000000u;
static std::vector<uint8_t> sram(SRAM_SIZE + 8), sdram(SDRAM_SIZE + 8);

static inline uint8_t *host(uint32_t a, int &reg) {
    if (a < SRAM_SIZE) { reg = 0; return &sram[a]; }
    if (a - SDRAM_BASE < SDRAM_SIZE) { reg = 1; return &sdram[a - SDRAM_BASE]; }
    reg = -1; return nullptr;
}

struct Cache {
    uint32_t lines = 0;
    std::vector<uint32_t> tag;
    void init(uint32_t bytes) { lines = bytes / 32; tag.assign(lines ? lines : 1, 0xffffffffu); }
    bool miss(uint32_t a) {
        if (!lines) return true;
        uint32_t l = a >> 5, i = l % lines;
        if (tag[i] != l) { tag[i] = l; return true; }
        return false;
    }
};
struct Cnt { uint64_t instr = 0, im[2] = {0, 0}, dm[2] = {0, 0}, st[2] = {0, 0}; };

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s fw.bin [--org A] [--in f] [--max Minstr] [--icache B] [--dcache B] [--prof f]\n", argv[0]); return 2; }
    uint64_t max_instr = 20000ull * 1000 * 1000;
    uint32_t icb = 4096, dcb = 4096, org = SDRAM_BASE, in_size = 0;
    const char *inf = 0;
    FILE *prof = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--max")) max_instr = strtoull(argv[++i], 0, 10) * 1000000ull;
        else if (!strcmp(argv[i], "--icache")) icb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dcache")) dcb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--org")) org = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--in")) inf = argv[++i];
        else if (!strcmp(argv[i], "--prof")) prof = fopen(argv[++i], "w");
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    int reg;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(host(org, reg), 1, 16u << 20, f);
    fclose(f);
    if (inf) {
        FILE *w = fopen(inf, "rb");
        if (!w) { perror(inf); return 2; }
        in_size = fread(&sdram[IN_BASE - SDRAM_BASE], 1, 8u << 20, w);
        fclose(w);
    }
    fprintf(stderr, "loaded %zu bytes at %08x, input %u bytes, I$ %u D$ %u\n", n, org, in_size, icb, dcb);
    Cache ic, dc; ic.init(icb); dc.init(dcb);
    uint32_t x[32] = {0}, pc = org;
    uint32_t mstatus = 0, mie = 0, mtvec = 0, mepc = 0, mcause = 0, mtval = 0;
    bool ten = false; uint32_t tcmp = 0;
    uint64_t ninstr = 0, misaligned = 0;
    Cnt tot, last;
    int exit_code = 0;
    bool done = false;
    std::string line;
    auto bad = [&](const char *what, uint32_t a) {
        fprintf(stderr, "%s %08x at pc %08x ra %08x after %llu\n", what, a, pc, x[1], (unsigned long long)ninstr);
        exit(3);
    };
    auto ld = [&](uint32_t a, int sz) -> uint32_t {
        if (a & 0x80000000u) {
            switch (a & 0xff) { case 0x0c: return (uint32_t)ninstr; case 0x20: return in_size; }
            return 0;
        }
        uint8_t *p = host(a, reg);
        if (!p) bad("load from", a);
        if (a & (sz - 1)) { if (!misaligned++) fprintf(stderr, "MISALIGNED load %d at %08x pc %08x\n", sz, a, pc); }
        if (dc.miss(a)) tot.dm[reg]++;
        uint32_t v = 0;
        memcpy(&v, p, sz);
        return v;
    };
    auto st = [&](uint32_t a, uint32_t v, int sz) {
        if (a & 0x80000000u) {
            uint32_t o = a & 0xff;
            if (o == 0x00) { char c = (char)v; if (c == '\n') { printf("%s\n", line.c_str()); line.clear(); } else line += c; }
            else if (o == 0x08) { exit_code = (int)v; done = true; }
            else if (o == 0x10) {
                tot.instr = ninstr;
                printf("M,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", v,
                       (unsigned long long)(tot.instr - last.instr),
                       (unsigned long long)(tot.im[0] - last.im[0]), (unsigned long long)(tot.im[1] - last.im[1]),
                       (unsigned long long)(tot.dm[0] - last.dm[0]), (unsigned long long)(tot.dm[1] - last.dm[1]),
                       (unsigned long long)(tot.st[0] - last.st[0]), (unsigned long long)(tot.st[1] - last.st[1]));
                last = tot;
            }
            return;
        }
        uint8_t *p = host(a, reg);
        if (!p) bad("store to", a);
        if (a & (sz - 1)) { if (!misaligned++) fprintf(stderr, "MISALIGNED store %d at %08x pc %08x\n", sz, a, pc); }
        tot.st[reg]++;
        memcpy(p, &v, sz);
    };
    while (!done && ninstr < max_instr) {
        if (prof && (ninstr & 255) == 0) fprintf(prof, "%x %x\n", pc, x[1]);
        uint8_t *ip = host(pc, reg);
        if (!ip) bad("fetch from", pc);
        if (ic.miss(pc)) tot.im[reg]++;
        uint32_t in; memcpy(&in, ip, 4);
        uint32_t op = in & 0x7f, rd = (in >> 7) & 31, f3 = (in >> 12) & 7, rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
        int32_t immi = (int32_t)in >> 20;
        int32_t imms = ((int32_t)in >> 25 << 5) | ((in >> 7) & 31);
        int32_t immb = (((int32_t)in >> 31) << 12) | (((in >> 7) & 1) << 11) | (((in >> 25) & 63) << 5) | (((in >> 8) & 15) << 1);
        int32_t immj = (((int32_t)in >> 31) << 20) | (in & 0xff000) | (((in >> 20) & 1) << 11) | (((in >> 21) & 1023) << 1);
        uint32_t a = x[rs1], b = x[rs2], r = 0, npc = pc + 4;
        bool wr = true;
        switch (op) {
        case 0x37: r = in & 0xfffff000u; break;
        case 0x17: r = pc + (in & 0xfffff000u); break;
        case 0x6f: r = pc + 4; npc = pc + immj; break;
        case 0x67: r = pc + 4; npc = (a + immi) & ~1u; break;
        case 0x63: {
            bool t = false;
            switch (f3) {
            case 0: t = a == b; break; case 1: t = a != b; break;
            case 4: t = (int32_t)a < (int32_t)b; break; case 5: t = (int32_t)a >= (int32_t)b; break;
            case 6: t = a < b; break; case 7: t = a >= b; break;
            default: fprintf(stderr, "bad branch at %08x\n", pc); return 3;
            }
            if (t) npc = pc + immb;
            wr = false; break;
        }
        case 0x03: {
            uint32_t ea = a + immi;
            switch (f3) {
            case 0: r = (uint32_t)(int32_t)(int8_t)ld(ea, 1); break;
            case 1: r = (uint32_t)(int32_t)(int16_t)ld(ea, 2); break;
            case 2: r = ld(ea, 4); break;
            case 4: r = ld(ea, 1) & 0xff; break;
            case 5: r = ld(ea, 2) & 0xffff; break;
            default: fprintf(stderr, "bad load at %08x\n", pc); return 3;
            }
            break;
        }
        case 0x23: {
            uint32_t ea = a + imms;
            st(ea, b, f3 == 0 ? 1 : f3 == 1 ? 2 : 4);
            wr = false; break;
        }
        case 0x13: {
            uint32_t sh = immi & 31;
            switch (f3) {
            case 0: r = a + immi; break;
            case 2: r = (int32_t)a < immi; break;
            case 3: r = a < (uint32_t)immi; break;
            case 4: r = a ^ immi; break;
            case 6: r = a | immi; break;
            case 7: r = a & immi; break;
            case 1: r = a << sh; break;
            case 5: r = (in >> 30 & 1) ? (uint32_t)((int32_t)a >> sh) : a >> sh; break;
            }
            break;
        }
        case 0x33:
            if (f7 == 1) {
                int64_t sa = (int32_t)a, sb = (int32_t)b;
                switch (f3) {
                case 0: r = a * b; break;
                case 1: r = (uint32_t)((sa * sb) >> 32); break;
                case 2: r = (uint32_t)((sa * (int64_t)(uint64_t)b) >> 32); break;
                case 3: r = (uint32_t)(((uint64_t)a * b) >> 32); break;
                case 4: r = b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
                case 5: r = b == 0 ? 0xffffffffu : a / b; break;
                case 6: r = b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
                case 7: r = b == 0 ? a : a % b; break;
                }
            } else {
                switch (f3) {
                case 0: r = (f7 & 0x20) ? a - b : a + b; break;
                case 1: r = a << (b & 31); break;
                case 2: r = (int32_t)a < (int32_t)b; break;
                case 3: r = a < b; break;
                case 4: r = a ^ b; break;
                case 5: r = (f7 & 0x20) ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
                case 6: r = a | b; break;
                case 7: r = a & b; break;
                }
            }
            break;
        case 0x0f: wr = false; break; // fence
        case 0x73: {
            if (in == 0x30200073u) { // mret
                pc = mepc; npc = mepc; mstatus = (mstatus & ~0x8u) | ((mstatus >> 4) & 8) | 0x80; wr = false; break;
            }
            uint32_t csr = in >> 20, *cp = 0;
            switch (csr) { case 0x300: cp = &mstatus; break; case 0x304: cp = &mie; break; case 0x305: cp = &mtvec; break;
                           case 0x341: cp = &mepc; break; case 0x342: cp = &mcause; break; case 0x343: cp = &mtval; break; }
            uint32_t old = cp ? *cp : (csr == 0x344 ? ((ten && (int32_t)((uint32_t)(ninstr / 32) - tcmp) >= 0) ? 0x80u : 0u) : 0u);
            uint32_t src = (f3 & 4) ? rs1 : a;
            if (cp) { if ((f3 & 3) == 1) *cp = src; else if ((f3 & 3) == 2) *cp |= src; else if ((f3 & 3) == 3) *cp &= ~src; }
            r = old; wr = (rd != 0) && f3 != 0;
            break;
        }
        default:
            fprintf(stderr, "illegal %08x at %08x after %llu\n", in, pc, (unsigned long long)ninstr);
            return 3;
        }
        if (wr && rd) x[rd] = r;
        pc = npc;
        ninstr++;
    }
    if (!line.empty()) printf("%s\n", line.c_str());
    if (prof) fclose(prof);
    printf("END instr %llu exit %d%s pc %08x misaligned %llu\n", (unsigned long long)ninstr, exit_code, done ? "" : " (max)", pc,
           (unsigned long long)misaligned);
    return done ? exit_code : 1;
}
