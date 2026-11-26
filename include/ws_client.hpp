// ============================================================================
// KUSAK-İNFRA V13 — HFT REFACTOR
// ws_client.hpp — Binance WebSocket Client (TLS, C++20, header-only)
//
// REVIZYON (v13 — STRATEJİK REFAKTÖR ŞARTNAMESİ V2.0):
//   parse_agg_trade()   → parse_agg_trade_dfa()   (Single-Pass DFA, branchless)
//   parse_book_ticker() → parse_book_ticker_dfa()  (Single-Pass DFA, branchless)
//
//   Eliminasyonlar:
//     - json::find_num(), json::find_uint() çağrıları kaldırıldı
//     - std::stod, std::stoull tamamen kaldırıldı
//     - double/float tamamen kaldırıldı (AggTrade, BookTicker)
//     - JSON key eşleştirmede strcmp/find kaldırıldı → switch jump table
//
//   Eklemeler:
//     - kusak::ws::dfa namespace: parse_fixed_point(), parse_uint64(),
//       skip_ws(), skip_value()
//     - AggTrade::price, qty → int64_t (×1e8 fixed-point)
//     - BookTicker::best_bid/ask/qty → int64_t (×1e8 fixed-point)
//     - dispatch_payload → '@' karakterine göre O(1) stream tipi tespiti
//
//   Hedef: Ingestion-to-Event Latency < 50 ns
//
// KORUNAN DUZELTILER (R7 + v11):
//   FIX-1: RFC 6455 fragmentation destegi
//   FIX-2: JSON parser exception-safe (json namespace korundu, depth/user hatlari icin)
//   FIX-3: expand() hata loglama
//   FIX-4: TLS hata detayi
//   FIX-5: Sunucu mask savunmasi
//   FIX-6: stop() noexcept wrap
//   v11:   UserDataWsClient + ExecutionReport + OrderErrorHandler
//
// SSL NOTU:
//   SSL_set_hostflags() icin 0; standart hostname dogrulamasi SSL_set1_host() ile.
// ============================================================================

#pragma once

#include <openssl/ssl.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/err.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/mman.h>    // memfd_create, mmap (ayna-haritali ring buffer)
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  // [FIX-CONN-1] TCP_KEEPIDLE/KEEPINTVL/KEEPCNT
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <x86intrin.h>   // _mm_pause(), __rdtscp(), _mm_lfence()

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <algorithm>
#include <iostream>

