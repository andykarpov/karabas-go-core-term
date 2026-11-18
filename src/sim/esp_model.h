// Ф2 sim: an ESP8266 with the NonOS AT 1.7 firmware on the UART pins of term_soc (bit level, CLKS clocks a
// bit), as the module of the bench answered (src/tools/esp_probe.py, docs/PLAN.md Ф0.3): echo of the
// commands (ATE1), +IPD,n: frames, AT+CIPSEND=n -> "> " -> n bytes -> "Recv n bytes" "SEND OK", CLOSED
// when the remote side closes. AT+CIPSTART="TCP","host",port opens a real TCP socket of this PC (the tests
// run a server on 127.0.0.1). The module sends only while CTS (cts_n) is 0.
#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

struct EspModel {
    int clks;
    // receive from the core
    int r_st = 0, r_cnt = 0, r_bit = 0; uint8_t r_sh = 0;
    // transmit to the core
    std::deque<uint8_t> out;
    int t_bit = -1, t_cnt = 0; uint16_t t_sh = 0;
    bool txd = true;
    // AT state
    std::string line;
    int sock = -1;
    int send_left = 0;               // bytes of a CIPSEND still to come
    std::string send_buf;
    uint64_t delay_until = 0;        // the module "works" (no +IPD) until this clock
    uint64_t k = 0;
    bool log = false;
    FILE *at_log = nullptr;          // the AT lines the core sent

    explicit EspModel(int clks_per_bit) : clks(clks_per_bit) {}

    void say(const std::string &s) { for (char c : s) out.push_back((uint8_t)c); }

    void command(const std::string &c) {
        if (at_log) fprintf(at_log, "%s\n", c.c_str());
        say(c + "\r\n");                                    // the echo
        if (c == "AT" || c == "ATE0" || c == "ATE1" || c.rfind("AT+CIPMODE=0", 0) == 0) { say("\r\nOK\r\n"); return; }
        if (c == "AT+CIPMUX=0") { say(sock >= 0 ? "link is builded\r\n\r\nERROR\r\n" : "\r\nOK\r\n"); return; }
        if (c == "AT+GMR") { say("AT version:1.7.6.0(sim)\r\nSDK version:3.0.6-dev\r\nOK\r\n"); return; }
        if (c == "AT+CIFSR") { say("+CIFSR:STAIP,\"192.168.1.114\"\r\n+CIFSR:STAMAC,\"e8:68:e7:00:00:01\"\r\n\r\nOK\r\n"); return; }
        if (c == "AT+CWJAP_CUR?" || c == "AT+CWJAP?") { say("+CWJAP_CUR:\"simnet\",\"00:11:22:33:44:55\",6,-50\r\nOK\r\n"); return; }
        if (c == "AT+CIPSTATUS") { say(sock >= 0 ? "STATUS:3\r\n" : "STATUS:4\r\n"); say("\r\nOK\r\n"); return; }
        if (c == "AT+CIPCLOSE") {
            if (sock >= 0) { close(sock); sock = -1; say("CLOSED\r\n\r\nOK\r\n"); } else say("\r\nERROR\r\n");
            return;
        }
        if (c.rfind("AT+CIPSTART=\"TCP\",\"", 0) == 0) {
            size_t a = 19, b = c.find('"', a);
            std::string host = c.substr(a, b - a);
            int port = atoi(c.c_str() + b + 2);
            if (sock >= 0) { say("ALREADY CONNECTED\r\n\r\nERROR\r\n"); return; }
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
            char ps[16]; snprintf(ps, sizeof ps, "%d", port);
            if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0) { say("DNS Fail\r\n\r\nERROR\r\n"); return; }
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); freeaddrinfo(res); say("\r\nERROR\r\nCLOSED\r\n"); return; }
            freeaddrinfo(res);
            fcntl(s, F_SETFL, O_NONBLOCK);
            sock = s;
            say("CONNECT\r\n\r\nOK\r\n");
            return;
        }
        if (c.rfind("AT+CIPSEND=", 0) == 0) {
            int n = atoi(c.c_str() + 11);
            if (sock < 0) { say("link is not valid\r\n\r\nERROR\r\n"); return; }
            if (n < 1 || n > 2048) { say("\r\nERROR\r\n"); return; }
            send_left = n; send_buf.clear();
            say("\r\nOK\r\n> ");
            return;
        }
        say("\r\nERROR\r\n");
    }

    void got_byte(uint8_t b) {
        if (send_left > 0) {
            send_buf.push_back((char)b);
            if (--send_left == 0) {
                if (sock >= 0) { ssize_t w = write(sock, send_buf.data(), send_buf.size()); (void)w; }
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
        // the socket: +IPD frames (when the line is idle and nothing is queued), CLOSED
        if (sock >= 0 && out.empty() && t_bit < 0 && send_left == 0 && (k & 1023) == 0) {
            uint8_t buf[1460];
            ssize_t n = read(sock, buf, sizeof buf);
            if (n > 0) { say("\r\n+IPD," + std::to_string(n) + ":"); for (ssize_t i = 0; i < n; i++) out.push_back(buf[i]); }
            else if (n == 0) { close(sock); sock = -1; say("CLOSED\r\n"); }
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
