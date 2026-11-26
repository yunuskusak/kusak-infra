// FILE: tools/tsc_overflow_test.cpp -- [P0-01-TEST] KZ-P0-01 kirmizi test: tsc_ns() 64-bit carpim tasmasi (~4.29 s'de basa sarma).
// g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/tsc_overflow_test.cpp -o tsc_overflow_test
// Borsaya/aga DOKUNMAZ. Mevcut kodda FAIL etmesi BEKLENIR; esikler sabittir.
#include "infra/time.hpp"
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <vector>

// [P0-01-TEST] time.hpp'nin extern bekledigi tanimlar (normalde main.cpp'de)
uint64_t g_tsc_ns_mult = 0;
std::atomic<bool> g_stop{false};

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else std::printf("ok   : %s\n", #c); } while (0)

static uint64_t mono_raw_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

struct Sample { uint64_t t; uint64_t mono; };

int main() {
    const TscCalibration cal = calibrate_tsc();
    g_tsc_ns_mult = cal.ns_mult;
    const uint64_t mult = g_tsc_ns_mult;

    // ── T1: kalibrasyon makul mu ─────────────────────────────────────────────
    std::printf("      [T1] mult=%llu tsc_per_ns=%llu\n", (unsigned long long)mult, (unsigned long long)cal.tsc_per_ns);
    CHECK(mult > 0);
    CHECK(mult >= (1ULL << 32) / 10 && mult <= (1ULL << 32) * 2);
    if (mult == 0) { std::printf("SONUC fails=%d\n", fails); return 1; }

    // ── Pencere basi referans (T4 icin) ──────────────────────────────────────
    unsigned aux = 0;
    const uint64_t raw0 = __rdtscp(&aux);
    const uint64_t ref = (uint64_t)(((unsigned __int128)raw0 * mult) >> 32);
    const unsigned __int128 thr = ((unsigned __int128)1 << 64) / mult;   // tasma esigi (tick)

    // ── T2: ~6 s boyunca ~1 ms aralikla ornekle ─────────────────────────────
    std::vector<Sample> s;
    s.reserve(8000);
    const uint64_t w0 = mono_raw_ns();
    const uint64_t WIN_NS = 6'000'000'000ULL;
    while (true) {
        const uint64_t t = tsc_ns();
        const uint64_t m = mono_raw_ns();
        s.push_back({t, m});
        if (m - w0 >= WIN_NS) break;
        struct timespec rq = {0, 1'000'000};
        nanosleep(&rq, nullptr);
    }

    uint64_t mn = s[0].t, mx = s[0].t;
    long back = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].t < mn) mn = s[i].t;
        if (s[i].t > mx) mx = s[i].t;
        if (i > 0 && s[i].t < s[i - 1].t) {
            if (back == 0)
                std::printf("      [T2] ilk geri sicrama: idx=%zu onceki=%llu sonraki=%llu pencere_ms=%.3f\n",
                            i, (unsigned long long)s[i - 1].t, (unsigned long long)s[i].t,
                            double(s[i].mono - w0) / 1e6);
            ++back;
        }
    }
    std::printf("      [T2] ornek=%zu geri_sicrama=%ld\n", s.size(), back);
    CHECK(back == 0);

    // ── T3: hiz uyumu (signed) ───────────────────────────────────────────────
    {
        const int64_t dtsc  = (int64_t)(s.back().t - s.front().t);
        const int64_t dmono = (int64_t)(s.back().mono - s.front().mono);
        const double diff = double(dtsc - dmono);
        const double tol  = 2'000'000.0 + double(dmono) * 0.005;
        std::printf("      [T3] dtsc_ns=%lld dmono_ns=%lld fark=%.0f tol=%.0f\n",
                    (long long)dtsc, (long long)dmono, diff, tol);
        CHECK((diff < 0 ? -diff : diff) <= tol);
    }

    // ── T4: ham TSC buyuk ise tsc_ns() 2^32 ns'yi gecmeli ───────────────────
    if (ref >= (1ULL << 32)) {
        CHECK(mx >= (1ULL << 32));
    } else {
        std::printf("T4 SKIP (ref<2^32)\n");
    }

    // ── Teshis ───────────────────────────────────────────────────────────────
    std::printf("      [DIAG] raw_tsc=%llu mult=%llu tasma_esigi(2^64/mult)=%llu oran=%.4f ref_s=%.6f min=%llu max=%llu ornek=%zu\n",
                (unsigned long long)raw0, (unsigned long long)mult, (unsigned long long)thr,
                double(raw0) / double(thr), double(ref) / 1e9,
                (unsigned long long)mn, (unsigned long long)mx, s.size());

    // ── [FIX-P0-01-TEST] T5-T8: sabit girdili testler (donanimdan bagimsiz) ─
    // Beklenen degerler tsc_to_ns formulunden bagimsiz hesaplanir.
    {
        const uint64_t ks[] = {32, 33, 40, 56};

        // T5: birim mult (2^32) -> sonuc girdiye esit olmali
        const uint64_t m1 = 1ULL << 32;
        for (uint64_t k : ks) {
            const uint64_t x = 1ULL << k;
            CHECK(tsc_to_ns(x, m1) == x);
        }

        // T6: gercekci mult
        const uint64_t m6 = 1430000000ULL;
        for (uint64_t k : ks) {
            const uint64_t x = 1ULL << k;
            CHECK(tsc_to_ns(x, m6) == (m6 << (k - 32)));
        }
        {
            bool all_ok = true;
            for (uint64_t k = 0; k <= 31; ++k) {
                if (tsc_to_ns(1ULL << k, m6) != (m6 >> (32 - k))) all_ok = false;
            }
            CHECK(all_ok);
        }

        // T7: uzun uptime (3 GHz, m3 = 2^32/3)
        {
            const uint64_t m3 = (1ULL << 32) / 3;   // 1431655765
            const uint64_t days[] = {30, 365};
            const long double tols[] = {1'000'000.0L, 10'000'000.0L};
            for (int i = 0; i < 2; ++i) {
                const uint64_t tsc = 3'000'000'000ULL * 86400ULL * days[i];
                const long double expect = (long double)days[i] * 86400.0L * 1e9L;
                const long double got = (long double)tsc_to_ns(tsc, m3);
                long double d = got - expect;
                if (d < 0) d = -d;
                std::printf("      [T7] gun=%llu fark_ns=%.0Lf\n", (unsigned long long)days[i], d);
                CHECK(d <= tols[i]);
            }
        }

        // T8: eski tasma siniri civari
        {
            const uint64_t ms[] = {1864063991ULL, 1430000000ULL};
            for (uint64_t m : ms) {
                const uint64_t b = (uint64_t)((((unsigned __int128)1) << 64) / m);
                const uint64_t t1 = b - 1000, t2 = b + 1000;
                const uint64_t r1 = tsc_to_ns(t1, m), r2 = tsc_to_ns(t2, m);
                const uint64_t exp_diff = (2000ULL * m) >> 32;
                const int64_t fd = (int64_t)(r2 - r1) - (int64_t)exp_diff;
                CHECK(r2 > r1);
                CHECK(fd >= -1 && fd <= 1);
                const uint64_t a = tsc_to_ns(b - 1, m), c = tsc_to_ns(b, m), d = tsc_to_ns(b + 1, m);
                CHECK(a <= c && c <= d);
            }
        }
    }

    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