namespace kusak::ws {

// [PHASE3-KS] Kurulu market-WS baglantisinin kopma sayaci (kill-switch MAX_WS_DISCONNECT).
// WS thread yazar, strateji thread'i main.cpp uzerinden okur.
inline std::atomic<uint64_t> g_ws_disconnect_total{0};

// ============================================================================
// Base64 + WebSocket key helpers
// ============================================================================
namespace detail {

static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64_encode(const uint8_t* src, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(src[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(src[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(src[i + 2]);
        out += B64_TABLE[(b >> 18) & 0x3F];
        out += B64_TABLE[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_TABLE[(b >>  6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_TABLE[ b        & 0x3F] : '=';
    }
    return out;
}

inline std::string ws_accept_key(const std::string& key) {
    static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string cat = key + GUID;
    uint8_t sha[20];
    SHA1(reinterpret_cast<const uint8_t*>(cat.c_str()), cat.size(), sha);
    return base64_encode(sha, 20);
}

inline std::string ws_random_key() {
    uint8_t buf[16];
    RAND_bytes(buf, 16);
    return base64_encode(buf, 16);
}

// FIX-4: Tam TLS hata zinciri
inline std::string ssl_error_string() {
    char buf[256];
    std::string result;
    unsigned long code;
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buf, sizeof(buf));
        if (!result.empty()) result += " | ";
        result += buf;
    }
    return result.empty() ? "unknown TLS error" : result;
}

// GÖREV 1 (Yama Rapor v3.0 -- Handshake korlugunun ortadan kaldirilmasi):
// Handshake reddedildiginde HTTP durum kodunu, Binance'in gonderdigi response
// body'yi ve hata alan endpoint'i terminale (stderr) yazdirir. Sadece teshis
// amaclidir -- retry/backoff mantigina veya cekirdek mimariye dokunmaz.
inline void log_handshake_rejection(const char* tag,
                                     const std::string& host,
                                     uint16_t port,
                                     const std::string& path,
                                     const std::string& raw_resp) {
    std::string status_line = "(yanit alinamadi)";
    std::string status_code = "(bilinmiyor)";
    auto line_end = raw_resp.find("\r\n");
    if (line_end != std::string::npos) {
        status_line = raw_resp.substr(0, line_end);
        auto sp1 = status_line.find(' ');
        if (sp1 != std::string::npos) {
            auto sp2 = status_line.find(' ', sp1 + 1);
            status_code = status_line.substr(
                sp1 + 1, (sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1));
        }
    }

    std::string body = "(yok / henuz alinamadi)";
    auto hdr_end = raw_resp.find("\r\n\r\n");
    if (hdr_end != std::string::npos && hdr_end + 4 < raw_resp.size()) {
        body = raw_resp.substr(hdr_end + 4);
    }

    std::cerr << "[" << tag << "] ===== HANDSHAKE REDDI (teshis) =====\n"
              << "[" << tag << "]  Endpoint      : wss://" << host << ":" << port << path << "\n"
              << "[" << tag << "]  HTTP Status   : " << status_code << "\n"
              << "[" << tag << "]  Status Satiri : " << status_line << "\n"
              << "[" << tag << "]  Response Body : " << body << "\n"
              << "[" << tag << "]  (ham yanit " << raw_resp.size() << " byte)\n"
              << "[" << tag << "] =====================================\n";
}

inline std::vector<uint8_t> make_ws_frame(const std::string& msg) {
    size_t plen = msg.size();
    std::vector<uint8_t> frame;
    frame.reserve(2 + 8 + 4 + plen);
    frame.push_back(0x81);
    if (plen <= 125)        frame.push_back(static_cast<uint8_t>(plen) | 0x80);
    else if (plen <= 65535) { frame.push_back(0x7E | 0x80);
                               frame.push_back((plen >> 8) & 0xFF);
                               frame.push_back(plen & 0xFF); }
    else                    { frame.push_back(0x7F | 0x80);
                               for (int i = 7; i >= 0; --i)
                                   frame.push_back((plen >> (8*i)) & 0xFF); }
    uint8_t mk[4]; RAND_bytes(mk, 4);
    frame.push_back(mk[0]); frame.push_back(mk[1]);
    frame.push_back(mk[2]); frame.push_back(mk[3]);
    for (size_t i = 0; i < plen; ++i)
        frame.push_back(static_cast<uint8_t>(msg[i]) ^ mk[i % 4]);
    return frame;
}

inline std::vector<uint8_t> make_pong_frame(const std::vector<uint8_t>& ping) {
    uint8_t plen = static_cast<uint8_t>(std::min(ping.size(), size_t(125)));
    std::vector<uint8_t> frame;
    frame.push_back(0x8A); frame.push_back(plen | 0x80);
    uint8_t mk[4]; RAND_bytes(mk, 4);
    frame.push_back(mk[0]); frame.push_back(mk[1]);
    frame.push_back(mk[2]); frame.push_back(mk[3]);
    for (size_t i = 0; i < plen; ++i)
        frame.push_back(ping[i] ^ mk[i % 4]);
    return frame;
}

} // namespace detail

// ============================================================================
// HWCLOCK — TSC tabanlı donanım saati (CLAUDE.md Kural 5)
//
// clock_gettime() / std::chrono::*::now() HER ÇAĞRIDA bir syscall'a (veya
// VDSO üzerinden syscall'a eşdeğer bir geçişe) iner. tsc_ns() bunun yerine
// RDTSCP makine komutunu kullanır (syscall değildir, ~20-40 döngü).
//
// MÜHÜRLEME (serialization): __rdtsc() tek başına sırasız yürütme (OoO) ile
// komut akışı içinde ÖNCESİ/SONRASI ile yer değiştirebilir -- bu da kaymış,
// güvenilmez zaman damgalarına yol açar. Bunun önüne geçmek için:
//   __rdtscp(&aux) : sayacı okumadan ONCE kendisinden önceki tüm komutların
//                    geri yazılmasını (retire) bekler (kısmi seri hale getirme).
//   _mm_lfence()   : okumadan SONRA, takip eden komutların erken (speculative)
//                    baslamasini engeller.
// Bu ikili olmadan ölçüm, OoO çekirdekte sahte/kaymış olabilir.
//
// Kalibrasyon (ns/tick oranı ve epoch ofseti) yalnızca BİR KEZ, programın
// ilk tsc_ns() çağrısında (static local, thread-safe lazy init) yapılır.
// Bu, hot-path'te tekrarlanan bir "zaman ölçümü" değil, tek seferlik bir
// kurulum işlemidir — Kural 5'in hedefi olan per-event clock_gettime()
// çağrılarını ortadan kaldırmaktır.
// ============================================================================
namespace detail {

// Muhurlu (serialized) TSC okumasi -- tum tsc_ns() cagrilari buradan gecer.
[[gnu::always_inline]] inline uint64_t rdtscp_sealed() noexcept {
    unsigned int aux = 0;
    const uint64_t t = __rdtscp(&aux); // onceki komutlari serilestirir
    _mm_lfence();                      // sonraki komutlarin erken baslamasini engeller
    return t;
}

struct TscCalibration {
    double   ns_per_tick;
    uint64_t t0_tick;
    uint64_t epoch_ns_at_t0;

    TscCalibration() noexcept {
        using namespace std::chrono;
        t0_tick        = rdtscp_sealed();
        epoch_ns_at_t0 = static_cast<uint64_t>(
            duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
        const auto w0 = steady_clock::now();
        std::this_thread::sleep_for(milliseconds(20));
        const uint64_t t1 = rdtscp_sealed();
        const auto     w1 = steady_clock::now();
        const double elapsed_ns    = static_cast<double>(duration_cast<nanoseconds>(w1 - w0).count());
        const double elapsed_ticks = static_cast<double>(t1 - t0_tick);
        ns_per_tick = (elapsed_ticks > 0.0) ? (elapsed_ns / elapsed_ticks) : 0.30; // fallback ~3.3GHz
    }
};

inline const TscCalibration& tsc_calib() noexcept {
    static const TscCalibration c; // lazy, bir kez
    return c;
}

} // namespace detail

[[gnu::always_inline]] inline uint64_t tsc_ns() noexcept {
    const auto&    c        = detail::tsc_calib();
    const uint64_t now_tick = detail::rdtscp_sealed();
    const double   delta_ns = static_cast<double>(now_tick - c.t0_tick) * c.ns_per_tick;
    return c.epoch_ns_at_t0 + static_cast<uint64_t>(delta_ns);
}

// ============================================================================
// MirrorRing — "ayna haritalı" (magic/mirrored) dairesel tampon
//
// Amac: head/tail imlecleri serbestce ilerlerken, herhangi bir [head, head+N)
// araligi -- N <= Capacity ve aralik fiziksel dizinin sonunu (wrap noktasini)
// asa bile -- HER ZAMAN bitisik ve gecerli bellek olarak okunabilsin.
//
// Yontem: Capacity boyutunda TEK bir paylasimli bellek sayfasi (memfd), adres
// uzayina ARKA ARKAYA İKİ KEZ mmap edilir (ayni fiziksel sayfalar). Sonuc:
// [0, Capacity) ile [Capacity, 2*Capacity) bayt-bayt ozdestir. Bu sayede
// "tasma" (wrap) anında veriyi sola kaydirmaya (memmove) ASLA gerek kalmaz --
// & MASK ile hesaplanan ofsetten itibaren Capacity bayta kadar her zaman
// gecerli, tek parca bir gorunum elde edilir.
//
// Kurulum (memfd_create/mmap) BIR KEZ, nesne insasinda yapılır -- hot-path'te
// hicbir syscall tetiklenmez; bu KURAL 2/3'un hedefledigi per-event maliyeti
// degildir.
// ============================================================================
class MirrorRing {
public:
    explicit MirrorRing(size_t capacity) noexcept : cap_(capacity) {
        int fd = ::memfd_create("kusak_rbuf", 0);
        if (__builtin_expect(fd < 0, 0)) { fail("memfd_create"); return; }
        if (__builtin_expect(::ftruncate(fd, static_cast<off_t>(cap_)) != 0, 0)) {
            fail("ftruncate"); ::close(fd); return;
        }

        void* region = ::mmap(nullptr, cap_ * 2, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (__builtin_expect(region == MAP_FAILED, 0)) { fail("mmap reserve"); ::close(fd); return; }

        void* lo = ::mmap(region, cap_, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, 0);
        void* hi = ::mmap(static_cast<uint8_t*>(region) + cap_, cap_,
                           PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        ::close(fd);

        if (__builtin_expect(lo == MAP_FAILED || hi == MAP_FAILED, 0)) {
            fail("mmap mirror"); ::munmap(region, cap_ * 2); return;
        }
        base_ = reinterpret_cast<uint8_t*>(region);
    }

    ~MirrorRing() noexcept { if (base_) ::munmap(base_, cap_ * 2); }

    MirrorRing(const MirrorRing&)            = delete;
    MirrorRing& operator=(const MirrorRing&) = delete;

    [[nodiscard]] uint8_t* data()  const noexcept { return base_; }
    [[nodiscard]] bool     ok()    const noexcept { return base_ != nullptr; }

private:
    static void fail(const char* who) noexcept {
        std::cerr << "[MirrorRing] " << who << " basarisiz: " << std::strerror(errno) << "\n";
    }
    size_t   cap_;
    uint8_t* base_ = nullptr;
};

// ============================================================================
// TLSSocket — RAII TCP+TLS
// ============================================================================
class TLSSocket {
public:
    TLSSocket() noexcept {
        // [ARCH-FIX] ctx_ nesne omru boyunca BIR KEZ olusturulur; CA sertifika/disk
        // okuma maliyeti reconnect basina degil, sadece burada odenir.
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return; // connect() ctx_==nullptr durumunu kontrol edip hata dondurur

        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);

        if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
            std::cerr << "[TLSSocket] UYARI: Sistem CA sertifikalari yuklenemedi"
                      << " -- SSL_CTX_set_default_verify_paths() basarisiz.\n"
                      << "  Cozum: 'sudo apt install ca-certificates' veya"
                      << " SSL_CERT_FILE/SSL_CERT_DIR ortam degiskenlerini ayarlayin.\n"
                      << "  TLS dogrulamasi SSL_VERIFY_PEER ile devam ediyor;"
                      << " baglanti CA hatasi ile reddedilebilir.\n";
        }

        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    }
    ~TLSSocket() {
        close();
        // [ARCH-FIX] ctx_'in serbest birakildigi TEK yer -- nesne omrunun sonu.
        // close() artik ctx_'e dokunmuyor (reconnect'ler arasi hayatta kalmasi icin).
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    }
    TLSSocket(const TLSSocket&)            = delete;
    TLSSocket& operator=(const TLSSocket&) = delete;

    bool connect(const std::string& host, uint16_t port,
                 std::string& err_out) noexcept {
        close();

        // [ARCH-FIX] ctx_ constructor'da kuruldu; burada sadece gecerliligi denetlenir,
        // yeniden olusturulmaz.
        if (!ctx_) { err_out = "TLSSocket: SSL_CTX gecersiz (constructor basarisiz)"; return false; }

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        std::string ps = std::to_string(port);
        if (::getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0) {
            err_out = "DNS cozumlenemedi: " + host; return false;
        }
        fd_ = -1;
        for (auto* p = res; p; p = p->ai_next) {
            fd_ = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd_ < 0) continue;
            struct timeval tv{10, 0};
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            // [FIX-CONN-1] SO_KEEPALIVE: soket O_NONBLOCK'a alindiktan sonra
            // SO_RCVTIMEO/SO_SNDTIMEO artik read()/write() icin islevsizdir --
            // sadece blocking soketlerde etkilidirler. Sessizce olen (FIN/RST
            // gelmeyen) bir peer'i yakalamanin tek OS-seviyesi yolu budur.
            // idle=10s sonra prob baslar, 5s arayla 3 prob (~25s'de ECONNRESET/ETIMEDOUT).
            {
                const int on = 1;
                ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#if defined(TCP_KEEPIDLE)
                const int idle = 10;
                ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#if defined(TCP_KEEPINTVL)
                const int intvl = 5;
                ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#if defined(TCP_KEEPCNT)
                const int cnt = 3;
                ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
            }
            // [FIX-BLOCK-1] connect() suresiz bloklanabiliyordu (paket sessizce
            // dusunce OS varsayilan TCP timeout'u 20-130s olabiliyor ve bu, tek
            // thread'li strateji donguyu tamamen kilitliyordu). Soketi connect()
            // ONCESINDE non-blocking yapip, sonucu select() ile 5s'de sinirliyoruz.
            int fl = ::fcntl(fd_, F_GETFL, 0);
            ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);

            int cr = ::connect(fd_, p->ai_addr, p->ai_addrlen);
            if (cr == 0) {
                ::fcntl(fd_, F_SETFL, fl);  // basariyla baglandi, blocking'e geri don
                break;
            }
            if (errno == EINPROGRESS) {
                fd_set wfds; FD_ZERO(&wfds); FD_SET(fd_, &wfds);
                struct timeval ctv{5, 0};  // 5 saniyelik baglanti zaman asimi
                int sr = ::select(fd_ + 1, nullptr, &wfds, nullptr, &ctv);
                int soerr = 0; socklen_t sl = sizeof(soerr);
                ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (sr > 0 && soerr == 0) {
                    ::fcntl(fd_, F_SETFL, fl);  // blocking'e geri don
                    break;
                }
            }
            ::close(fd_); fd_ = -1;
        }
        ::freeaddrinfo(res);
        if (fd_ < 0) { err_out = "TCP baglanti basarisiz: " + host + ":" + ps; return false; }

        // [ARCH-FIX] SSL_CTX_new / set_min_proto_version / set_default_verify_paths /
        // set_verify BURADAN KALDIRILDI -- artik constructor'da bir kez yapiliyor.
        ssl_ = SSL_new(ctx_);
        if (!ssl_) { err_out = "SSL_new: " + detail::ssl_error_string();
                     ::close(fd_); fd_ = -1; return false; }

        SSL_set_tlsext_host_name(ssl_, host.c_str());
        SSL_set_hostflags(ssl_, 0);
        SSL_set1_host(ssl_, host.c_str());

        // [ARCH-FIX] ALPN: http/1.1 zorunlu kilinir (wire format: 1-byte uzunluk + etiket).
        // WS upgrade zaten yalniz http/1.1 uzerinden calisir; ALPN'siz ClientHello WAF/CDN
        // katmaninda bot-fingerprint olarak isaretlenip TLS seviyesinde reddedilebiliyor.
        static const unsigned char ALPN_PROTOS[] = { 8,'h','t','t','p','/','1','.','1' };
        // NOT: SSL_set_alpn_protos() digerlerinin tersine BASARIDA 0, HATADA non-zero doner.
        if (SSL_set_alpn_protos(ssl_, ALPN_PROTOS, sizeof(ALPN_PROTOS)) != 0) {
            err_out = "SSL_set_alpn_protos basarisiz: " + detail::ssl_error_string();
            close(); return false;
        }

        SSL_set_fd(ssl_, fd_);

        // [FIX-BLOCK-2] SSL_connect de blocking soket uzerinde suresiz bekleyebilir.
        // Soketi tekrar non-blocking yapip WANT_READ/WANT_WRITE donduğunde poll()
        // ile 5s toplam zaman asimi uyguluyoruz.
        {
            int fl2 = ::fcntl(fd_, F_GETFL, 0);
            ::fcntl(fd_, F_SETFL, fl2 | O_NONBLOCK);

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                int rc2 = SSL_connect(ssl_);
                if (rc2 == 1) break;
                int e2 = SSL_get_error(ssl_, rc2);
                if (e2 != SSL_ERROR_WANT_READ && e2 != SSL_ERROR_WANT_WRITE) break;

                auto remain = deadline - std::chrono::steady_clock::now();
                if (remain <= std::chrono::seconds(0)) break;  // timeout

                struct pollfd pfd{};
                pfd.fd     = fd_;
                pfd.events = (e2 == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;
                int pr = ::poll(&pfd, 1,
                    static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(remain).count()));
                if (pr <= 0) break;  // timeout ya da hata
            }
            ::fcntl(fd_, F_SETFL, fl2);  // sonuc ne olursa olsun blocking'e geri don
        }

        int rc  = SSL_connect(ssl_);
        int eno = errno; // [ARCH-FIX] errno, SSL_connect'ten HEMEN sonraki satirda, SSL_get_error
                          // DAHIL hicbir ara cagriya girmeden yakalanir (errno-clobber savunmasi).
        if (rc != 1) {
            // [ARCH-FIX] Kor ssl_error_string() cagrisi kaldirildi; SSL_get_error() ile
            // SSL_ERROR_SYSCALL / SSL_ERROR_SSL / SSL_ERROR_ZERO_RETURN ayrimi yapiliyor.
            int e = SSL_get_error(ssl_, rc);
            switch (e) {
                case SSL_ERROR_SYSCALL:
                    err_out = "TLS el sikismasi basarisiz (SSL_ERROR_SYSCALL, errno=" +
                              std::to_string(eno) + " \"" + std::strerror(eno) + "\"): " +
                              detail::ssl_error_string();
                    break;
                case SSL_ERROR_SSL:
                    err_out = "TLS el sikismasi basarisiz (SSL_ERROR_SSL): " +
                              detail::ssl_error_string();
                    break;
                case SSL_ERROR_ZERO_RETURN:
                    err_out = "TLS el sikismasi basarisiz (SSL_ERROR_ZERO_RETURN): "
                              "karsi taraf TLS close_notify ile baglantiyi kapatti";
                    break;
                default:
                    err_out = "TLS el sikismasi basarisiz (SSL_get_error=" +
                              std::to_string(e) + "): " + detail::ssl_error_string();
                    break;
            }
            close(); return false;
        }
        int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    int write(const void* buf, int len) noexcept {
        return ssl_ ? SSL_write(ssl_, buf, len) : -1;
    }
    int read(void* buf, int len) noexcept {
        if (!ssl_) return -2;
        int n = SSL_read(ssl_, buf, len);
        if (n > 0) return n;
        int e = SSL_get_error(ssl_, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return -1;
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        return -2;
    }
    bool poll_readable(int ms) noexcept {
        if (fd_ < 0) return false;
        struct pollfd pfd{fd_, POLLIN, 0};
        return ::poll(&pfd, 1, ms) > 0;
    }
    bool is_open() const noexcept { return fd_ >= 0 && ssl_; }
    void close() noexcept {
        // [ARCH-FIX] ctx_ BURADAN KALDIRILDI -- artik sinif omru boyunca yasiyor,
        // sadece ~TLSSocket() icinde serbest birakiliyor. Aksi halde ilk close()'dan
        // sonra ctx_ dangling kalir ve bir sonraki connect() SSL_new(nullptr) ile
        // kalici olarak basarisiz olurdu -- reconnect dongusunu tamamen kirar.
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_ = -1; SSL_CTX* ctx_ = nullptr; SSL* ssl_ = nullptr;
};

// ============================================================================
// JSON parser — minimal, Binance format, exception-safe
// Korunuyor: parse_depth() ve UserDataWsClient bu yardimcilara bagimli.
// ============================================================================
namespace json {

inline std::string_view find_str(std::string_view s, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\":\"";
    auto pos = s.find(search);
    if (pos == std::string_view::npos) return {};
    pos += search.size();
    auto end = s.find('"', pos);
    return (end != std::string_view::npos) ? s.substr(pos, end - pos) : std::string_view{};
}

inline bool find_num(std::string_view s, std::string_view key, double& out) {
    std::string search = "\"" + std::string(key) + "\":";
    auto pos = s.find(search);
    if (pos == std::string_view::npos) return false;
    pos += search.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '"')) ++pos;
    if (pos >= s.size()) return false;
    try {
        size_t n = 0;
        out = std::stod(std::string(s.substr(pos, 48)), &n);
        return n > 0;
    } catch (...) { return false; }
}

inline bool find_uint(std::string_view s, std::string_view key, uint64_t& out) {
    std::string search = "\"" + std::string(key) + "\":";
    auto pos = s.find(search);
    if (pos == std::string_view::npos) return false;
    pos += search.size();
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '"')) ++pos;
    if (pos >= s.size()) return false;
    try {
        size_t n = 0;
        out = std::stoull(std::string(s.substr(pos, 24)), &n);
        return n > 0;
    } catch (...) { return false; }
}

// IYI-1: parse_book_levels catch blogu hatаyi stderr'e loglуyor.
inline int parse_book_levels(std::string_view arr,
                              double* prices, double* qtys, int max_n) {
    int n = 0; size_t pos = 0;
    while (n < max_n && pos < arr.size()) {
        auto lb = arr.find('[', pos); if (lb == std::string_view::npos) break; lb++;
        auto q1 = arr.find('"', lb);       if (q1 == std::string_view::npos) break;
        auto q2 = arr.find('"', q1 + 1);  if (q2 == std::string_view::npos) break;
        auto q3 = arr.find('"', q2 + 1);  if (q3 == std::string_view::npos) break;
        auto q4 = arr.find('"', q3 + 1);  if (q4 == std::string_view::npos) break;
        try {
            prices[n] = std::stod(std::string(arr.substr(q1+1, q2-q1-1)));
            qtys[n]   = std::stod(std::string(arr.substr(q3+1, q4-q3-1)));
            n++;
        } catch (const std::exception& e) {
            std::cerr << "[ws_client] JSON parse hatasi (book level " << n << "): "
                      << e.what()
                      << " | price_str=\""
                      << std::string(arr.substr(q1+1, q2-q1-1)).substr(0, 32)
                      << "\" qty_str=\""
                      << std::string(arr.substr(q3+1, q4-q3-1)).substr(0, 32)
                      << "\"\n";
        } catch (...) {
            std::cerr << "[ws_client] JSON parse hatasi (book level " << n
                      << "): bilinmeyen istisna\n";
        }
        pos = q4 + 1;
    }
    return n;
}

} // namespace json

// ============================================================================
// DFA PARSER ENGINE — KUSAK V13
//
// Kural seti (şartname §2, §4, §5, §7):
//   • Sıfır heap tahsisi: new/malloc/std::string/std::vector yasak.
//   • Sıfır kayan nokta: double/float yasak.
//   • Sıfır string karşılaştırması: strcmp/std::string::find yasak.
//   • Sayı döngüsü içinde tek dallanma: yalnızca döngü çıkışı ('"' / non-digit).
//   • Tüm fiyat/miktar: int64_t × 1e8 fixed-point.
//   • Timestamp: uint64_t, doğrudan ms.
// ============================================================================
namespace dfa {

// ---------------------------------------------------------------------------
// Sabitler (§4.1, §4.2)
// ---------------------------------------------------------------------------
static constexpr int64_t PRICE_SCALE = 100'000'000LL; // 1e8

// SCALE_TABLE[k] = 10^(8-k),  k = fraksiyon hane sayısı (0..8)
// SCALE_TABLE[0] kullanılmaz (tamsayı girdi: frac_cnt==0, frac_part==0),
// ama dizinin bütünlüğü için 1e8 olarak bırakılmıştır.
static constexpr int64_t SCALE_TABLE[9] = {
    100'000'000LL,   // 0 ondalık hane — frac_part==0 olduğundan sonuç değişmez
     10'000'000LL,   // 1
      1'000'000LL,   // 2
        100'000LL,   // 3
         10'000LL,   // 4
          1'000LL,   // 5
            100LL,   // 6
             10LL,   // 7
              1LL    // 8
};

// ---------------------------------------------------------------------------
// parse_fixed_point  (§4.3)
//
// Giriş : *p == '"'  (açılış tırnağı)
// Çıkış : p, kapanış tırnağından sonraki konumu gösterir
// Dönüş : değer × PRICE_SCALE  (int64_t)
//
// Döngü içinde dal yoktur; tek dallanma döngü çıkışı içindir.
// Dot karakteri hem int_part hem frac_part için maskelenerek pas geçilir.
// ---------------------------------------------------------------------------
[[nodiscard]] inline int64_t parse_fixed_point(const char*& p) noexcept {
    ++p; // açılış tırnağını atla

    int64_t int_part  = 0;
    int64_t frac_part = 0;
    int     frac_mode = 0; // 0 = tam kısım, 1 = ondalık kısım
    int     frac_cnt  = 0; // ondalık hane sayısı

    while (true) {
        const char c = *p;
        if (__builtin_expect(c == '"', 0)) { ++p; break; } // tek dal: döngü çıkışı

        // '.' → is_dot=1, '0'..'9' → is_dot=0
        const int is_dot  = (c == '.') ? 1 : 0;
        const int64_t digit = static_cast<int64_t>(c - '0'); // geçerli aralık: 0..9

        // Ondalık moda geç (bir kez 1'e setlenir, geri dönmez)
        frac_mode |= is_dot;

        // ---- int_part branchless güncelleme --------------------------------
        // frac_mode==0 → mask_int = 0xFFFF… → new_int seçilir
        // frac_mode==1 → mask_int = 0x0000… → int_part korunur
        // Dot görüldüğünde frac_mode zaten 1'e döndü; int_part korunur.
        const int64_t mask_int  = ~(-static_cast<int64_t>(frac_mode)); // ~0 or ~(-1)
        const int64_t new_int   = (int_part  * 10LL) + digit;
        int_part  ^= (int_part  ^ new_int)  & mask_int;

        // ---- frac_part branchless güncelleme --------------------------------
        // Guncelleme kosulu: frac_mode==1 VE is_dot==0 VE frac_cnt<8 (gercek,
        // tasma yapmayan ondalik hane). 8. haneden sonrasi branchless yutulur.
        // [ARCH-FIX] can_absorb eklendi: 8 haneden fazla ondalik gelirse (or.
        // "1.123456789") frac_part 9. haneyi de yutup SCALE_TABLE[8] ile
        // carpiminda sessizce yanlis buyuklukte bir deger uretiyordu (Sessiz
        // Veri Bozulmasi) -- frac_cnt donguden SONRA 8'e sabitlense bile
        // frac_part zaten bozulmus oluyordu.
        const int     can_absorb    = (frac_cnt < 8 ? 1 : 0);
        const int     is_frac_digit = frac_mode & (is_dot ^ 1) & can_absorb;
        const int64_t mask_frac     = -static_cast<int64_t>(is_frac_digit); // 0 or 0xFFFF…
        const int64_t new_frac      = (frac_part * 10LL) + digit;
        frac_part ^= (frac_part ^ new_frac) & mask_frac;

        // ---- frac_cnt branchless artırma ------------------------------------
        frac_cnt += is_frac_digit; // yalnızca gerçek, 8 hane sinirini asmayan hanede artar

        ++p;
    }

    // frac_cnt'yi SCALE_TABLE indeks aralığına [0,8] sınırla
    frac_cnt = (frac_cnt < 9) ? frac_cnt : 8;

    return int_part * PRICE_SCALE + frac_part * SCALE_TABLE[frac_cnt];
}

// ---------------------------------------------------------------------------
// parse_uint64  (§4.4)
//
// Giriş : *p ilk rakam karakteri (tırnak yok)
// Çıkış : p, ilk rakam-dışı karakteri gösterir
// Dönüş : uint64_t değeri
//
// Döngü içinde dal yoktur; tek dallanma döngü çıkışı (digit test).
// ---------------------------------------------------------------------------
[[nodiscard]] inline uint64_t parse_uint64(const char*& p) noexcept {
    uint64_t val = 0;
    while (true) {
        const unsigned d = static_cast<unsigned>(*p - '0');
        if (__builtin_expect(d > 9u, 0)) break; // tek dal: döngü çıkışı
        val = val * 10ULL + static_cast<uint64_t>(d);
        ++p;
    }
    return val;
}

// ---------------------------------------------------------------------------
// skip_ws — boşluk atlama (stack, sıfır kopya)
// ---------------------------------------------------------------------------
inline void skip_ws(const char*& p, const char* end) noexcept {
    while (__builtin_expect(p < end, 1)) {
        const char c = *p;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++p;
    }
}

// ---------------------------------------------------------------------------
// skip_value — istenmeyen JSON değerini atla  (§3.3 SKIP_VALUE)
//
// Desteklenen: string, number, bool/null, iç içe object/array.
// Escape karakterleri string içinde doğru işlenir.
// ---------------------------------------------------------------------------
inline void skip_value(const char*& p, const char* end) noexcept {
    if (__builtin_expect(p >= end, 0)) return;

    if (*p == '"') {
        // Tırnaklı string
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') ++p; // escape: bir sonraki karakteri atla
            if (p < end) ++p;
        }
        if (p < end) ++p; // kapanış '"'
        return;
    }

