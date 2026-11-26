// ============================================================================
// KUSAK V12 — LIVE TRADING
// binance_http_client.hpp  [HFT REFACTOR]
//
// 1. curl_easy_perform → IO thread; strateji thread sadece SPSC ring'e yazar
// 2. std::string URL/body → thread_local char[] + snprintf + from_chars
// 3. HMAC_CTX reuse (thread_local), curl_global_init → call_once,
//    curl_slist prebuild in ctor → reuse, no per-request alloc/free
// 4. nlohmann::json emir yolundan kaldırıldı → minimal string_view scanner
//
// [FIX-NETWORK-3]: cold_handle() — thread_local CURL* lazy init + null guard
//   curl_easy_init() başarısız olursa nullptr döner ve çağıran fonksiyon
//   erken dönüş yaparak UB/segfault'dan kaçınır. Önceki kod: ilk init
//   başarısız → nullptr kalıcı → her çağrıda null dereference.
//
// [FIX-NETWORK-4]: TokenBucket — mutex kaldırıldı, lock-free atomics
//   tokens_, last_refill_ns_ → std::atomic<int64_t> (fixed-point x1e6 scale)
//   try_consume() → CAS döngüsü (~5 ns vs ~50 ns mutex kilit maliyeti)
//   sync_from_server() → atomic store + atomic CAS (IO thread'den güvenli)
//   submit_order() hot-path'te artık mutex contention yok
// ============================================================================

#pragma once

#include "binance_signer.hpp"
#include "binance_rest_client.hpp"
#include "research_types.hpp"
#include "order_guard.hpp"   // [FIX-P1-RATEGATE] kusak::guard::RateGate

#include <curl/curl.h>
#include "net/curl_sent.hpp"   // [FIX-P37-C19A] curl_request_maybe_sent
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#   define HFT_ALWAYS_INLINE __attribute__((always_inline)) inline
#   define HFT_LIKELY(x)     __builtin_expect(!!(x), 1)
#   define HFT_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#else
#   define HFT_ALWAYS_INLINE inline
#   define HFT_LIKELY(x)     (x)
#   define HFT_UNLIKELY(x)   (x)
#endif

// [FIX-SHUTDOWN-1] main.cpp'deki global durdurma bayragina erisim -- curl
// istekleri Ctrl+C sirasinda bekletmeden (retry/timeout beklemeden) hemen
// iptal edilebilsin diye.
extern std::atomic<bool> g_stop;

