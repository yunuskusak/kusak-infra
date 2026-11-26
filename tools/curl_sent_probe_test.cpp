// [PHASE37-TEST] C19a gercek libcurl kaniti: "istegin tek bayti bile gonderilmedi" sinyali dogru mu?
// Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/curl_sent_probe_test.cpp -o ~/kusak_build/tools/curl_sent_probe_test -lcurl -lpthread
// Kapsam: YALNIZ 127.0.0.1 uzerinde testin kendi acip kapattigi soketler. Dis ag / DNS / Binance YOK.
// Beklenen yardimci (uretim, henuz yok -> kirmizi): include/net/curl_sent.hpp
//     bool kusak::net::curl_request_maybe_sent(CURL* h) noexcept
//   false YALNIZ "hicbir bayt gonderilmedi" kanitlandiginda (pretransfer=0 ve request_size=0); aksi halde true.
// Senaryolar ve beklenti (helper sonucu ile sunucunun gercekte okudugu bayt sayisi tutarli olmali):
//   S1 baglanti reddi (kapali port)          -> rc=COULDNT_CONNECT, maybe_sent=false, sunucu bayt=0
//   S2 kabul + oku + yanit yok, timeout       -> rc=OPERATION_TIMEDOUT, maybe_sent=true, sunucu bayt>0
//   S3 kabul + oku + yanitsiz kapat           -> rc=GOT_NOTHING, maybe_sent=true, sunucu bayt>0
//   S4 kabul + 200 yanit                      -> rc=OK, maybe_sent=true
// Her senaryoda ham curl olcumleri (pretransfer, request_size, connect) basilir: helper yoksa bile kanit kaydi.
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if __has_include("../include/net/curl_sent.hpp")
#include "../include/net/curl_sent.hpp"
#define HAS_HELPER 1
#else
#define HAS_HELPER 0
#endif

static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

enum class Mode { READ_NO_REPLY, READ_CLOSE, REPLY_200 };

struct Server {
    int lfd = -1; int port = 0; std::thread th; std::atomic<long> bytes{0}; std::atomic<bool> stop{false};
    explicit Server(Mode m) {
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(lfd, (sockaddr*)&a, sizeof a); listen(lfd, 4);
        socklen_t l = sizeof a; getsockname(lfd, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        th = std::thread([this, m] {
            int c = accept(lfd, nullptr, nullptr);
            if (c < 0) return;
            timeval tv{0, 200000}; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char buf[4096];
            for (int i = 0; i < 10 && !stop; ++i) {
                ssize_t n = recv(c, buf, sizeof buf, 0);
                if (n > 0) { bytes += n; if (std::string(buf, n).find("\r\n\r\n") != std::string::npos && m != Mode::READ_NO_REPLY) break; }
                else if (n == 0) break;
            }
            if (m == Mode::REPLY_200) {
                const char* r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                send(c, r, std::strlen(r), 0);
            }
            if (m == Mode::READ_NO_REPLY) { while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            close(c);
        });
    }
    ~Server() { stop = true; shutdown(lfd, SHUT_RDWR); close(lfd); if (th.joinable()) th.join(); }
};

static int closed_port() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof a); socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l);
    int p = ntohs(a.sin_port); close(s); return p;
}

struct Probe { CURLcode rc; curl_off_t pre_us; long req_size; curl_off_t conn_us; int maybe_sent; };

static size_t sink(char*, size_t s, size_t n, void*) { return s * n; }

static Probe post(int port, long timeout_ms) {
    CURL* h = curl_easy_init();
    char url[64]; std::snprintf(url, sizeof url, "http://127.0.0.1:%d/api/v3/order", port);
    const char* body = "symbol=BTCUSDT&side=BUY&type=LIMIT&quantity=1&price=1&timestamp=1&signature=x";
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sink);
    Probe p{};
    p.rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_PRETRANSFER_TIME_T, &p.pre_us);
    curl_easy_getinfo(h, CURLINFO_REQUEST_SIZE, &p.req_size);
    curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T, &p.conn_us);
#if HAS_HELPER
    p.maybe_sent = kusak::net::curl_request_maybe_sent(h) ? 1 : 0;
#else
    p.maybe_sent = -1;
#endif
    curl_easy_cleanup(h);
    std::printf("  curl: rc=%d (%s) pretransfer_us=%lld request_size=%ld connect_us=%lld maybe_sent=%d\n",
        (int)p.rc, curl_easy_strerror(p.rc), (long long)p.pre_us, p.req_size, (long long)p.conn_us, p.maybe_sent);
    return p;
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::printf("bilgi: %s; yardimci %s\n", curl_version(), HAS_HELPER ? "VAR" : "YOK (kirmizi beklenir)");
    {
    std::printf("S1 baglanti reddi (kapali port): gonderilmedi\n");
    Probe p = post(closed_port(), 1000);
    CHECK(p.rc == CURLE_COULDNT_CONNECT);
    CHECK(p.req_size == 0);
    CHECK(p.maybe_sent == 0);
    }
    {
    std::printf("S2 kabul + oku + yanit yok (timeout 800 ms): gonderildi\n");
    Server s(Mode::READ_NO_REPLY);
    Probe p = post(s.port, 800);
    std::printf("  sunucu okunan bayt=%ld\n", s.bytes.load());
    CHECK(p.rc == CURLE_OPERATION_TIMEDOUT);
    CHECK(s.bytes.load() > 0);
    CHECK(p.maybe_sent == 1);
    }
    {
    std::printf("S3 kabul + oku + yanitsiz kapat: gonderildi\n");
    Server s(Mode::READ_CLOSE);
    Probe p = post(s.port, 2000);
    std::printf("  sunucu okunan bayt=%ld\n", s.bytes.load());
    CHECK(p.rc == CURLE_GOT_NOTHING);
    CHECK(s.bytes.load() > 0);
    CHECK(p.maybe_sent == 1);
    }
    {
    std::printf("S4 kabul + 200: gonderildi\n");
    Server s(Mode::REPLY_200);
    Probe p = post(s.port, 2000);
    CHECK(p.rc == CURLE_OK);
    CHECK(p.maybe_sent == 1);
    }
    curl_global_cleanup();
    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