    if (*p == '{' || *p == '[') {
        // İç içe yapı: derinlik sayacı
        const char open  = *p;
        const char close = (open == '{') ? '}' : ']';
        int depth = 1;
        ++p;
        while (p < end && depth > 0) {
            if (*p == '"') {
                // String içindeki { } [ ] karakterlerini atla
                ++p;
                while (p < end && *p != '"') {
                    if (*p == '\\') ++p;
                    if (p < end) ++p;
                }
                if (p < end) ++p;
            } else {
                depth += (*p == open);
                depth -= (*p == close);
                ++p;
            }
        }
        return;
    }

    // Tırnak yok, obje/dizi değil → sayı/bool/null: ayırıcıya kadar atla
    while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
}

// ---------------------------------------------------------------------------
// parse_depth_levels  (derinlik seviyeleri — bid/ask dizisi, EMIR-3 revizyonu)
//
// Format : [["fiyat_str","miktar_str"], ["fiyat_str","miktar_str"], ...]
// Giriş  : *p == '['  (dış dizinin açılış parantezi)
// Çıkış  : p, dış dizinin kapanış ']' karakterinden sonrasını gösterir
// Dönüş  : doldurulan seviye sayısı (max_n ile sınırlı)
//
// Heap yok, exception yok, double yok — her seviye parse_fixed_point ile
// int64_t ×1e8 fixed-point'e çevrilir (aggTrade/bookTicker ile aynı kod yolu).
// ---------------------------------------------------------------------------
[[nodiscard]] inline int parse_depth_levels(const char*& p, const char* end,
                                             int64_t* prices, int64_t* qtys,
                                             int max_n) noexcept {
    int n = 0;
    if (__builtin_expect(p >= end || *p != '[', 0)) return 0;
    ++p; // dış '['

    while (p < end) {
        skip_ws(p, end);
        if (__builtin_expect(p >= end, 0)) break;

        if (*p == ']') { ++p; break; }       // dış dizi bitti
        if (*p == ',') { ++p; continue; }    // seviyeler arası virgül

        if (__builtin_expect(*p != '[', 0)) { skip_value(p, end); continue; }
        ++p; // iç '[' (["fiyat","miktar"])

        skip_ws(p, end);
        const int64_t price = (p < end && *p == '"') ? parse_fixed_point(p) : 0;

        skip_ws(p, end);
        if (p < end && *p == ',') ++p;
        skip_ws(p, end);
        const int64_t qty = (p < end && *p == '"') ? parse_fixed_point(p) : 0;

        // iç ']' ye kadar ilerle (şartname dışı ekstra alan olsa da güvenli)
        while (p < end && *p != ']') ++p;
        if (p < end) ++p; // iç ']'

        if (n < max_n) { prices[n] = price; qtys[n] = qty; ++n; }
    }
    return n;
}

} // namespace dfa

// ============================================================================
// Callback veri yapıları
// ============================================================================
// V13: DepthUpdate — double → int64_t (×1e8 fixed-point, EMIR-3 revizyonu)
struct DepthUpdate {
    uint64_t ts_exchange;
    int64_t  bid_prices[20]; int64_t bid_qtys[20]; int bid_n;
    int64_t  ask_prices[20]; int64_t ask_qtys[20]; int ask_n;
};

// V13: BookTicker — double → int64_t (×1e8 fixed-point, §6)
struct BookTicker {
    uint64_t ts_exchange;    // T alanı × 1000 → mikrosaniye
    int64_t  best_bid;       // "b" × 1e8
    int64_t  best_bid_qty;   // "B" × 1e8
    int64_t  best_ask;       // "a" × 1e8
    int64_t  best_ask_qty;   // "A" × 1e8
};

// V13: AggTrade — double → int64_t (×1e8 fixed-point, §6)
struct AggTrade {
    uint64_t ts_exchange;    // T alanı × 1000 → mikrosaniye
    int64_t  price;          // "p" × 1e8
    int64_t  qty;            // "q" × 1e8
    bool     buyer_is_maker;
};

// v11: User Data Stream Callback veri yapisi (değişmedi)
struct ExecutionReport {
    std::string event_type;
    std::string symbol;
    std::string client_oid;
    std::string exec_type;
    std::string order_status;
    std::string reject_reason;
    double      price         = 0.0;
    double      qty           = 0.0;
    double      exec_qty      = 0.0;
    uint64_t    order_id      = 0;
    uint64_t    ts_ms         = 0;