namespace kusak::net {

// ============================================================================
// §1  Constants
// ============================================================================

inline constexpr const char* BINANCE_SPOT_BASE_URL    = "https://api.binance.com";
inline constexpr const char* BINANCE_SPOT_TESTNET_URL = "https://testnet.binance.vision";
inline constexpr const char* BINANCE_FUT_BASE_URL     = "https://fapi.binance.com";
inline constexpr const char* BINANCE_FUT_TESTNET_URL  = "https://testnet.binancefuture.com";
// legacy aliases
inline constexpr const char* BINANCE_BASE_URL         = BINANCE_SPOT_BASE_URL;
inline constexpr const char* BINANCE_TESTNET_URL      = BINANCE_SPOT_TESTNET_URL;

inline constexpr int DEFAULT_WEIGHT_LIMIT_1M = 1200;
inline constexpr int DEFAULT_ORDER_LIMIT_1S  = 10;
inline constexpr int DEFAULT_ORDER_LIMIT_1D  = 200000;

// ============================================================================
// §2  curl_global_init — call_once, multi-instance safe
// ============================================================================

HFT_ALWAYS_INLINE void curl_init_once() noexcept {
    static std::once_flag flag;
    std::call_once(flag, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// ============================================================================
// §3  FastHmac — thread_local HMAC_CTX reuse, zero per-call allocation
//     Output: 64-char lowercase hex, written to caller-supplied buffer.
//     No std::string returned. No heap touched after init.
// ============================================================================

struct FastHmac {
    static constexpr int KEY_MAX = 64;
    unsigned char key[KEY_MAX] = {};
    int           key_len      = 0;

    void init(const char* secret, size_t n) noexcept {
        key_len = static_cast<int>(n < KEY_MAX ? n : KEY_MAX);
        std::memcpy(key, secret, static_cast<size_t>(key_len));
    }

    // Writes exactly 64 hex chars to out_hex (no null terminator added by this fn).
    // out_hex must be ≥ 64 chars.
    HFT_ALWAYS_INLINE int sign_hex(const char* msg, size_t msg_len,
                                   char* out_hex) noexcept
    {
        // thread_local context: created once per IO-thread, never freed during run
        thread_local HMAC_CTX* ctx = HMAC_CTX_new();

        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int  mac_len = 0;

        HMAC_Init_ex(ctx, key, key_len, EVP_sha256(), nullptr);
        HMAC_Update(ctx,
                    reinterpret_cast<const unsigned char*>(msg),
                    msg_len);
        HMAC_Final(ctx, mac, &mac_len);

        static constexpr char kHex[16] = {
            '0','1','2','3','4','5','6','7',
            '8','9','a','b','c','d','e','f'
        };
        for (unsigned i = 0; i < 32; ++i) {
            out_hex[2*i    ] = kHex[(mac[i] >> 4) & 0xF];
            out_hex[2*i + 1] = kHex[ mac[i]       & 0xF];
        }
        return 64;
    }
};

// ============================================================================
// §4  SPSC ring — cache-line separated, power-of-2 capacity
//     Strategy thread pushes (producer), IO thread pops (consumer).
//     Lock-free, zero allocation.
// ============================================================================

template<typename T, uint32_t N>
struct SpscRing {
    static_assert(N > 1 && (N & (N - 1)) == 0, "N must be power of 2");
    static constexpr uint32_t MASK = N - 1;

    alignas(64) std::atomic<uint32_t> head_{0};  // producer writes
    alignas(64) std::atomic<uint32_t> tail_{0};  // consumer reads
    alignas(64) std::array<T, N>      buf_{};

    // Producer (strategy thread): non-blocking
    HFT_ALWAYS_INLINE bool try_push(const T& v) noexcept {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t next = (h + 1) & MASK;
        if (HFT_UNLIKELY(next == tail_.load(std::memory_order_acquire)))
            return false;  // full
        buf_[h] = v;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer (IO thread): non-blocking
    HFT_ALWAYS_INLINE bool try_pop(T& out) noexcept {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire))
            return false;  // empty
        out = buf_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// §5  HttpResponse — public API type
// ============================================================================

struct HttpResponse {
    long        status_code    = 0;
    std::string body;          // cold-path only; hot-path uses RespBuf below
    int         used_weight_1m = -1;
    int         order_count_1s = -1;
    int         order_count_1d = -1;
    long        retry_after_s  = -1;
    // [FIX-P37-C19A] false YALNIZ curl hicbir istek baytinin gonderilmedigini kanitladiginda (cold_post).
    // Varsayilan true (fail-closed): kanit yoksa istek gonderilmis olabilir.
    bool        request_sent   = true;

    bool ok()           const noexcept { return status_code == 200; }
    bool rate_limited() const noexcept { return status_code == 429 || status_code == 418; }
    bool server_error() const noexcept { return status_code >= 500; }
};

// Lightweight response used in IO thread (no heap allocation)
struct RespLite {
    long status         = 0;
    int  used_weight_1m = -1;
    int  retry_after_s  = -1;
    bool gate_denied    = false;   // [FIX-P1-RATEGATE] true => istek RateGate tarafindan YEREL olarak engellendi, aga CIKMADI
    // body is written into thread_local resp_buf in IO thread
    const char* body_ptr = nullptr;
    size_t      body_len = 0;
};

// ============================================================================
// §6  OrderRequest — fixed-size, zero-allocation message to IO thread
// ============================================================================

struct OrderRequest {
    static constexpr size_t EP_CAP     = 64;
    static constexpr size_t PARAMS_CAP = 768;

    char     endpoint[EP_CAP]     = {};
    char     params[PARAMS_CAP]   = {};   // raw params, no sig/timestamp yet
    uint16_t ep_len               = 0;
    uint16_t params_len           = 0;
    long     recv_window_ms       = 5000;

    // Zero-allocation completion callback (raw fn ptr + user ptr)
    void (*on_done)(void* user, const RespLite& resp) = nullptr;
    void* user = nullptr;
};

// ============================================================================
// §7  TokenBucket — [FIX-NETWORK-4] Lock-free atomic rate limiter
//
// Önceki implementasyon: her metod std::mutex alıyordu.
// Sorun: try_consume() strateji thread'inden (hot-path),
//        sync_from_server() IO thread'inden çağrılır → iki thread
//        tek mutex üzerinde çakışır → submit_order() latency spike.
//
// Düzeltme: tokens_ ve last_refill_ns_ → std::atomic<int64_t>
//   - try_consume(): refill + CAS döngüsü, mutex YOK (~5 ns)
//   - sync_from_server(): atomic CAS ile min clamp, mutex YOK
//   - available(): atomic load, mutex YOK
//   - SCALE = 1_000_000 (mikro-token): double precision kaybı yok
// ============================================================================

class TokenBucket {
public:
    // [FIX-NETWORK-4]: mutex kaldırıldı; sabitler ve atomics
    static constexpr int64_t SCALE = 1'000'000LL; // fixed-point çarpanı

    TokenBucket(int capacity, double refill_rate_per_s) noexcept
        : capacity_units_(static_cast<int64_t>(capacity) * SCALE)
        , refill_units_per_s_(
              static_cast<int64_t>(refill_rate_per_s * static_cast<double>(SCALE)))
        , tokens_units_(static_cast<int64_t>(capacity) * SCALE)
        , last_refill_ns_(now_ns_())
    {}

    // [FIX-NETWORK-4]: Hot-path — lock-free CAS döngüsü
    // strateji thread'inden çağrılır; mutex contention yok
    HFT_ALWAYS_INLINE bool try_consume(int n = 1) noexcept {
        refill_now_(); // token yenileme — atomic, mutex yok
        const int64_t need = static_cast<int64_t>(n) * SCALE;
        int64_t cur = tokens_units_.load(std::memory_order_relaxed);
        while (cur >= need) {
            if (tokens_units_.compare_exchange_weak(
                    cur, cur - need,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
            // CAS başarısız: cur güncellendi, döngüye devam
        }
        return false;
    }

    // [FIX-NETWORK-4]: IO thread'inden çağrılır — atomic CAS ile min clamp
    // Sunucunun bildirdiği kullanıma göre token sayısını düşür (asla artırma)
    void sync_from_server(int used, int period_s = 60) noexcept {
        const int64_t server_remaining =
            std::max<int64_t>(0LL,
                capacity_units_ - static_cast<int64_t>(used) * SCALE);

        // [FIX-NETWORK-4]: min clamp — tokens azaltılabilir, artırılamaz
        int64_t cur = tokens_units_.load(std::memory_order_relaxed);
        while (cur > server_remaining) {
            if (tokens_units_.compare_exchange_weak(
                    cur, server_remaining,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        // Refill hızını güncelle (period değişirse)
        if (period_s > 0) {
            refill_units_per_s_.store(
                capacity_units_ / static_cast<int64_t>(period_s),
                std::memory_order_release); // [FIX-NETWORK-4]
        }
    }

    // [FIX-NETWORK-4]: mutex YOK — salt okunur atomic load
    double available() const noexcept {
        return static_cast<double>(
            tokens_units_.load(std::memory_order_relaxed))
            / static_cast<double>(SCALE);
    }

    int capacity() const noexcept {
        return static_cast<int>(capacity_units_ / SCALE);
    }

private:
    // [FIX-NETWORK-4]: Token yenileme — CAS ile tek thread erişimi garantisi
    // Birden fazla thread aynı anda refill_now_() çağırırsa sadece biri kazanır;
    // diğerleri eski değerle devam eder (stale refill değil, sadece gecikmeli).
    void refill_now_() noexcept {
        const int64_t now  = now_ns_();
        int64_t       last = last_refill_ns_.load(std::memory_order_relaxed);

        if (now <= last) return; // Saat geri gitmedi (güvenli)

        // [FIX-NETWORK-4]: CAS ile tek refill sahibi seç
        if (!last_refill_ns_.compare_exchange_strong(
                last, now,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return; // Başka thread refill ediyor
        }

        const int64_t elapsed_ns = now - last;
        const int64_t rate       = refill_units_per_s_.load(std::memory_order_relaxed);
        // elapsed_ns * rate / 1e9 — taşmadan kaçın: rate max ~1200*1e6=1.2e9,
        // elapsed_ns max ~1e9 (1 sn), çarpım max ~1.2e18 < INT64_MAX ✓
        const int64_t add = (elapsed_ns * rate) / 1'000'000'000LL;
        if (add <= 0) return;

        int64_t cur  = tokens_units_.load(std::memory_order_relaxed);
        int64_t next = 0;
        do {
            next = std::min(capacity_units_, cur + add);
        } while (!tokens_units_.compare_exchange_weak(
                     cur, next,
                     std::memory_order_acq_rel,
                     std::memory_order_relaxed));
    }

    static int64_t now_ns_() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // [FIX-NETWORK-4]: const capacity; atomics; mutex KALDIRILDI
    const int64_t        capacity_units_;
    std::atomic<int64_t> refill_units_per_s_;
    std::atomic<int64_t> tokens_units_;
    std::atomic<int64_t> last_refill_ns_;
    // mutable std::mutex mu_; ← KALDIRILDI [FIX-NETWORK-4]
};

// ============================================================================
// §8  CURL write / header callbacks — zero allocation
// ============================================================================

struct CurlBodyBuf {
    char*    ptr;
    size_t   cap;
    size_t   len;
};

static size_t write_cb(void* data, size_t sz, size_t nm, void* ud) noexcept {
    auto* b = static_cast<CurlBodyBuf*>(ud);
    const size_t n    = sz * nm;
    const size_t room = b->cap > b->len ? b->cap - b->len : 0;
    const size_t take = n < room ? n : room;
    std::memcpy(b->ptr + b->len, data, take);
    b->len += take;
    return n;
}

// Case-insensitive ASCII prefix compare, zero allocation
static HFT_ALWAYS_INLINE bool iprefix(const char* s, size_t sn,
                                       const char* p, size_t pn) noexcept
{
    if (sn < pn) return false;
    for (size_t i = 0; i < pn; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[i]) | 0x20;
        const unsigned char b = static_cast<unsigned char>(p[i]) | 0x20;
        if (a != b) return false;
    }
    return true;
}

static size_t header_cb(char* buf, size_t sz, size_t nm, void* ud) noexcept {
    auto* r = static_cast<RespLite*>(ud);
    const size_t n = sz * nm;

    // from_chars: zero allocation, no exception
    auto parse_int_after = [&](const char* pfx) noexcept -> int {
        const size_t pn = std::strlen(pfx);
        if (!iprefix(buf, n, pfx, pn)) return -1;
        const char* p = buf + pn;
        const char* e = buf + n;
        while (p < e && (*p == ' ' || *p == '\t')) ++p;
        int v = -1;
        auto [ep, ec] = std::from_chars(p, e, v);
        return (ec == std::errc()) ? v : -1;
    };

    if (int v = parse_int_after("x-mbx-used-weight-1m:"); v >= 0) r->used_weight_1m = v;
    if (int v = parse_int_after("retry-after:");          v >= 0) r->retry_after_s  = v;
    return n;
}

// ============================================================================
// §9  BinanceHttpClient — main class
//
//   Hot-path (strategy thread):
//     submit_order() → SPSC ring push (non-blocking, ~10 ns)
//
//   Cold-path (IO thread):
//     io_loop() → pops ring → builds URL/body in thread_local buffers →
//                 curl_easy_perform → callback → optional rate-limit sleep
//
//   Synchronous methods (get_public, get_signed, fetch_*) are cold-path only.
//   Never call from hot-loop.
// ============================================================================

class BinanceHttpClient
    : public std::enable_shared_from_this<BinanceHttpClient>
{
public:
    struct Config {
        const char* api_key        = nullptr;
        const char* api_secret     = nullptr;
        const char* market         = "SPOT";   // "SPOT" | "FUTURES"
        bool        testnet        = false;
        long        connect_timeout_ms = 3000;
        long        request_timeout_ms = 8000;
        long        recv_window_ms     = 5000;
        int         max_retries        = 3;
        bool        verify_ssl         = true;
        bool        require_clock_sync = false;
    };

    // ── Constructor / Destructor ─────────────────────────────────────────────
    explicit BinanceHttpClient(const Config& c)
        : cfg_(c)
        , base_url_(resolve_base_url(c))
        , weight_limiter_(DEFAULT_WEIGHT_LIMIT_1M,
                          static_cast<double>(DEFAULT_WEIGHT_LIMIT_1M) / 60.0)
        , order_limiter_(DEFAULT_ORDER_LIMIT_1S,
                         static_cast<double>(DEFAULT_ORDER_LIMIT_1S))
    {
        curl_init_once();

        gate_.weight_limit = static_cast<int>(DEFAULT_WEIGHT_LIMIT_1M);   // [FIX-P1-RATEGATE]

        // HMAC signer
        hmac_.init(c.api_secret, std::strlen(c.api_secret));

        // Legacy signer for clock-sync / signed GET helpers
        signer_.init(c.api_secret, base_url_, c.require_clock_sync);

        // Prebuild auth headers — created once, reused for every hot POST
        // "Expect:" disables 100-continue (saves a round-trip)
        {
            char key_hdr[256];
            std::snprintf(key_hdr, sizeof(key_hdr), "X-MBX-APIKEY: %s", c.api_key);
            hdr_post_auth_ = curl_slist_append(nullptr, key_hdr);
            hdr_post_auth_ = curl_slist_append(hdr_post_auth_,
                "Content-Type: application/x-www-form-urlencoded");
            hdr_post_auth_ = curl_slist_append(hdr_post_auth_, "Expect:");

            hdr_get_auth_ = curl_slist_append(nullptr, key_hdr);
            hdr_get_auth_ = curl_slist_append(hdr_get_auth_, "Expect:");
        }
    }

    ~BinanceHttpClient() {
        stop_io_thread();
        if (hdr_post_auth_) curl_slist_free_all(hdr_post_auth_);
        if (hdr_get_auth_)  curl_slist_free_all(hdr_get_auth_);
    }

    BinanceHttpClient(const BinanceHttpClient&)            = delete;
    BinanceHttpClient& operator=(const BinanceHttpClient&) = delete;

    // ── IO thread lifecycle ──────────────────────────────────────────────────
    void start_io_thread() {
        io_running_.store(true, std::memory_order_release);
        io_thread_ = std::thread([this]{ io_loop(); });
    }

    void stop_io_thread() noexcept {
        if (io_running_.exchange(false, std::memory_order_acq_rel)) {
            if (io_thread_.joinable()) io_thread_.join();
        }
    }

    // ── §A  HOT-PATH: strategy thread API ───────────────────────────────────

    // Non-blocking enqueue (~10 ns).
    // params: raw URL-encoded fields WITHOUT timestamp/signature, e.g.
    //         "symbol=BTCUSDT&side=BUY&type=MARKET&quantity=0.001"
    // Returns false if ring is full (back-pressure — caller must handle).
    HFT_ALWAYS_INLINE bool submit_order(const char*  endpoint,
                                        const char*  params,
                                        void (*on_done)(void*, const RespLite&) = nullptr,
                                        void* user = nullptr) noexcept
    {
        // [FIX-P1-RATEGATE] Ban/backoff aktifse emir halkaya HIC girmez (stale emir birikmez).
        if (!gate_allow(/*is_order=*/true)) return false;
        // [FIX-NETWORK-4]: try_consume() artık lock-free; mutex contention yok
        if (!order_limiter_.try_consume(1)) return false;

        OrderRequest req;
        req.ep_len = static_cast<uint16_t>(
            std::min(std::strlen(endpoint), OrderRequest::EP_CAP - 1));
        req.params_len = static_cast<uint16_t>(
            std::min(std::strlen(params), OrderRequest::PARAMS_CAP - 1));
        std::memcpy(req.endpoint, endpoint, req.ep_len);
        req.endpoint[req.ep_len] = '\0';
        std::memcpy(req.params, params, req.params_len);
        req.params[req.params_len] = '\0';
        req.recv_window_ms = cfg_.recv_window_ms;
        req.on_done        = on_done;
        req.user           = user;

        return order_ring_.try_push(req);
    }

    // ── §B  COLD-PATH: synchronous helpers (never call from hot-loop) ────────

    // Config accessors
    void               set_recv_window(long ms) noexcept { cfg_.recv_window_ms = ms; }
    void               set_testnet(bool v)      noexcept {
        cfg_.testnet = v;
        base_url_ = resolve_base_url(cfg_);
    }
    const char*        config_market()    const noexcept { return cfg_.market; }

    // Clock sync
    bool    force_clock_sync()    noexcept { return signer_.force_sync(); }
    bool    ensure_clock_synced() noexcept { return signer_.ensure_synced_ok(); }
    int64_t clock_offset_ms()     const noexcept { return signer_.clock_offset_ms(); }
    bool    clock_is_synced()     const noexcept { return signer_.is_synchronised(); }

    // Metrics
    uint64_t total_requests()   const noexcept { return total_requests_.load(std::memory_order_relaxed); }
    uint64_t total_retries()    const noexcept { return total_retries_.load(std::memory_order_relaxed); }
    uint64_t rate_limit_hits()  const noexcept { return rate_limit_hits_.load(std::memory_order_relaxed); }
    // [FIX-P1-RATEGATE] Merkezi ban durumu (strateji/ana dongu gorunurlugu)
    bool     rate_gate_banned()  const noexcept { GateLock g(gate_lock_); return gate_.banned(gate_now_ms()); }
    int64_t  rate_gate_wait_ms() const noexcept { GateLock g(gate_lock_); return gate_.wait_ms(gate_now_ms()); }
    uint32_t rate_gate_strikes() const noexcept { GateLock g(gate_lock_); return gate_.strikes; }
    int      weight_available() const noexcept { return static_cast<int>(weight_limiter_.available()); }
    int      order_tokens_available() const noexcept { return static_cast<int>(order_limiter_.available()); }

    // ── Unsigned GET ─────────────────────────────────────────────────────────
    HttpResponse get_public(const char* endpoint,
                            const char* query  = "",
                            int         weight = 1)
    {
        weight_limiter_.try_consume(weight);  // best-effort; no sleep in hot-path
        thread_local std::array<char, 2048> url_buf;
        const int n = (query && query[0])
            ? std::snprintf(url_buf.data(), url_buf.size(),
                            "%s%s?%s", base_url_, endpoint, query)
            : std::snprintf(url_buf.data(), url_buf.size(),
                            "%s%s", base_url_, endpoint);
        (void)n;
        return cold_get(url_buf.data(), /*authenticated=*/false);
    }

    // ── Signed GET ───────────────────────────────────────────────────────────
    HttpResponse get_signed(const char* endpoint,
                            const char* query         = "",
                            int         weight        = 1,
                            long        recv_window_ms = 0)
    {
        weight_limiter_.try_consume(weight);
        const long rw = (recv_window_ms > 0) ? recv_window_ms : cfg_.recv_window_ms;

        signer_.ensure_synced();
        // sign_query returns std::string (cold-path — acceptable)
        const std::string signed_query = signer_.sign_query(query ? query : "", rw);

        thread_local std::array<char, 2048> url_buf;
        std::snprintf(url_buf.data(), url_buf.size(),
                      "%s%s?%s", base_url_, endpoint, signed_query.c_str());
        return cold_get(url_buf.data(), /*authenticated=*/true);
    }

    // ── Signed POST (cold-path, e.g. cancel, leverage) ───────────────────────
    // [FIX-P36-RETRY] allow_retry=false: emir olusturan POST tek deneme yapar (bkz. cold_post).
    HttpResponse post_signed(const char* endpoint,
                             const char* params         = "",
                             int         order_count_cost = 1,
                             bool        allow_retry      = true)
    {
        if (!order_limiter_.try_consume(order_count_cost)) {
            HttpResponse r;
            r.status_code = 429;
            r.body = "{\"code\":-1003,\"msg\":\"Local order rate limit\"}";
            return r;
        }
        weight_limiter_.try_consume(1);

        signer_.ensure_synced();
        const std::string signed_body = signer_.sign_body(params ? params : "",
                                                          cfg_.recv_window_ms);

        thread_local std::array<char, 2048> url_buf;
        std::snprintf(url_buf.data(), url_buf.size(),
                      "%s%s", base_url_, endpoint);
        return cold_post(url_buf.data(), signed_body.c_str(),
                         static_cast<long>(signed_body.size()),
                         /*key_only=*/false, allow_retry);
    }

    // ── Signed DELETE (cold-path, e.g. cancel order) ──────────────────────────
    HttpResponse delete_signed(const char* endpoint,
                               const char* params         = "",
                               int         weight         = 1)
    {
        weight_limiter_.try_consume(weight);

        signer_.ensure_synced();
        const std::string signed_query = signer_.sign_query(params ? params : "",
                                                            cfg_.recv_window_ms);

        thread_local std::array<char, 2048> url_buf;
        std::snprintf(url_buf.data(), url_buf.size(),
                      "%s%s?%s", base_url_, endpoint, signed_query.c_str());
        return cold_delete(url_buf.data());
    }

    // API-key-only POST (listenKey etc.)
    HttpResponse post_api_key(const char* endpoint, const char* body = "") {
        thread_local std::array<char, 2048> url_buf;
        std::snprintf(url_buf.data(), url_buf.size(), "%s%s", base_url_, endpoint);
        return cold_post(url_buf.data(), body, static_cast<long>(std::strlen(body)),
                         /*key_only=*/true);
    }

    // ── ExchangeInfo — minimal string_view scanner, zero nlohmann ────────────
    bool fetch_exchange_info(const char*                       symbol,
                             kusak::exchange::FilterValidator& validator)
    {
        thread_local std::array<char, 128> qbuf;
        std::snprintf(qbuf.data(), qbuf.size(), "symbol=%s", symbol);
        auto resp = get_public("/api/v3/exchangeInfo", qbuf.data(), 10);
        if (!resp.ok()) { log_err("fetch_exchange_info", resp.status_code); return false; }
        return parse_exchange_info_fast(resp.body, validator);
    }

    bool fetch_exchange_info_multi(const std::vector<std::string>&   symbols,
                                   kusak::exchange::FilterValidator& validator)
    {
        // Build query string in stack buffer (cold-path: acceptable size)
        thread_local std::array<char, 2048> qbuf;
        int pos = std::snprintf(qbuf.data(), qbuf.size(), "symbols=[");
        for (size_t i = 0; i < symbols.size() && pos < (int)qbuf.size() - 10; ++i) {
            pos += std::snprintf(qbuf.data() + pos, qbuf.size() - pos,
                                 "%%22%s%%22%s",
                                 symbols[i].c_str(),
                                 (i + 1 < symbols.size()) ? "," : "");
        }
        std::snprintf(qbuf.data() + pos, qbuf.size() - pos, "]");
        auto resp = get_public("/api/v3/exchangeInfo", qbuf.data(), 10);
        if (!resp.ok()) { log_err("fetch_exchange_info_multi", resp.status_code); return false; }
        return parse_exchange_info_fast(resp.body, validator);
    }

    // ── Account balances ─────────────────────────────────────────────────────
    struct Balance { std::string asset; double free = 0.0; double locked = 0.0; };

    std::vector<Balance> fetch_account_balances(bool omit_zero = false,
                                                bool debug     = false)
    {
        const char* q = omit_zero ? "omitZeroBalances=true" : "";
        auto resp = get_signed("/api/v3/account", q, 10);
        if (debug)
            std::cerr << "[BinanceHttpClient] fetch_account_balances HTTP "
                      << resp.status_code << " body: "
                      << resp.body.substr(0, 500) << "\n";
        if (!resp.ok()) { log_err("fetch_account_balances", resp.status_code); return {}; }
        return parse_balances_fast(resp.body);
    }

    double fetch_balance(const char* asset, bool omit_zero = false) {
        for (const auto& b : fetch_account_balances(omit_zero))
            if (b.asset == asset) return b.free;
        return 0.0;
    }

    // ── HttpFunc factories (legacy adapter) ──────────────────────────────────
    kusak::exchange::RestOrderClient::HttpFunc make_signed_post_func() {
        auto self = shared_from_this();
        return [self](const std::string& ep,
                      const std::string& params) -> kusak::exchange::HttpResponse
        {
            // [FIX-P1-RATEGATE] sleep+yeniden-deneme KALDIRILDI: ban/backoff karari merkezi RateGate'te.
            //  Reddedilen istek oldugu gibi (429/418) yukari doner; cagiran (order thread) hata olarak isler.
            // [FIX-P36-RETRY] Bu HttpFunc yalniz RestOrderClient::new_order (emir olusturma) icindir:
            //  otomatik yeniden deneme YOK. Ilk deneme borsaya ulasmis olabilir; ayni govdeyi tekrar
            //  gondermek duplicate (-2010) / -1021 ile "kesin ret" gibi gorunen sonuc uretir. Belirsiz
            //  sonuc (-1/5xx) new_order'da AMBIGUOUS siniflanir -> UNKNOWN -> origClientOrderId POLL.
            auto r = self->post_signed(ep.c_str(), params.c_str(), /*order_count_cost=*/1, /*allow_retry=*/false);
            kusak::exchange::HttpResponse out;
            out.status_code  = r.status_code;
            out.body         = std::move(r.body);
            out.request_sent = r.request_sent;   // [FIX-P37-C19A]
            return out;
        };
    }

    using AccountHttpFunc = kusak::exchange::RestOrderClient::AccountHttpFunc;
    AccountHttpFunc make_signed_get_func() {
        auto self = shared_from_this();
        return [self](const std::string& ep,
                      const std::string& query,
                      long recv_window_ms) -> kusak::exchange::HttpResponse
        {
            // [FIX-P1-RATEGATE] sleep+yeniden-deneme KALDIRILDI (bkz. make_signed_post_func).
            auto r = self->get_signed(ep.c_str(), query.c_str(), 1, recv_window_ms);
            kusak::exchange::HttpResponse out;
            out.status_code = r.status_code;
            out.body        = std::move(r.body);
            return out;
        };
    }

    using PublicGetFunc = std::function<kusak::exchange::HttpResponse(
        const std::string&, const std::string&)>;
    PublicGetFunc make_public_get_func() {
        auto self = shared_from_this();
        return [self](const std::string& ep,
                      const std::string& query) -> kusak::exchange::HttpResponse
        {
            auto r = self->get_public(ep.c_str(), query.c_str(), 10);
            kusak::exchange::HttpResponse out;
            out.status_code = r.status_code;
            out.body        = std::move(r.body);
            return out;
        };
    }

    void log_err(const char* ctx, long code) noexcept {
        std::cerr << "[BinanceHttpClient::" << ctx << "] HTTP " << code << "\n";
    }

    // [FIX-SHUTDOWN-1] curl'un periyodik cagirdigi ilerleme geri-cagirmasi.
    // g_stop true olursa 1 dondururuz -- bu, curl'a "transferi HEMEN iptal
    // et" der (retry/backoff/timeout beklemeden). Aksi halde 0 donup normal
    // devam eder.
    static int abort_progress_cb(void*, curl_off_t, curl_off_t,
                                  curl_off_t, curl_off_t) noexcept {
        return g_stop.load(std::memory_order_acquire) ? 1 : 0;
    }

    // [FIX-SHUTDOWN-2] Normal sleep_for, g_stop true olsa bile tam suresini
    // bosa bekler (retry backoff'ta 9.6s'e, rate-limit beklemesinde 5s'e
    // kadar cikabiliyordu). Bunun yerine 50ms'lik adimlarla uyuyup her
    // adimda g_stop kontrol ederiz -- Ctrl+C artik ~50ms icinde etkili olur.
    static void interruptible_sleep_ms(long total_ms) noexcept {
        constexpr long step_ms = 50;
        long remaining = total_ms;
        while (remaining > 0) {
            if (g_stop.load(std::memory_order_acquire)) return;
            const long chunk = (remaining < step_ms) ? remaining : step_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            remaining -= chunk;
        }
    }

private:
    // ============================================================================
    // §IO  IO thread — single dedicated thread for all hot-path orders
    //      curl_easy_perform blocks here; strategy thread is never touched.
    //      sleep_for for rate-limits lives here, not in strategy thread.
    // ============================================================================

    void io_loop() {
        // Single persistent CURL handle: TLS session + TCP connection reused
        CURL* easy = curl_easy_init();

        // thread_local scratch — zero per-request allocation
        thread_local std::array<char, 2048> url_buf;
        thread_local std::array<char, 2048> body_buf;
        thread_local std::array<char, 8192> resp_buf;

        while (io_running_.load(std::memory_order_acquire)) {
            OrderRequest req;
            if (!order_ring_.try_pop(req)) {
                std::this_thread::yield();
                continue;
            }

            // [FIX-P1-RATEGATE] ZORUNLU KAPI: aga cikmadan once allow(). Ban sirasinda istek atmak
            // banı uzatir. Bekleyip sonra gondermek (stale emir) yerine FAIL-CLOSED: emir gonderilmez,
            // on_done 429 + gate_denied ile cagirana bildirilir.
            if (!gate_allow(/*is_order=*/true)) {
                RespLite denied{};
                denied.status        = 429;
                denied.gate_denied   = true;
                denied.retry_after_s = static_cast<int>((gate_wait_ms() + 999) / 1000);
                ++rate_limit_hits_;
                if (req.on_done) req.on_done(req.user, denied);
                continue;
            }

            // Build signed body in body_buf — zero allocation
            const int64_t now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count();

            int blen = 0;
            if (req.params_len > 0) {
                blen += std::snprintf(body_buf.data(),
                                      body_buf.size(),
                                      "%.*s&recvWindow=%ld&timestamp=%lld",
                                      static_cast<int>(req.params_len),
                                      req.params,
                                      req.recv_window_ms,
                                      static_cast<long long>(now_ms));
            } else {
                blen += std::snprintf(body_buf.data(),
                                      body_buf.size(),
                                      "recvWindow=%ld&timestamp=%lld",
                                      req.recv_window_ms,
                                      static_cast<long long>(now_ms));
            }

            // HMAC — thread_local context reuse
            char sig_hex[64];
            hmac_.sign_hex(body_buf.data(), static_cast<size_t>(blen), sig_hex);
            blen += std::snprintf(body_buf.data() + blen,
                                  body_buf.size()  - static_cast<size_t>(blen),
                                  "&signature=%.64s",
                                  sig_hex);

            // URL — stack buffer
            std::snprintf(url_buf.data(), url_buf.size(),
                          "%s%.*s",
                          base_url_,
                          static_cast<int>(req.ep_len),
                          req.endpoint);

            // Response buffers — no heap
            RespLite      resp{};
            CurlBodyBuf   cbuf{ resp_buf.data(), resp_buf.size(), 0 };

            curl_easy_reset(easy);
            curl_easy_setopt(easy, CURLOPT_URL,             url_buf.data());
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER,      hdr_post_auth_);  // prebuilt, reused
            curl_easy_setopt(easy, CURLOPT_POST,            1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS,      body_buf.data()); // no copy
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,   static_cast<long>(blen));
            curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,   write_cb);
            curl_easy_setopt(easy, CURLOPT_WRITEDATA,       &cbuf);
            curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,  header_cb);
            curl_easy_setopt(easy, CURLOPT_HEADERDATA,      &resp);
            curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, cfg_.connect_timeout_ms);
            curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,      cfg_.request_timeout_ms);
            curl_easy_setopt(easy, CURLOPT_NOSIGNAL,        1L);
            curl_easy_setopt(easy, CURLOPT_NOPROGRESS,      0L);   // [FIX-SHUTDOWN-1]
            curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abort_progress_cb);
            curl_easy_setopt(easy, CURLOPT_XFERINFODATA,    nullptr);
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE,   1L);
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE,    30L);
            curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL,   15L);
            curl_easy_setopt(easy, CURLOPT_TCP_NODELAY,     1L);
            curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,    CURL_HTTP_VERSION_1_1);
            if (!cfg_.verify_ssl) {
                curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
            }

