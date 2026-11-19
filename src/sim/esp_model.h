// Ф2 / Ф5 sim: an ESP8266 with the NonOS AT 1.7 firmware on the UART pins of term_soc (bit level, CLKS clocks a
// bit), as the module of the bench answered (src/tools/esp_probe.py, esp_mux_probe.py; docs/PLAN.md Ф0.3, Ф5.0):
// echo of the commands (ATE1), CIPMUX 0 (+IPD,n:, CONNECT, CLOSED) and CIPMUX 1 (links 0..4: +IPD,id,n:,
// id,CONNECT, id,CLOSED, CIPSEND=id,n, CIPCLOSE=id, CIPCLOSE=5), AT+CIPSEND -> "> " -> n bytes -> "Recv n bytes"
// "SEND OK"; +IPD frames of the links can come while a CIPSEND waits for its bytes (the bench showed them between
// "> " and SEND OK). AT+CIPSTART opens a real TCP socket of this PC (the tests run servers on 127.0.0.1). The
// module sends only while CTS (cts_n) is 0.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

struct EspModel {
    static const int LINKS = 5;
    int clks;
    // receive from the core
    int r_st = 0, r_cnt = 0, r_bit = 0; uint8_t r_sh = 0;
    // transmit to the core
    std::deque<uint8_t> out;
    int t_bit = -1, t_cnt = 0; uint16_t t_sh = 0;
    bool txd = true;
    // AT state
    std::string line;
    int mux = 0;
    int sock[LINKS] = { -1, -1, -1, -1, -1 };
    int send_left = 0, send_link = 0;    // bytes of a CIPSEND still to come, to this link
    std::string send_buf;
    uint64_t k = 0;
    int rr = 0;                          // the next link to read (round robin)
    int max_links = 0;                   // most links open at once (the tests check it)
    bool log = false;
    FILE *at_log = nullptr;              // the AT lines the core sent

    explicit EspModel(int clks_per_bit) : clks(clks_per_bit) {}

    void say(const std::string &s) { for (char c : s) out.push_back((uint8_t)c); }
    int open_links() const { int n = 0; for (int s : sock) n += s >= 0; return n; }
    std::string pre(int id) const { return mux ? std::to_string(id) + "," : ""; }

    void close_link(int id) { close(sock[id]); sock[id] = -1; say(pre(id) + "CLOSED\r\n"); }