    bool isRejected()  const noexcept { return order_status == "REJECTED"; }
    bool isFilled()    const noexcept { return order_status == "FILLED";   }
    bool isCanceled()  const noexcept { return order_status == "CANCELED"; }
    bool isNew()       const noexcept { return order_status == "NEW";      }
};

using DepthCb      = std::function<void(const DepthUpdate&)>;
using TickerCb     = std::function<void(const BookTicker&)>;
using TradeCb      = std::function<void(const AggTrade&)>;
using ExecutionCb  = std::function<void(const ExecutionReport&)>;

// ============================================================================
// OrderErrorHandler — REST -1013 MIN_NOTIONAL try-catch arayuzu (v11)
// ============================================================================
struct OrderErrorHandler {
    static bool is_min_notional_error(std::string_view error_body) noexcept {
        return error_body.find("-1013") != std::string_view::npos ||
               error_body.find("MIN_NOTIONAL") != std::string_view::npos ||
               error_body.find("Filter failure") != std::string_view::npos;
    }

    static void on_rest_error(std::string_view error_body) noexcept {
        if (is_min_notional_error(error_body)) {
            std::cerr << "[OrderErrorHandler] Emir reddedildi: MIN_NOTIONAL "
                         "(Notional deger cok kucuk). Hata: " << error_body << "\n";
        } else {
            std::cerr << "[OrderErrorHandler] REST emir hatasi: " << error_body << "\n";
        }
    }

    template<typename BinanceApiFunc>
    static bool safe_send(BinanceApiFunc&& apiCall,
                          std::string& error_out) noexcept {
        try {
            apiCall();
            return true;
        } catch (const std::exception& e) {
            error_out = e.what();
            on_rest_error(error_out);
            return false;
        } catch (...) {
            error_out = "Bilinmeyen REST hatasi";
            std::cerr << "[OrderErrorHandler] " << error_out << "\n";
            return false;
        }
    }
};

// ============================================================================
// UserDataWsClient — Binance Spot User Data Stream (v11 — değişmedi)
// ============================================================================
class UserDataWsClient {
public:
    static constexpr const char* WS_HOST = "stream.binance.com";
    static constexpr uint16_t    WS_PORT = 9443;

    struct Config {
        std::string listen_key;
        int reconnect_delay_ms = 2000;
        int reconnect_max_ms   = 60000;  // GÖREV 2: 30sn -> 60sn (rapor talebi)
        bool verbose           = false;
        bool log_rejected      = true;
        bool log_all_events    = false;
    };

    UserDataWsClient(Config cfg, ExecutionCb on_execution)
        : cfg_(std::move(cfg))
        , on_execution_(std::move(on_execution))
    {
        ws_path_ = "/ws/" + cfg_.listen_key;
    }