            const CURLcode rc = curl_easy_perform(easy);
            if (rc == CURLE_OK) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &resp.status);
            } else {
                resp.status = -1;
            }

            resp.body_ptr = resp_buf.data();
            resp.body_len = cbuf.len;

            ++total_requests_;

            if (resp.used_weight_1m >= 0)
                weight_limiter_.sync_from_server(resp.used_weight_1m, 60);

            // [FIX-P1-RATEGATE] sleep_for KALDIRILDI. Yanit merkezi kapiya islenir (418 => min 2 dk ban,
            // 429 => ustel backoff + strikes). Sonraki istek yukaridaki allow() kapisinda durur.
            gate_note(resp.status, resp.retry_after_s, resp.used_weight_1m);
            if (resp.status == 429 || resp.status == 418) ++rate_limit_hits_;

            if (req.on_done) req.on_done(req.user, resp);
        }

        curl_easy_cleanup(easy);
    }

    // ============================================================================
    // [FIX-P1-RATEGATE]  Merkezi RateGate baglantisi
    //   Tum istemciler (IO thread + cold-path: main/order/strateji thread'leri) AYNI gate_ orneginden
    //   gecer. RateGate duz alanlardan olustugu icin kisa kritik bolum atomic_flag spinlock ile
    //   korunur (noexcept, istisna firlatmaz, hot-path'te mutex/syscall yok).
    //   Saat: steady_clock (duvar saati atlamalarindan etkilenmez).
    //   Emir sinifi (is_order=true): YALNIZ ban ile engellenir (iptal dahil: risk azaltan istek
    //   agirlik tavanina takilmaz). Okuma sinifi: ban + soft agirlik tavani (%80) ile engellenir.
    // ============================================================================
    struct GateLock {
        explicit GateLock(std::atomic_flag& f) noexcept : f_(f) {
            while (f_.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        }
        ~GateLock() { f_.clear(std::memory_order_release); }
        GateLock(const GateLock&)            = delete;
        GateLock& operator=(const GateLock&) = delete;
        std::atomic_flag& f_;
    };

    static int64_t gate_now_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    bool gate_allow(bool is_order) const noexcept {
        GateLock g(gate_lock_);
        return gate_.allow(gate_now_ms(), is_order);
    }
    int64_t gate_wait_ms() const noexcept {
        GateLock g(gate_lock_);
        return gate_.wait_ms(gate_now_ms());
    }
    void gate_note(long status, int retry_after_s, int used_weight) noexcept {
        if (status <= 0) return;                    // ag hatasi (-1): sunucu karari yok
        GateLock g(gate_lock_);
        gate_.on_response(static_cast<int>(status), retry_after_s, used_weight, gate_now_ms());
    }
    // Yerel red: aga CIKILMADI. 429 + -1003 govdesi => RestOrderClient.error.is_rate_limit() ile uyumlu.
    HttpResponse gate_denied_response() const {
        HttpResponse r;
        r.status_code   = 429;
        const int64_t w = gate_wait_ms();
        r.retry_after_s = (w > 0) ? static_cast<long>((w + 999) / 1000) : 1L;
        r.body = "{\"code\":-1003,\"msg\":\"Local RateGate: request blocked (ban/backoff/weight)\"}";
        return r;
    }

    // ============================================================================
    // §COLD  Cold-path synchronous execute helpers
    //        These run on the calling thread (never called from hot-loop).
    //        They use a separate CURL handle from the pool to avoid
    //        contention with the IO thread's dedicated handle.
    // ============================================================================

    // [FIX-NETWORK-3]: cold_handle() — lazy init + null guard
    // Önceki kod: thread_local CURL* h = curl_easy_init() → bir kez çalışır;
    // ilk init başarısız → h kalıcı nullptr → her çağrıda curl_easy_reset(nullptr) UB.
    // Düzeltme: h başlangıçta nullptr; her çağrıda null kontrolü + lazy reinit.
    CURL* cold_handle() noexcept {
        thread_local CURL* h = nullptr;  // [FIX-NETWORK-3]: nullptr ile başla
        if (!h) h = curl_easy_init();    // [FIX-NETWORK-3]: lazy init
        if (!h) return nullptr;          // [FIX-NETWORK-3]: init başarısız → null döndür
        curl_easy_reset(h);              // clears options but preserves connection/SSL cache
        return h;
    }

    // Free C-function for CURL write callback (no capture → valid function pointer)
    static size_t cold_write_cb(void* ptr, size_t sz, size_t nm, void* ud) noexcept {
        auto* s = static_cast<std::string*>(ud);
        s->append(static_cast<char*>(ptr), sz * nm);
        return sz * nm;
    }

    HttpResponse cold_get(const char* url, bool authenticated) {
        HttpResponse result;
        std::string  body_accum;

        CURL* easy = cold_handle();
        if (!easy) { // [FIX-NETWORK-3]: null guard — curl_easy_init() OOM
            result.status_code = -1;
            result.body = "curl_easy_init() failed (OOM or curl not initialized)";
            return result;
        }
        RespLite rl{};

        curl_easy_setopt(easy, CURLOPT_URL,             url);
        curl_easy_setopt(easy, CURLOPT_HTTPGET,         1L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,   cold_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA,       &body_accum);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,  header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA,      &rl);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, cfg_.connect_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,      cfg_.request_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL,        1L);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS,      0L);   // [FIX-SHUTDOWN-1]
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abort_progress_cb);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA,    nullptr);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE,   1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,    CURL_HTTP_VERSION_1_1);
        if (authenticated)
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER,  hdr_get_auth_);
        if (!cfg_.verify_ssl) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
            if (attempt > 0) {
                ++total_retries_;
                interruptible_sleep_ms((1L << attempt) * 200L);   // [FIX-SHUTDOWN-2]
                if (g_stop.load(std::memory_order_acquire)) break;   // [FIX-SHUTDOWN-2]
                body_accum.clear();
                rl = {};
            }

            // [FIX-P1-RATEGATE] her deneme (retry dahil) allow() kapisindan gecer
            if (!gate_allow(false)) return gate_denied_response();
            const CURLcode rc = curl_easy_perform(easy);
            if (rc != CURLE_OK) { result.status_code = -1; result.body = curl_easy_strerror(rc); continue; }

            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status_code);
            result.body           = std::move(body_accum);
            result.used_weight_1m = rl.used_weight_1m;
            result.retry_after_s  = rl.retry_after_s;
            ++total_requests_;

            if (result.used_weight_1m >= 0)
                weight_limiter_.sync_from_server(result.used_weight_1m, 60);

            gate_note(result.status_code, static_cast<int>(result.retry_after_s), result.used_weight_1m);  // [FIX-P1-RATEGATE]
            if (result.ok()) break;
            if (result.rate_limited()) { ++rate_limit_hits_; break; }   // sleep/retry YOK: karar RateGate'te
            if (!result.server_error()) break;
        }
        return result;
    }

    // [FIX-P36-RETRY] allow_retry=false => tek deneme (max_retries yok sayilir). RateGate kapisi aynen uygulanir.
    HttpResponse cold_post(const char* url, const char* body, long body_len,
                           bool key_only = false, bool allow_retry = true) {
        HttpResponse result;
        std::string  body_accum;

        CURL* easy = cold_handle();
        if (!easy) { // [FIX-NETWORK-3]: null guard
            result.status_code = -1;
            result.body = "curl_easy_init() failed (OOM or curl not initialized)";
            result.request_sent = false;   // [FIX-P37-C19A] handle yok: hicbir sey gonderilmedi
            return result;
        }
        RespLite rl{};

        curl_easy_setopt(easy, CURLOPT_URL,             url);
        curl_easy_setopt(easy, CURLOPT_POST,            1L);
        curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS,  body);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,   body_len);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,   cold_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA,       &body_accum);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,  header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA,      &rl);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, cfg_.connect_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,      cfg_.request_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL,        1L);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS,      0L);   // [FIX-SHUTDOWN-1]
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, abort_progress_cb);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA,    nullptr);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE,   1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,    CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER,
                         key_only ? hdr_get_auth_ : hdr_post_auth_);
        if (!cfg_.verify_ssl) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        const int max_attempt = allow_retry ? cfg_.max_retries : 0;   // [FIX-P36-RETRY]
        bool any_maybe_sent = false;   // [FIX-P37-C19A] denemeler boyunca: herhangi biri gitmis olabilir mi
        for (int attempt = 0; attempt <= max_attempt; ++attempt) {
            if (attempt > 0) {
                ++total_retries_;
                interruptible_sleep_ms((1L << attempt) * 200L);   // [FIX-SHUTDOWN-2]
                if (g_stop.load(std::memory_order_acquire)) break;   // [FIX-SHUTDOWN-2]
                body_accum.clear(); rl = {};
            }

            // [FIX-P1-RATEGATE] her deneme (retry dahil) allow() kapisindan gecer
            if (!gate_allow(!key_only)) return gate_denied_response();
            const CURLcode rc = curl_easy_perform(easy);
            if (rc != CURLE_OK) {
                result.status_code = -1;
                result.body = std::string("curl: ") + curl_easy_strerror(rc);
                any_maybe_sent = any_maybe_sent || kusak::net::curl_request_maybe_sent(easy);   // [FIX-P37-C19A]
                result.request_sent = any_maybe_sent;
                continue;
            }

            result.request_sent = true;   // [FIX-P37-C19A] HTTP yaniti alindi
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status_code);
            result.body           = std::move(body_accum);
            result.used_weight_1m = rl.used_weight_1m;
            result.retry_after_s  = rl.retry_after_s;
            ++total_requests_;

            if (result.used_weight_1m >= 0)
                weight_limiter_.sync_from_server(result.used_weight_1m, 60);
            if (result.order_count_1s >= 0)
                order_limiter_.sync_from_server(result.order_count_1s, 1);

            gate_note(result.status_code, static_cast<int>(result.retry_after_s), result.used_weight_1m);  // [FIX-P1-RATEGATE]
            if (result.ok()) break;
            if (result.rate_limited()) { ++rate_limit_hits_; break; }   // sleep/retry YOK: karar RateGate'te
            if (!result.server_error()) break;
        }
        return result;
    }

    HttpResponse cold_delete(const char* url) {
        HttpResponse result;
        std::string  body_accum;

        CURL* easy = cold_handle();
        if (!easy) {
            result.status_code = -1;
            result.body = "curl_easy_init() failed (OOM or curl not initialized)";
            return result;
        }
        RespLite rl{};

        curl_easy_setopt(easy, CURLOPT_URL,               url);
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST,      "DELETE");
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,      cold_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA,          &body_accum);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,     header_cb);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA,         &rl);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,  cfg_.connect_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS,         cfg_.request_timeout_ms);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL,           1L);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS,         0L);   // [FIX-SHUTDOWN-1]
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION,   abort_progress_cb);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA,       nullptr);
        curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE,      1L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,       CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER,         hdr_get_auth_);
        if (!cfg_.verify_ssl) {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
            if (attempt > 0) {
                ++total_retries_;
                interruptible_sleep_ms((1L << attempt) * 200L);   // [FIX-SHUTDOWN-2]
                if (g_stop.load(std::memory_order_acquire)) break;   // [FIX-SHUTDOWN-2]
                body_accum.clear(); rl = {};
            }

            // [FIX-P1-RATEGATE] her deneme (retry dahil) allow() kapisindan gecer
            if (!gate_allow(true)) return gate_denied_response();
            const CURLcode rc = curl_easy_perform(easy);
            if (rc != CURLE_OK) {
                result.status_code = -1;
                result.body = std::string("curl: ") + curl_easy_strerror(rc);
                continue;
            }

            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status_code);
            result.body           = std::move(body_accum);
            result.used_weight_1m = rl.used_weight_1m;
            result.retry_after_s  = rl.retry_after_s;
            ++total_requests_;

            if (result.used_weight_1m >= 0)
                weight_limiter_.sync_from_server(result.used_weight_1m, 60);

            gate_note(result.status_code, static_cast<int>(result.retry_after_s), result.used_weight_1m);  // [FIX-P1-RATEGATE]
            if (result.ok()) break;
            if (result.rate_limited()) { ++rate_limit_hits_; break; }   // sleep/retry YOK: karar RateGate'te
            if (!result.server_error()) break;
        }
        return result;
    }

    // ============================================================================
    // §JSON  Minimal string_view scanners — zero nlohmann, zero heap in hot path
    //        Cold-path only (exchangeInfo, balances).
    // ============================================================================

    // Find the value string for "key":"<value>" or "key":<value>
    static std::string_view json_str_value(std::string_view doc,
                                           std::string_view key) noexcept
    {
        // Build search pattern: "key":"
        char pat[128];
        const int pn = std::snprintf(pat, sizeof(pat), "\"%.*s\":\"",
                                     static_cast<int>(key.size()), key.data());
        const auto pos = doc.find(std::string_view(pat, static_cast<size_t>(pn)));
        if (pos == std::string_view::npos) return {};
        const auto vstart = pos + static_cast<size_t>(pn);
        const auto vend   = doc.find('"', vstart);
        if (vend == std::string_view::npos) return {};
        return doc.substr(vstart, vend - vstart);
    }

    static double json_double_value(std::string_view doc,
                                    std::string_view key) noexcept
    {
        auto sv = json_str_value(doc, key);
        if (sv.empty()) return 0.0;
        double v = 0.0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        return v;
    }

    bool parse_exchange_info_fast(const std::string&                body,
                                   kusak::exchange::FilterValidator& validator)
    {
        // Walk "symbols":[...] array — no heap allocation beyond what validator does
        std::string_view doc(body);
        size_t pos = doc.find("\"symbols\":");
        if (pos == std::string_view::npos) return false;

        while (true) {
            const size_t sym_pos = doc.find("\"symbol\":\"", pos);
            if (sym_pos == std::string_view::npos) break;

            const size_t vstart = sym_pos + 10;
            const size_t vend   = doc.find('"', vstart);
            if (vend == std::string_view::npos) break;

            const std::string sym(doc.substr(vstart, vend - vstart));
            kusak::exchange::SymbolFilterCache fc;
            fc.symbol = sym;

            // Scan filters array for this symbol's block
            const size_t block_end = doc.find("\"symbol\":", vend + 1);
            const std::string_view blk = doc.substr(vend, block_end != std::string_view::npos
                                                          ? block_end - vend
                                                          : doc.size() - vend);

            // PRICE_FILTER
            if (blk.find("PRICE_FILTER") != std::string_view::npos) {
                fc.min_price = json_double_value(blk, "minPrice");
                fc.max_price = json_double_value(blk, "maxPrice");
                fc.tick_size = json_double_value(blk, "tickSize");
            }
            // LOT_SIZE
            if (blk.find("LOT_SIZE") != std::string_view::npos) {
                fc.min_qty  = json_double_value(blk, "minQty");
                fc.max_qty  = json_double_value(blk, "maxQty");
                fc.step_size= json_double_value(blk, "stepSize");
            }
            // MIN_NOTIONAL / NOTIONAL
            if (blk.find("NOTIONAL") != std::string_view::npos)
                fc.min_notional = json_double_value(blk, "minNotional");

            // PERCENT_PRICE
            if (blk.find("PERCENT_PRICE") != std::string_view::npos) {
                fc.multiplier_up   = json_double_value(blk, "multiplierUp");
                fc.multiplier_down = json_double_value(blk, "multiplierDown");
            }

            fc.valid = true;
            validator.update_filter(fc);

            pos = (block_end != std::string_view::npos) ? block_end : doc.size();
        }
        return true;
    }

    std::vector<Balance> parse_balances_fast(const std::string& body) {
        std::vector<Balance> result;
        std::string_view doc(body);

        size_t pos = doc.find("\"balances\":");
        if (pos == std::string_view::npos) return result;

        while (true) {
            const size_t asset_pos = doc.find("\"asset\":\"", pos);
            if (asset_pos == std::string_view::npos) break;
            const size_t vstart = asset_pos + 9;
            const size_t vend   = doc.find('"', vstart);
            if (vend == std::string_view::npos) break;

            const std::string_view blk_sv = doc.substr(asset_pos,
                std::min(doc.find("\"asset\":", vend + 1), doc.size()) - asset_pos);

            Balance b;
            b.asset = std::string(doc.substr(vstart, vend - vstart));
            b.free   = json_double_value(blk_sv, "free");
            b.locked = json_double_value(blk_sv, "locked");
            result.push_back(std::move(b));

            pos = vend + 1;
        }
        return result;
    }

    // ============================================================================
    // §UTIL
    // ============================================================================

    static const char* resolve_base_url(const Config& c) noexcept {
        const bool fut = c.market && std::strcmp(c.market, "FUTURES") == 0;
        if (fut) return c.testnet ? BINANCE_FUT_TESTNET_URL : BINANCE_FUT_BASE_URL;
        return c.testnet ? BINANCE_SPOT_TESTNET_URL : BINANCE_SPOT_BASE_URL;
    }

    // ============================================================================
    // §MEMBERS
    // ============================================================================

    Config                               cfg_;
    const char*                          base_url_;

    FastHmac                             hmac_;
    mutable kusak::sign::SignedRequestBuilder signer_;  // for clock-sync + cold-path signed gets

    // Prebuilt curl_slist headers — created once in ctor, reused everywhere
    curl_slist*                          hdr_post_auth_ = nullptr;
    curl_slist*                          hdr_get_auth_  = nullptr;

    // [FIX-NETWORK-4]: Rate limiters — lock-free atomic TokenBucket
    mutable TokenBucket                  weight_limiter_;
    mutable TokenBucket                  order_limiter_;

    // [FIX-P1-RATEGATE] Merkezi IP-ban / backoff / agirlik kapisi (order_guard.hpp)
    mutable std::atomic_flag             gate_lock_;
    mutable kusak::guard::RateGate       gate_;

    // SPSC queue: strategy thread → IO thread
    SpscRing<OrderRequest, 1024>         order_ring_;

    // IO thread
    std::atomic<bool>                    io_running_{false};
    std::thread                          io_thread_;

    // Metrics
    std::atomic<uint64_t>                total_requests_{0};
    std::atomic<uint64_t>                total_retries_{0};
    std::atomic<uint64_t>                rate_limit_hits_{0};
};

} // namespace kusak::net