    void start(int id, const std::string &args) {         // args: "TCP","host",port
        if (args.rfind("\"TCP\",\"", 0) != 0) { say("\r\nERROR\r\n"); return; }
        size_t a = 7, b = args.find('"', a);
        std::string host = args.substr(a, b - a);
        int port = atoi(args.c_str() + b + 2);
        if (sock[id] >= 0) { say("ALREADY CONNECTED\r\n\r\nERROR\r\n"); return; }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ps[16]; snprintf(ps, sizeof ps, "%d", port);
        if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0) { say("DNS Fail\r\n\r\nERROR\r\n"); return; }
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); freeaddrinfo(res); say("\r\nERROR\r\n" + pre(id) + "CLOSED\r\n"); return; }
        freeaddrinfo(res);
        fcntl(s, F_SETFL, O_NONBLOCK);
        sock[id] = s;
        if (open_links() > max_links) max_links = open_links();
        say(pre(id) + "CONNECT\r\n\r\nOK\r\n");
    }

    void command(const std::string &c) {
        if (at_log) fprintf(at_log, "%s\n", c.c_str());
        say(c + "\r\n");                                    // the echo
        if (c == "AT" || c == "ATE0" || c == "ATE1" || c.rfind("AT+CIPMODE=0", 0) == 0) { say("\r\nOK\r\n"); return; }
        if (c == "AT+CIPMUX=0" || c == "AT+CIPMUX=1") {
            if (open_links()) { say("link is builded\r\n\r\nERROR\r\n"); return; }
            mux = c.back() - '0';
            say("\r\nOK\r\n");
            return;
        }
        if (c == "AT+CIPMUX?") { say("+CIPMUX:" + std::to_string(mux) + "\r\n\r\nOK\r\n"); return; }
        if (c == "AT+GMR") { say("AT version:1.7.6.0(sim)\r\nSDK version:3.0.6-dev\r\nOK\r\n"); return; }
        if (c == "AT+CIFSR") { say("+CIFSR:STAIP,\"192.168.1.114\"\r\n+CIFSR:STAMAC,\"e8:68:e7:00:00:01\"\r\n\r\nOK\r\n"); return; }
        if (c == "AT+CWJAP_CUR?" || c == "AT+CWJAP?") { say("+CWJAP_CUR:\"simnet\",\"00:11:22:33:44:55\",6,-50\r\nOK\r\n"); return; }
        if (c == "AT+CIPSTATUS") {
            say(open_links() ? "STATUS:3\r\n" : "STATUS:4\r\n");
            for (int i = 0; i < LINKS; i++) if (sock[i] >= 0) say("+CIPSTATUS:" + std::to_string(i) + ",\"TCP\",\"127.0.0.1\",0,0,0\r\n");
            say("\r\nOK\r\n");
            return;
        }
        if (c == "AT+CIPCLOSE") {
            if (mux) { say("MUX=1\r\n\r\nERROR\r\n"); return; }
            if (sock[0] >= 0) { close_link(0); say("\r\nOK\r\n"); } else say("\r\nERROR\r\n");
            return;
        }
        if (c.rfind("AT+CIPCLOSE=", 0) == 0) {
            if (!mux) { say("MUX=0\r\n\r\nERROR\r\n"); return; }
            int id = atoi(c.c_str() + 12);
            if (id == 5) { for (int i = 0; i < LINKS; i++) if (sock[i] >= 0) close_link(i); say("\r\nOK\r\n"); return; }
            if (id < 0 || id >= LINKS || sock[id] < 0) { say("UNLINK\r\n\r\nERROR\r\n"); return; }
            close_link(id);
            say("\r\nOK\r\n");
            return;
        }
        if (c.rfind("AT+CIPSTART=", 0) == 0) {
            std::string a = c.substr(12);
            if (mux) {
                if (a.size() < 2 || a[0] < '0' || a[0] > '4' || a[1] != ',') { say("\r\nERROR\r\n"); return; }
                start(a[0] - '0', a.substr(2));
            } else {
                if (a[0] != '"') { say("\r\nERROR\r\n"); return; }
                start(0, a);
            }
            return;
        }
        if (c.rfind("AT+CIPSEND=", 0) == 0) {
            int id = 0, n;
            const char *p = c.c_str() + 11;
            if (mux) { id = atoi(p); const char *q = strchr(p, ','); if (!q) { say("\r\nERROR\r\n"); return; } n = atoi(q + 1); }
            else n = atoi(p);
            if (id < 0 || id >= LINKS || sock[id] < 0) { say("link is not valid\r\n\r\nERROR\r\n"); return; }
            if (n < 1 || n > 2048) { say("\r\nERROR\r\n"); return; }
            send_left = n; send_link = id; send_buf.clear();
            say("\r\nOK\r\n> ");
            return;
        }
        say("\r\nERROR\r\n");
    }

    void got_byte(uint8_t b) {
        if (send_left > 0) {
            send_buf.push_back((char)b);
            if (--send_left == 0) {
                if (sock[send_link] >= 0) { ssize_t w = write(sock[send_link], send_buf.data(), send_buf.size()); (void)w; }
                say("\r\nRecv " + std::to_string(send_buf.size()) + " bytes\r\n\r\nSEND OK\r\n");
            }
            return;
        }
        if (b == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) command(line);
            line.clear();
        } else line.push_back((char)b);
    }

    // one clock: txd_core = the core's TX pin, cts_n = the core's CTS; returns the module's TX pin
    bool tick(bool txd_core, bool cts_n) {
        k++;
        // receive (sample in the middle of the bits)
        switch (r_st) {
        case 0: if (!txd_core) { r_st = 1; r_cnt = clks / 2; } break;
        case 1: if (--r_cnt <= 0) { if (!txd_core) { r_st = 2; r_cnt = clks; r_bit = 0; } else r_st = 0; } break;
        case 2: if (--r_cnt <= 0) { r_sh = (uint8_t)((r_sh >> 1) | (txd_core ? 0x80 : 0)); r_cnt = clks; if (++r_bit == 8) r_st = 3; } break;
        case 3: if (--r_cnt <= 0) { r_st = 0; if (txd_core) got_byte(r_sh); } break;
        }
        // the sockets: +IPD frames (when the line is idle and nothing is queued), CLOSED; a link a turn
        if (out.empty() && t_bit < 0 && line.empty() && (k & 1023) == 0) {
            for (int j = 0; j < LINKS; j++) {
                int id = (rr + j) % LINKS;
                if (sock[id] < 0) continue;
                uint8_t buf[1460];
                ssize_t n = read(sock[id], buf, sizeof buf);
                if (n > 0) {
                    say("\r\n+IPD," + pre(id) + std::to_string(n) + ":");
                    for (ssize_t i = 0; i < n; i++) out.push_back(buf[i]);
                    rr = id + 1;
                    break;
                }
                if (n == 0) { close_link(id); rr = id + 1; break; }
            }
        }
        // transmit
        if (t_bit < 0) {
            if (!out.empty() && !cts_n) { t_sh = (uint16_t)(0x200 | (out.front() << 1)); out.pop_front(); t_bit = 0; t_cnt = clks; txd = false; }
        } else if (--t_cnt <= 0) {
            t_bit++;
            if (t_bit == 10) { t_bit = -1; txd = true; }
            else { txd = (t_sh >> t_bit) & 1; t_cnt = clks; }
        }
        return txd;
    }
};