    void run(const std::atomic<bool>& g_stop) {
        int delay_ms = cfg_.reconnect_delay_ms;
        int attempt  = 0;
        while (!g_stop.load(std::memory_order_acquire)) {
            if (attempt > 0) {
                std::cerr << "[UserDataWs] Yeniden baglaniliyor (" << attempt
                          << ") -- " << delay_ms << "ms...\n";
                auto dl = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(delay_ms);
                while (std::chrono::steady_clock::now() < dl) {
                    if (g_stop.load(std::memory_order_acquire)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                delay_ms = std::min(delay_ms * 2, cfg_.reconnect_max_ms);
            }
            attempt++;
            std::string err;
            if (!sock_.connect(WS_HOST, WS_PORT, err)) {
                std::cerr << "[UserDataWs] TCP/TLS hatasi: " << err << "\n"; continue;
            }
            if (!do_ws_handshake(err)) {
                std::cerr << "[UserDataWs] Handshake hatasi: " << err << "\n";
                sock_.close(); continue;
            }
            std::cout << "[UserDataWs] Baglandi: User Data Stream aktif\n";
            delay_ms = cfg_.reconnect_delay_ms;

            bool clean = read_loop(g_stop);
            sock_.close();
            if (clean || g_stop.load(std::memory_order_acquire)) return;
            std::cerr << "[UserDataWs] Baglanti kesildi, yeniden deneniyor...\n";
        }
    }

private:
    bool do_ws_handshake(std::string& err) {
        std::string key = detail::ws_random_key();
        std::string req =
            "GET " + ws_path_ + " HTTP/1.1\r\n"
            "Host: " + std::string(WS_HOST) + "\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: KUSAK-V13/1.0\r\n\r\n";
        if (sock_.write(req.data(), static_cast<int>(req.size())) <= 0) {
            err = "HTTP request yazilamadi"; return false;
        }
        std::string resp; resp.reserve(512);
        char byte;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        
        // --- YENİ ZIRHLI DÖNGÜ BAŞLANGICI ---
        while (std::chrono::steady_clock::now() < dl) {
            int n = sock_.read(&byte, 1);

            if (n > 0) {
                resp += byte;
                if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0)
                    break;
                continue;
            }

            if (n == -1) {
                sock_.poll_readable(10);
                continue;
            }

            // GÖREV 1: Kor "okuma hatasi" yerine o ana kadar toplanan ham
            // yaniti (varsa) terminale yazdir.
            err = "Handshake okuma hatasi (soket/TLS, n=" + std::to_string(n) + ")";
            detail::log_handshake_rejection("UserDataWs", WS_HOST, WS_PORT, ws_path_, resp);
            return false;
        }
        // --- YENİ ZIRHLI DÖNGÜ BİTİŞİ ---

        // SENİN ORİJİNAL KONTROLLERİN (DOKUNULMADI)
        if (resp.find("101") == std::string::npos) {
            // GÖREV 1: Content-Length varsa body de gelmis olabilir (WAF/rate-limit
            // sebebi genelde oradadir) -- kisa sureli sinirli ek okuma ile yakala.
            auto body_dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (resp.size() < 2048 && std::chrono::steady_clock::now() < body_dl) {
                char b; int n2 = sock_.read(&b, 1);
                if (n2 > 0) { resp += b; continue; }
                if (n2 == -1) { sock_.poll_readable(20); continue; }
                break;
            }
            err = "101 beklendi (HTTP durum kodu / body icin terminale bak)";
            detail::log_handshake_rejection("UserDataWs", WS_HOST, WS_PORT, ws_path_, resp);
            return false;
        }
        return true;
    }

    bool read_loop(const std::atomic<bool>& g_stop) {
        std::vector<uint8_t> rbuf; rbuf.reserve(8192);
        std::vector<uint8_t> raw(4096);
        while (!g_stop.load(std::memory_order_acquire)) {
            if (!sock_.poll_readable(200)) continue;
            while (true) {
                int n = sock_.read(raw.data(), static_cast<int>(raw.size()));
                if (n == 0)  { std::cerr << "[UserDataWs] Sunucu kapatti\n"; return false; }
                if (n == -1) break;
                if (n == -2) { std::cerr << "[UserDataWs] TLS okuma hatasi\n"; return false; }
                rbuf.insert(rbuf.end(), raw.data(), raw.data() + n);
            }
            while (true) {
                int r = try_parse_frame(rbuf);
                if (r < 0) return false;
                if (r == 0) break;
            }
        }
        return true;
    }

    int try_parse_frame(std::vector<uint8_t>& buf) {
        if (buf.size() < 2) return 0;
        bool    fin    = (buf[0] & 0x80) != 0;
        uint8_t opcode = buf[0] & 0x0F;
        bool    masked = (buf[1] & 0x80) != 0;
        size_t  plen   = buf[1] & 0x7F;
        size_t  hsize  = 2;
        if (plen == 126) {
            if (buf.size() < 4) return 0;
            plen = (size_t(buf[2]) << 8) | buf[3]; hsize = 4;
        } else if (plen == 127) {
            if (buf.size() < 10) return 0;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | buf[2+i];
            hsize = 10;
        }
        if (masked) hsize += 4;
        if (buf.size() < hsize + plen) return 0;

        std::vector<uint8_t> payload(buf.begin() + hsize, buf.begin() + hsize + plen);
        if (masked) {
            const uint8_t* mk = buf.data() + hsize - 4;
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mk[i % 4];
        }
        buf.erase(buf.begin(), buf.begin() + hsize + plen);

        switch (opcode) {
            case 0x1: case 0x2:
                if (!fin) { frag_buf_.assign(payload.begin(), payload.end()); }
                else      { dispatch_payload(payload); }
                break;
            case 0x0:
                frag_buf_.insert(frag_buf_.end(), payload.begin(), payload.end());
                if (fin) { dispatch_payload(frag_buf_); frag_buf_.clear(); }
                break;
            case 0x8: std::cerr << "[UserDataWs] Close frame\n"; return -1;
            case 0x9: { auto pong = detail::make_pong_frame(payload);
                        sock_.write(pong.data(), static_cast<int>(pong.size())); break; }
            case 0xA: break;
            default:  break;
        }
        return 1;
    }

    void dispatch_payload(const std::vector<uint8_t>& payload) {
        if (payload.empty()) return;
        std::string_view msg(reinterpret_cast<const char*>(payload.data()),
                             payload.size());
        parse_execution_report(msg);
    }

    void parse_execution_report(std::string_view msg) noexcept {
        auto event_type = json::find_str(msg, "e");
        if (event_type != "executionReport") return;

        ExecutionReport er;
        er.event_type   = std::string(event_type);
        er.symbol       = std::string(json::find_str(msg, "s"));
        er.client_oid   = std::string(json::find_str(msg, "c"));
        er.exec_type    = std::string(json::find_str(msg, "x"));
        er.order_status = std::string(json::find_str(msg, "X"));
        er.reject_reason= std::string(json::find_str(msg, "r"));

        json::find_num (msg, "p", er.price);
        json::find_num (msg, "q", er.qty);
        json::find_num (msg, "l", er.exec_qty);
        json::find_uint(msg, "i", er.order_id);
        json::find_uint(msg, "T", er.ts_ms);

        if (er.isRejected()) {
            if (cfg_.log_rejected) {
                std::cerr << "[UserDataWs] Emir reddedildi (EXCHANGE tarafindan)"
                          << "  sembol=" << er.symbol
                          << "  sebep="  << (er.reject_reason.empty()
                                             ? "belirtilmedi" : er.reject_reason)
                          << "  clientOID=" << er.client_oid
                          << "  orderId="   << er.order_id
                          << "\n";
            }
        } else if (cfg_.log_all_events) {
            std::cerr << "[UserDataWs] execReport"
                      << "  status=" << er.order_status
                      << "  exec="   << er.exec_type
                      << "  sym="    << er.symbol << "\n";
        }

        if (on_execution_) {
            try { on_execution_(er); }
            catch (...) {}
        }
    }

    Config          cfg_;
    std::string     ws_path_;
    TLSSocket       sock_;
    ExecutionCb     on_execution_;
    std::vector<uint8_t> frag_buf_;
};

// ============================================================================
// BinanceWsClient — Piyasa Verisi Akisi (V13: DFA parsers)
// ============================================================================
class BinanceWsClient {
public:
    static constexpr const char* WS_HOST = "stream.binance.com";
    static constexpr uint16_t    WS_PORT = 9443;

    struct Config {
        std::string symbol             = "BTCUSDT";
        bool        enable_depth       = true;
        bool        enable_ticker      = true;
        bool        enable_trade       = true;
        int         reconnect_delay_ms = 2000;
        int         reconnect_max_ms   = 60000;  // GÖREV 2: 30sn -> 60sn (rapor talebi)
        bool        verbose            = false;
    };

    BinanceWsClient(Config cfg,
                    DepthCb  on_depth  = nullptr,
                    TickerCb on_ticker = nullptr,
                    TradeCb  on_trade  = nullptr)
        : cfg_(std::move(cfg))
        , on_depth_(std::move(on_depth))
        , on_ticker_(std::move(on_ticker))
        , on_trade_(std::move(on_trade))
    {
        sym_lower_ = cfg_.symbol;
        std::transform(sym_lower_.begin(), sym_lower_.end(),
                       sym_lower_.begin(), ::tolower);
        std::string streams;
        auto add = [&](const std::string& s) {
            if (!streams.empty()) streams += "/";
            streams += s;
        };
        if (cfg_.enable_depth)  add(sym_lower_ + "@depth@100ms");
        if (cfg_.enable_ticker) add(sym_lower_ + "@bookTicker");
        if (cfg_.enable_trade)  add(sym_lower_ + "@aggTrade");
        ws_path_ = "/stream?streams=" + streams;
    }

