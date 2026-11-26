// Minimal test — no libcurl dependency (ClockSynchronizer network methods
// are not called in this test; only PercentEncoder and HmacSha256 are exercised).
//
// Compile:
//   g++ -std=c++17 -O2 -I/usr/include/node \
//       -o test_signer_minimal test_signer_minimal.cpp \
//       -L/usr/lib/x86_64-linux-gnu -lcrypto -lssl

// Stub out libcurl so we can test without curl dev headers ─────────────────
// We define the minimum curl symbols used inside ClockSynchronizer's
// private http_get_time() method. Because ClockSynchronizer::sync() is
// never called in this test binary (only corrected_timestamp_ms() is used,
// which just reads the atomic offset), the stubs are never executed.
#define KUSAK_NO_CURL_HDR
typedef void CURL;
typedef int  CURLcode;
#define CURLE_OK           0
#define CURLOPT_URL        10002
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA  10001
#define CURLOPT_TIMEOUT_MS 156
#define CURLOPT_CONNECTTIMEOUT_MS 156
#define CURLOPT_NOSIGNAL   99
#define CURLOPT_FOLLOWLOCATION 52
#define CURLINFO_RESPONSE_CODE 0x200002
inline CURL* curl_easy_init()                          { return nullptr; }
inline void  curl_easy_cleanup(CURL*)                  {}
inline CURLcode curl_easy_perform(CURL*)               { return CURLE_OK; }
inline void  curl_easy_setopt(CURL*, int, ...)         {}
inline void  curl_easy_getinfo(CURL*, int, long* out)  { if(out) *out = 200; }
inline void  curl_global_init(long)                    {}
// ────────────────────────────────────────────────────────────────────────────

#include "binance_signer.hpp"

#include <cstdio>
#include <cassert>
#include <string>
#include <chrono>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name) {
    if (cond) { std::printf("[PASS] %s\n", name); ++g_pass; }
    else       { std::printf("[FAIL] %s\n", name); ++g_fail; }
}

// ── PercentEncoder ───────────────────────────────────────────────────────────
static void test_encoder() {
    using E = kusak::sign::PercentEncoder;
    check(E::encode("BTCUSDT")   == "BTCUSDT",  "encode: unreserved passthrough");
    check(E::encode("BTC/USDT") == "BTC%2FUSDT","encode: slash");
    check(E::encode("hello world") == "hello%20world", "encode: space");
    const std::string orig = "[\"BTCUSDT\",\"ETHUSDT\"]";
    check(E::decode(E::encode(orig)) == orig, "encode/decode roundtrip");
    check(E::encode("ABCabc0-._~") == "ABCabc0-._~", "encode: all unreserved");
}

// ── HmacSha256 ───────────────────────────────────────────────────────────────
static void test_hmac() {
    using H = kusak::sign::HmacSha256;
    // Python-verified: hmac.new(b'Jefe', b'what do ya want for nothing?', sha256)
    check(H::compute("Jefe","what do ya want for nothing?")
          == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
          "hmac: RFC test vector");

    // Canonical Binance payload (Python-verified)
    check(H::compute("test_secret_key_32bytes_padded__",
          "symbol=BTCUSDT&side=BUY&type=LIMIT"
          "&quantity=0.00100000&price=30000.00"
          "&timestamp=1700000000000&recvWindow=5000")
          == "36d5baf1acdff31abeddec531bca28269abbc472fb22a7cb5c7553e5bdbfd63b",
          "hmac: Binance canonical payload");
}

// ── RequestSigner structural tests ──────────────────────────────────────────
static bool is_last_param(const std::string& qs, const char* prefix) {
    auto pos = qs.rfind('&');
    std::string last = (pos == std::string::npos) ? qs : qs.substr(pos + 1);
    return last.compare(0, std::strlen(prefix), prefix) == 0;
}

static std::string get_param(const std::string& qs, const char* key) {
    std::string k = std::string(key) + "=";
    auto p = qs.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    auto e = qs.find('&', p);
    return (e == std::string::npos) ? qs.substr(p) : qs.substr(p, e - p);
}

static void test_signer() {
    kusak::sign::ClockSynchronizer clock;  // no sync → offset = 0
    kusak::sign::RequestSigner signer("mysecret", clock);

    // Signature must be last
    auto qs = signer.sign_query("symbol=BTCUSDT&side=BUY", 5000);
    check(is_last_param(qs, "signature="), "signer: signature is last param");

    // Must contain timestamp
    check(qs.find("timestamp=") != std::string::npos, "signer: has timestamp");

    // recvWindow present when > 0
    check(qs.find("recvWindow=5000") != std::string::npos, "signer: has recvWindow");

    // recvWindow absent when = 0
    auto qs0 = signer.sign_query("symbol=BTCUSDT", 0);
    check(qs0.find("recvWindow=") == std::string::npos, "signer: recvWindow=0 omitted");

    // Signature is 64 hex chars
    auto sig = get_param(qs, "signature");
    check(sig.size() == 64, "signer: signature is 64 hex chars");

    // Signature is reproducible (same payload → same sig)
    // We cannot do this with live timestamps, but we can verify sign_body == sign_query
    // produces the same structural layout.
    auto body = signer.sign_body("symbol=BTCUSDT", 5000);
    check(is_last_param(body, "signature="), "signer: sign_body last param");
}

// ── build_param ─────────────────────────────────────────────────────────────
static void test_build_param() {
    using R = kusak::sign::RequestSigner;
    auto p = R::build_param("symbol", "BTCUSDT") +
             R::build_param("side",   "BUY");
    if (!p.empty() && p.back() == '&') p.pop_back();
    check(p.find("symbol=BTCUSDT") != std::string::npos, "build_param: symbol");
    check(p.find("side=BUY")       != std::string::npos, "build_param: side");
    check(p.back() != '&', "build_param: no trailing &");
    // Values with reserved chars are encoded
    auto enc = R::build_param("val", "a/b c");
    check(enc.find("a%2Fb%20c") != std::string::npos, "build_param: encoding");
}

// ── ClockSynchronizer offline ────────────────────────────────────────────────
static void test_clock_offline() {
    kusak::sign::ClockSynchronizer cs;
    auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto ts = cs.corrected_timestamp_ms();
    auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    check(ts >= before && ts <= after + 5, "clock: corrected_ts in range");
    check(!cs.is_synchronised(), "clock: not synced without sync()");
}

int main() {
    std::printf("=== binance_signer.hpp minimal test suite ===\n\n");
    test_encoder();
    test_hmac();
    test_signer();
    test_build_param();
    test_clock_offline();
    std::printf("\nResults: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