    void run(const std::atomic<bool>& g_stop) {
        int delay_ms = cfg_.reconnect_delay_ms;
        int attempt  = 0;
        while (!g_stop.load(std::memory_order_acquire)) {
            if (attempt > 0) {
                std::cerr << "[ws] Yeniden baglaniliyor (" << attempt
                          << ") -- " << delay_ms << "ms...\n";
                auto dl = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(delay_ms);
                while (std::chrono::steady_clock::now() < dl) {
                    if (g_stop.load(std::memory_order_acquire)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                delay_ms = std::min(delay_ms * 2, cfg_.reconnect_max_ms);
            }
            attempt++;
            std::string err;
            if (!sock_.connect(WS_HOST, WS_PORT, err)) {
                std::cerr << "[ws] TCP/TLS hatasi: " << err << "\n"; continue;
            }
            if (cfg_.verbose)
                std::cout << "[ws] TLS OK -> " << WS_HOST << ":" << WS_PORT << "\n";
            if (!do_ws_handshake(err)) {
                std::cerr << "[ws] Handshake hatasi: " << err << "\n";
                sock_.close(); continue;
            }
            std::cout << "[ws] Baglandi: " << sym_lower_ << " stream aktif\n";
            delay_ms = cfg_.reconnect_delay_ms;

            bool clean = read_loop(g_stop);
            sock_.close();
            if (clean || g_stop.load(std::memory_order_acquire)) return;
            g_ws_disconnect_total.fetch_add(1, std::memory_order_relaxed);   // [PHASE3-KS]
            std::cerr << "[ws] Baglanti kesildi, yeniden deneniyor...\n";
        }
    }

private:
    bool do_ws_handshake(std::string& err) {
        std::string key = detail::ws_random_key();
        std::string req =
            "GET " + ws_path_ + " HTTP/1.1\r\n"
            "Host: " + std::string(WS_HOST) + "\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: KUSAK-V13/1.0\r\n\r\n";
        if (sock_.write(req.data(), static_cast<int>(req.size())) <= 0) {
            err = "HTTP request yazilamadi"; return false;
        }
        std::string resp; resp.reserve(512);
        char byte;
        auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool got_headers = false;

        // GÖREV 1: EAGAIN (n==-1) artik fatal sayilmiyor (UserDataWs'teki
        // zirhli donguyle ayni desen) -- sadece gercek soket/TLS hatasinda
        // (n==0 kapandi / n==-2 TLS hatasi) donguden cikilip teshis basiliyor.
        while (std::chrono::steady_clock::now() < dl) {
            int n = sock_.read(&byte, 1);
            if (n > 0) {
                resp += byte;
                if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0) {
                    got_headers = true; break;
                }
                continue;
            }
            if (n == -1) { sock_.poll_readable(10); continue; }

            err = "Handshake okuma hatasi (soket/TLS, n=" + std::to_string(n) + ")";
            detail::log_handshake_rejection("ws", WS_HOST, WS_PORT, ws_path_, resp);
            return false;
        }
        if (!got_headers) {
            err = "Handshake zaman asimi (10sn icinde header tamamlanamadi)";
            detail::log_handshake_rejection("ws", WS_HOST, WS_PORT, ws_path_, resp);
            return false;
        }
        if (resp.find("101") == std::string::npos) {
            // GÖREV 1: Header sonrasi body varsa (WAF/rate-limit sebebi genelde
            // burada) kisa sureli sinirli ek okuma ile yakalamayi dene.
            auto body_dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (resp.size() < 2048 && std::chrono::steady_clock::now() < body_dl) {
                char b; int n2 = sock_.read(&b, 1);
                if (n2 > 0) { resp += b; continue; }
                if (n2 == -1) { sock_.poll_readable(20); continue; }
                break;
            }
            err = "HTTP 101 beklenirken farkli durum kodu alindi (detay terminalde)";
            detail::log_handshake_rejection("ws", WS_HOST, WS_PORT, ws_path_, resp);
            return false;
        }
        std::string hk = "Sec-WebSocket-Accept: ";
        auto pos = resp.find(hk);
        if (pos == std::string::npos) { err = "Sec-WebSocket-Accept eksik"; return false; }
        pos += hk.size();
        auto end = resp.find("\r\n", pos);
        if (resp.substr(pos, end - pos) != detail::ws_accept_key(key)) {
            err = "Sec-WebSocket-Accept uyumsuz"; return false;
        }
        return true;
    }

    // KURAL 2: heap yasak. RBUF_CAP 2'nin kuvveti olmali (& MASK icin) ve
    // sayfa-hizali olmali (MirrorRing'in mmap'i icin); 65536 ikisini de saglar.
    static constexpr size_t RBUF_CAP          = 65536; // 64 KiB
    static constexpr size_t RBUF_MASK         = RBUF_CAP - 1;
    static constexpr size_t MAX_FRAME_PAYLOAD = 65536;  // tek frame/parca ust siniri
    static_assert((RBUF_CAP & RBUF_MASK) == 0, "RBUF_CAP 2'nin kuvveti olmali");

    // head_/tail_ : monotonik (hic sifirlanmayan, hic kaydirilmayan) bayt
    // sayaclari. Fiziksel ofset daima (cursor & RBUF_MASK). MirrorRing
    // sayesinde wrap noktasini asan okumalar da bitisik ve gecerlidir --
    // bu yuzden try_parse_frame ASLA memmove/memcpy ile veri kaydirmaz.
    uint64_t head_ = 0, tail_ = 0;

    bool read_loop(const std::atomic<bool>& g_stop) {
        if (__builtin_expect(!rbuf_.ok(), 0)) {
            std::cerr << "[ws] MirrorRing kurulamadi -- mmap/memfd basarisiz\n";
            return false;
        }
        uint8_t* const rb = rbuf_.data();
        head_ = 0; tail_ = 0; // her baglantida sifirdan basla
        alignas(64) uint8_t raw[4096];

        // [FIX-CONN-2] Sessiz-olum (half-open) bekcisi. Binance spot market
        // stream'i ~20s'de bir WS-ping gonderir; saglikli bir baglantida bu
        // yuzden >MAX_SILENCE_NS suresince hicbir byte gelmemesi anormaldir.
        // SO_KEEPALIVE OS seviyesinde ayni seyi ~25s'de yakalar; bu, onun
        // ONUNE gecen, protokolu bilen, daha hizli ve deterministik katmandir.
        // Boylece read_loop() bir daha SONSUZA kadar EAGAIN'de spin etmez.
        static constexpr uint64_t MAX_SILENCE_NS = 45'000'000'000ULL; // 45 s
        uint64_t last_data_ns = tsc_ns();

        while (__builtin_expect(!g_stop.load(std::memory_order_acquire), 1)) {
            const int n = sock_.read(raw, sizeof(raw));

            if (n > 0) {
                last_data_ns = tsc_ns(); // [FIX-CONN-2]
                if (__builtin_expect(tail_ - head_ + static_cast<size_t>(n) > RBUF_CAP, 0)) {
                    std::cerr << "[ws] rbuf tasmasi -- baglanti sifirlaniyor\n";
                    return false;
                }
                // Ayna-haritali tampon: yazim noktasi wrap'i asabilir, sorun degil --
                // ikinci haritalama bu bolgeyi otomatik olarak gecerli kilar.
                std::memcpy(rb + (tail_ & RBUF_MASK), raw, static_cast<size_t>(n));
                tail_ += static_cast<size_t>(n);

                while (true) {
                    const int r = try_parse_frame(rb);
                    if (r < 0) return false;
                    if (r == 0) break;
                }
                continue; // veri geldi: hemen tekrar oku, spin'e girme
            }
            if (n == 0)  { std::cerr << "[ws] Sunucu kapatti\n"; return false; }
            if (n == -2) { std::cerr << "[ws] TLS okuma hatasi\n"; return false; }

            // n == -1 (EAGAIN/SSL_ERROR_WANT_READ): syscall YOK.
            // KURAL 3 (poll/select/epoll yasagi): busy-spin + _mm_pause().
            // Üstel backoff KESINLIKLE kullanilmaz (plan onayli).
            if (__builtin_expect(tsc_ns() - last_data_ns > MAX_SILENCE_NS, 0)) { // [FIX-CONN-2]
                std::cerr << "[ws] " << (MAX_SILENCE_NS / 1'000'000'000ULL)
                          << "s boyunca veri yok (ping dahil) -- baglanti muhtemelen"
                             " yari-acik, yeniden baglaniliyor\n";
                return false;
            }
            _mm_pause();
        }
        return true;
    }

    // EMIR-2: hic memmove/memcpy ile "sola kaydirma" YOK. Tuketilen frame
    // sadece head_ ilerletilerek dusurulur (O(1), kopya yok). Parcalanma
    // (fragmentation) DISINDA hicbir kopya yapilmaz -- frag_buf_'a yazim,
    // RFC6455'in mantiksal olarak ayri parcalari birlestirme zorunlulugundan
    // gelir ve EMIR'de acikca istisna tutulmustur.
    int try_parse_frame(uint8_t* rb) {
        const size_t len = static_cast<size_t>(tail_ - head_);
        if (len < 2) return 0;

        uint8_t* const buf = rb + (head_ & RBUF_MASK); // bitisik gorunum (ayna sayesinde)
        bool    fin    = (buf[0] & 0x80) != 0;
        uint8_t opcode = buf[0] & 0x0F;
        bool    masked = (buf[1] & 0x80) != 0;
        size_t  plen   = buf[1] & 0x7F;
        size_t  hsize  = 2;
        if (plen == 126) {
            if (len < 4) return 0;
            plen = (size_t(buf[2]) << 8) | buf[3]; hsize = 4;
        } else if (plen == 127) {
            if (len < 10) return 0;
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | buf[2+i];
            hsize = 10;
        }
        if (masked) hsize += 4;
        if (len < hsize + plen) return 0;

        if (__builtin_expect(plen > MAX_FRAME_PAYLOAD, 0)) {
            std::cerr << "[ws] frame payload sinirini asti (" << plen << ")\n";
            return -1;
        }

        // Yerinde (in-place) unmask -- kopya yok. payload hala rb icine isaret
        // eder (ayna-haritali, wrap-guvenli); head_ asagida ilerletilmeden once
        // okunup islenir.
        uint8_t* const payload = buf + hsize;
        if (masked) {
            const uint8_t* mk = buf + hsize - 4;
            for (size_t i = 0; i < plen; ++i) payload[i] ^= mk[i & 3]; // &3: power-of-2, modulo degil
        }

        switch (opcode) {
            case 0x1: case 0x2:
                if (!fin) {
                    std::memcpy(frag_buf_, payload, plen); // istisna: parcalanma birlestirmesi
                    frag_len_    = plen;
                    frag_opcode_ = opcode;
                } else {
                    dispatch_payload(std::string_view(reinterpret_cast<const char*>(payload), plen));
                }
                break;
            case 0x0:
                if (__builtin_expect(frag_len_ + plen > MAX_FRAME_PAYLOAD, 0)) {
                    std::cerr << "[ws] frag_buf_ tasmasi -- baglanti sifirlaniyor\n";
                    return -1;
                }
                std::memcpy(frag_buf_ + frag_len_, payload, plen); // istisna: parcalanma birlestirmesi
                frag_len_ += plen;
                if (fin) {
                    dispatch_payload(std::string_view(reinterpret_cast<const char*>(frag_buf_), frag_len_));
                    frag_len_ = 0;
                }
                break;
            case 0x8: std::cerr << "[ws] Close frame alindi\n"; return -1;
            case 0x9: { // Ping -> Pong: kontrol cercevesi, hot-path disi (nadir)
                auto pong = detail::make_pong_frame(std::vector<uint8_t>(payload, payload + plen));
                sock_.write(pong.data(), static_cast<int>(pong.size()));
                break;
            }
            case 0xA: break;
            default:  break;
        }

        head_ += hsize + plen; // SADECE imlec ilerletme -- kaydirma/kopya YOK
        return 1;
    }

    // ------------------------------------------------------------------------
    // dispatch_payload — V13
    //
    // Binance combined stream formatı:
    //   {"stream":"<sym>@<type>","data":{...}}
    //
    // Stream tipi tespiti: payload içindeki ilk '@' karakterinden sonraki harfe
    // bakılır. Bu, tüm payload'u taramak yerine O(sembol_uzunluğu) işlemdir;
    // sembol adı tipik olarak 6-10 karakter olduğundan pratikte sabittir.
    //   '@' + 'a' → aggTrade   → parse_agg_trade_dfa()
    //   '@' + 'b' → bookTicker → parse_book_ticker_dfa()
    //   '@' + 'd' → depth      → parse_depth()  (mevcut yol, değişmedi)
    // ------------------------------------------------------------------------
    void dispatch_payload(std::string_view msg) noexcept {
        if (__builtin_expect(msg.empty(), 0)) return;

        const char* const data = msg.data();
        const char* const end  = data + msg.size();

        // '@' karakterini bul — stream adında sembol/tip ayracı
        const char* p = data;
        while (p < end && *p != '@') ++p;
        if (__builtin_expect(p >= end, 0)) return;
        ++p; // '@' atlandı; *p şimdi stream tipinin ilk harfi

        switch (*p) {
            case 'a': parse_agg_trade_dfa(msg);   break; // aggTrade
            case 'b': parse_book_ticker_dfa(msg);  break; // bookTicker
            case 'd': parse_depth_dfa(msg);        break; // depth@100ms
            default:  break;
        }
    }

    // ========================================================================
    // parse_depth_dfa — V13 (EMIR-3 revizyonu)
    //
    // Eski parse_depth(): std::string + std::stod + heap (Kural 2 ihlali) +
    // double (Zorunlu Kural ihlali). Tamamen kaldırıldı.
    //
    // Hedef payload örneği:
    //   {"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","E":...,
    //    "s":"BTCUSDT","U":100,"u":105,
    //    "b":[["16500.00","1.50000000"],["16499.50","2.00000000"]],
    //    "a":[["16500.10","0.80000000"]]}}
    //
    // Toplanan alanlar:
    //   E → uint64_t ts_ms     (PARSE_INTEGER, tırnaksız)
    //   b → bid seviyeleri     (dfa::parse_depth_levels → int64_t[20])
    //   a → ask seviyeleri     (dfa::parse_depth_levels → int64_t[20])
    //   diğerleri (e,s,U,u,pu) → skip_value
    //
    // Sıfır heap, sıfır string kopyası, sıfır double — int64_t ×1e8.
    // ========================================================================
    void parse_depth_dfa(std::string_view msg) noexcept {
        if (__builtin_expect(!on_depth_, 0)) return;

        static constexpr std::string_view DATA_TAG = "\"data\":{";
        const auto dp = msg.find(DATA_TAG);
        if (__builtin_expect(dp == std::string_view::npos, 0)) return;

        const char* p   = msg.data() + dp + DATA_TAG.size();
        const char* end = msg.data() + msg.size();

        DepthUpdate upd{}; // alignas(64), stack-only
        uint64_t ts_ms = 0;

        uint8_t collected = 0u;
        static constexpr uint8_t F_E   = 0x1u;
        static constexpr uint8_t F_B   = 0x2u;
        static constexpr uint8_t F_A   = 0x4u;
        static constexpr uint8_t F_ALL = F_E | F_B | F_A;

        while (p < end) {
            while (p < end && *p != '"' && *p != '}') ++p;
            if (p >= end || *p == '}') break;

            ++p; // açılış '"'
            if (__builtin_expect(p >= end, 0)) break;
            const char key_first = *p;

            while (p < end && *p != '"') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // kapanış '"'

            while (p < end && *p != ':') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // ':'

            dfa::skip_ws(p, end);
            if (__builtin_expect(p >= end, 0)) break;

            // Çakışma yok: 'E'≠'b'≠'a' (depthUpdate alanları arasında).
            switch (key_first) {

                // ---- E: event time (unquoted int64, ms) ----------------------
                case 'E': {
                    if (__builtin_expect(static_cast<unsigned>(*p - '0') > 9u, 0)) {
                        dfa::skip_value(p, end);
                        break;
                    }
                    ts_ms      = dfa::parse_uint64(p);
                    collected |= F_E;
                    break;
                }

                // ---- b: bid seviyeleri (dizi → int64_t[20] ×1e8) -------------
                case 'b': {
                    if (__builtin_expect(*p != '[', 0)) { dfa::skip_value(p, end); break; }
                    upd.bid_n  = dfa::parse_depth_levels(p, end, upd.bid_prices, upd.bid_qtys, 20);
                    collected |= F_B;
                    break;
                }

                // ---- a: ask seviyeleri (dizi → int64_t[20] ×1e8) -------------
                case 'a': {
                    if (__builtin_expect(*p != '[', 0)) { dfa::skip_value(p, end); break; }
                    upd.ask_n  = dfa::parse_depth_levels(p, end, upd.ask_prices, upd.ask_qtys, 20);
                    collected |= F_A;
                    break;
                }

                // ---- Bilinmeyen alan: değeri atla -----------------------------
                // e, s, U, u, pu — hiçbiri heap tahsisi gerektirmez
                default: {
                    dfa::skip_value(p, end);
                    break;
                }
            }

            dfa::skip_ws(p, end);
            if (p < end && *p == ',') ++p;

            // Erken çıkış: tüm hedef alanlar toplandı
            if (__builtin_expect(collected == F_ALL, 0)) break;
        }

        // Zorunlu alanlar eksikse callback'i tetikleme
        if (__builtin_expect((collected & F_ALL) != F_ALL, 0)) return;

        upd.ts_exchange = ts_ms * 1000ULL; // ms → µs (şartname §6)
        on_depth_(upd);
    }

    // ========================================================================
    // parse_agg_trade_dfa  — V13 (§3, §4, §5)
    //
    // Hedef payload örneği:
    //   {"stream":"btcusdt@aggTrade","data":{"e":"aggTrade","E":...,"s":"BTCUSDT",
    //    "a":12345,"p":"16500.12","q":"0.05","f":100,"l":101,"T":1672512000000,
    //    "m":true,"M":true}}
    //
    // Toplanan alanlar (§3.1a):
    //   T  → uint64_t ts_ms  (PARSE_INTEGER)
    //   p  → int64_t  price  (PARSE_STRING_NUMBER, quoted decimal)
    //   q  → int64_t  qty    (PARSE_STRING_NUMBER, quoted decimal)
    //   m  → bool     buyer_is_maker  (PARSE_BOOL_TRUE / PARSE_BOOL_FALSE)
    //
    // Alan tanıma (§5): switch on first key char
    //   'T' → F_T,  'p' → F_P,  'q' → F_Q,  'm' → F_M
    //   diğerleri ('e','E','s','a','f','l','M') → UNKNOWN → skip_value
    //
    // DFA durumları (§3.2):
    //   SEEK_DATA → IN_DATA → {IN_KEY + AFTER_COLON + [value state]} → END
    // ========================================================================
    void parse_agg_trade_dfa(std::string_view msg) noexcept {
        if (__builtin_expect(!on_trade_, 0)) return;

        // ---- SEEK_DATA: "data":{ kalıbını bul (tek seferlik lineer tarama) --
        static constexpr std::string_view DATA_TAG = "\"data\":{";
        const auto dp = msg.find(DATA_TAG);
        if (__builtin_expect(dp == std::string_view::npos, 0)) return;

        const char* p   = msg.data() + dp + DATA_TAG.size();
        const char* end = msg.data() + msg.size();

        // ---- Toplanacak değerler -------------------------------------------
        int64_t  price          = 0;
        int64_t  qty            = 0;
        uint64_t ts_ms          = 0;
        bool     buyer_is_maker = false;

        // ---- Alan toplama bitmişlik bayrakları --------------------------------
        // Tüm zorunlu alanlar toplandığında erken çıkış yapılır.
        uint8_t collected = 0u;
        static constexpr uint8_t F_T   = 0x1u;
        static constexpr uint8_t F_P   = 0x2u;
        static constexpr uint8_t F_Q   = 0x4u;
        static constexpr uint8_t F_M   = 0x8u;
        static constexpr uint8_t F_ALL = F_T | F_P | F_Q | F_M;

        // ---- IN_DATA: "data":{...} içini tek geçişle işle -------------------
        while (p < end) {
            // Sonraki '"' veya '}' karakterine ulaş
            while (p < end && *p != '"' && *p != '}') ++p;
            if (p >= end || *p == '}') break; // END

            // IN_KEY: açılış tırnağını atla, ilk karakter → alan tipi
            ++p;
            if (__builtin_expect(p >= end, 0)) break;
            const char key_first = *p;

            // Anahtar sonuna kadar ilerle (kapanış tırnağı)
            while (p < end && *p != '"') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // kapanış '"'

            // AFTER_KEY: ':' atla
            while (p < end && *p != ':') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // ':'

            // AFTER_COLON: boşlukları atla
            dfa::skip_ws(p, end);
            if (__builtin_expect(p >= end, 0)) break;

            // ---- Alan tanıma (§5 — perfect hash: switch on first char) ------
            // aggTrade alanları: T, p, q, m
            // Çakışma yok: 'T'≠'p'≠'q'≠'m', büyük/küçük harf ayrımı sağlar.
            switch (key_first) {

                // ---- T: transaction timestamp (unquoted int64, ms) ----------
                case 'T': {
                    if (__builtin_expect(static_cast<unsigned>(*p - '0') > 9u, 0)) {
                        dfa::skip_value(p, end);
                        break;
                    }
                    ts_ms      = dfa::parse_uint64(p);
                    collected |= F_T;
                    break;
                }

                // ---- p: price (quoted decimal → int64_t ×1e8) ---------------
                case 'p': {
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end);
                        break;
                    }
                    price      = dfa::parse_fixed_point(p);
                    collected |= F_P;
                    break;
                }

                // ---- q: quantity (quoted decimal → int64_t ×1e8) ------------
                case 'q': {
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end);
                        break;
                    }
                    qty        = dfa::parse_fixed_point(p);
                    collected |= F_Q;
                    break;
                }

                // ---- m: buyer_is_maker (bool, lowercase) ---------------------
                // 'M' (büyük, "M" alanı) bu case'e düşmez (ASCII 77 ≠ 109).
                case 'm': {
                    if (*p == 't' && (end - p) >= 4) {
                        buyer_is_maker = true;
                        p += 4; // 't','r','u','e'
                        collected |= F_M;
                    } else if (*p == 'f' && (end - p) >= 5) {
                        buyer_is_maker = false;
                        p += 5; // 'f','a','l','s','e'
                        collected |= F_M;
                    } else {
                        dfa::skip_value(p, end);
                    }
                    break;
                }

                // ---- Bilinmeyen alan: değeri atla ----------------------------
                // e, E, s, a (aggr ID), f, l, M — hiçbiri heap tahsisi gerektirmez
                default: {
                    dfa::skip_value(p, end);
                    break;
                }
            }

            // Virgül varsa atla (IN_DATA → sonraki alana)
            dfa::skip_ws(p, end);
            if (p < end && *p == ',') ++p;

            // Erken çıkış: tüm hedef alanlar toplandı
            if (__builtin_expect(collected == F_ALL, 0)) break;
        }

        // Zorunlu alanlar eksikse callback'i tetikleme
        if (__builtin_expect((collected & (F_T | F_P | F_Q)) != (F_T | F_P | F_Q), 0))
            return;

        // ---- Stack-only yapı inşası ve emit (§8 memory ordering) -----------
        // Callback çağrısı: ring buffer'a yazım üretici tarafında yapılıyorsa
        // caller sorumludur (release/acquire semantic orada uygulanır).
        AggTrade t;
        t.ts_exchange    = ts_ms * 1000ULL; // ms → µs (şartname §6)
        t.price          = price;
        t.qty            = qty;
        t.buyer_is_maker = buyer_is_maker;
        on_trade_(t);
    }

    // ========================================================================
    // parse_book_ticker_dfa  — V13 (§3, §4, §5)
    //
    // Hedef payload örneği:
    //   {"stream":"btcusdt@bookTicker","data":{"u":400900217,"s":"BTCUSDT",
    //    "b":"16500.00","B":"1.5","a":"16500.10","A":"0.8",
    //    "T":1672512000000,"E":1672512000000}}
    //
    // Toplanan alanlar (§3.1b):
    //   T  → uint64_t ts_ms    (PARSE_INTEGER)
    //   b  → int64_t  best_bid      (quoted decimal)
    //   B  → int64_t  best_bid_qty  (quoted decimal)
    //   a  → int64_t  best_ask      (quoted decimal)
    //   A  → int64_t  best_ask_qty  (quoted decimal)
    //
    // Alan tanıma (§5):
    //   'T' → F_T
    //   'b' → F_BID      (lowercase, best bid price)
    //   'B' → F_BID_QTY  (uppercase, best bid qty)
    //   'a' → F_ASK      (lowercase, best ask price)
    //   'A' → F_ASK_QTY  (uppercase, best ask qty)
    //   diğerleri ('u','s','E') → UNKNOWN → skip_value
    // ========================================================================
    void parse_book_ticker_dfa(std::string_view msg) noexcept {
        if (__builtin_expect(!on_ticker_, 0)) return;

        // ---- SEEK_DATA -------------------------------------------------------
        static constexpr std::string_view DATA_TAG = "\"data\":{";
        const auto dp = msg.find(DATA_TAG);
        if (__builtin_expect(dp == std::string_view::npos, 0)) return;

        const char* p   = msg.data() + dp + DATA_TAG.size();
        const char* end = msg.data() + msg.size();

        // ---- Toplanacak değerler --------------------------------------------
        int64_t  best_bid     = 0;
        int64_t  best_bid_qty = 0;
        int64_t  best_ask     = 0;
        int64_t  best_ask_qty = 0;
        uint64_t ts_ms        = 0;

        // ---- Bitmişlik bayrakları -------------------------------------------
        uint8_t collected = 0u;
        static constexpr uint8_t F_T       = 0x01u;
        static constexpr uint8_t F_BID     = 0x02u;
        static constexpr uint8_t F_BID_QTY = 0x04u;
        static constexpr uint8_t F_ASK     = 0x08u;
        static constexpr uint8_t F_ASK_QTY = 0x10u;
        static constexpr uint8_t F_ALL     = F_T | F_BID | F_BID_QTY | F_ASK | F_ASK_QTY;

        // ---- IN_DATA: tek geçiş --------------------------------------------
        while (p < end) {
            while (p < end && *p != '"' && *p != '}') ++p;
            if (p >= end || *p == '}') break; // END

            ++p; // açılış '"'
            if (__builtin_expect(p >= end, 0)) break;
            const char key_first = *p;

            while (p < end && *p != '"') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // kapanış '"'

            while (p < end && *p != ':') ++p;
            if (__builtin_expect(p >= end, 0)) break;
            ++p; // ':'

            dfa::skip_ws(p, end);
            if (__builtin_expect(p >= end, 0)) break;

            // ---- Alan tanıma (§5) ------------------------------------------
            // bookTicker için: T, b, B, a, A
            // 'b'(98) ≠ 'B'(66) ve 'a'(97) ≠ 'A'(65): switch doğru ayırt eder.
            switch (key_first) {

                case 'T': { // transaction time (unquoted integer, ms)
                    if (__builtin_expect(static_cast<unsigned>(*p - '0') > 9u, 0)) {
                        dfa::skip_value(p, end); break;
                    }
                    ts_ms      = dfa::parse_uint64(p);
                    collected |= F_T;
                    break;
                }

                case 'b': { // best bid price (lowercase, quoted decimal)
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end); break;
                    }
                    best_bid   = dfa::parse_fixed_point(p);
                    collected |= F_BID;
                    break;
                }

                case 'B': { // best bid quantity (uppercase, quoted decimal)
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end); break;
                    }
                    best_bid_qty = dfa::parse_fixed_point(p);
                    collected   |= F_BID_QTY;
                    break;
                }

                case 'a': { // best ask price (lowercase, quoted decimal)
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end); break;
                    }
                    best_ask   = dfa::parse_fixed_point(p);
                    collected |= F_ASK;
                    break;
                }

                case 'A': { // best ask quantity (uppercase, quoted decimal)
                    if (__builtin_expect(*p != '"', 0)) {
                        dfa::skip_value(p, end); break;
                    }
                    best_ask_qty = dfa::parse_fixed_point(p);
                    collected   |= F_ASK_QTY;
                    break;
                }

                default: { // u (updateId), s (symbol), E (event time) → skip
                    dfa::skip_value(p, end);
                    break;
                }
            }

            dfa::skip_ws(p, end);
            if (p < end && *p == ',') ++p;

            if (__builtin_expect(collected == F_ALL, 0)) break;
        }

        // T yoksa now_ms() ile dolduruluyor (eski davranış korunuyor)
        if (__builtin_expect(!(collected & F_T), 0))
            ts_ms = now_ms();

        // Fiyat alanlarından herhangi biri yoksa emit etme
        if (__builtin_expect(
                (collected & (F_BID | F_BID_QTY | F_ASK | F_ASK_QTY))
                != (F_BID | F_BID_QTY | F_ASK | F_ASK_QTY), 0))
            return;

        // ---- Stack-only yapı inşası ve emit --------------------------------
        BookTicker t;
        t.ts_exchange   = ts_ms * 1000ULL; // ms → µs
        t.best_bid      = best_bid;
        t.best_bid_qty  = best_bid_qty;
        t.best_ask      = best_ask;
        t.best_ask_qty  = best_ask_qty;
        on_ticker_(t);
    }

    // ------------------------------------------------------------------------
    // KURAL 5: clock_gettime() yasak -- artik tsc_ns() (RDTSC) kullanilir.
    static uint64_t now_ms() noexcept {
        return tsc_ns() / 1'000'000ULL;
    }

    Config      cfg_;
    std::string sym_lower_, ws_path_;
    TLSSocket   sock_;
    DepthCb     on_depth_;
    TickerCb    on_ticker_;
    TradeCb     on_trade_;

    // EMIR-2: memmove yok -- head_/tail_ imlecleri MirrorRing uzerinde ilerler.
    // Kurulum (memfd/mmap) BIR KEZ nesne insasinda yapilir, hot-path'te degil.
    MirrorRing rbuf_{RBUF_CAP};

    // KURAL 2: heap yasak -- parcali (fragmented) frame tamponu sabit boyutlu.
    alignas(64) uint8_t frag_buf_[MAX_FRAME_PAYLOAD];
    size_t              frag_len_    = 0;
    uint8_t             frag_opcode_ = 0;
};

} // namespace kusak::ws