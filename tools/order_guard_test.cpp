// FILE: tools/order_guard_test.cpp -- Faz 0 regresyon: max_position_usd=30, firewall, maruziyet, 429/418, soguk baslangic.
// Borsaya/aga DOKUNMAZ (saf mantik + sahte emir). g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/order_guard_test.cpp
#include "order_guard.hpp"
#include <cstdio>
#include <cmath>
#include <limits>
using namespace kusak::guard;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } else std::printf("ok   : %s\n", #c); } while (0)
struct Lcg { uint64_t s; uint32_t next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return uint32_t(s >> 33); } };

static Limits mk() {
    Limits L{}; L.symbol = "BTCUSDT"; L.f = {0.00001, 0.01, 0.00001, 1000.0, 5.0};
    L.pos_cap_usd = 30.0; L.order_cap_usd = 0.0; L.other_exposure_usd = 0.0; L.free_quote_usd = 100.0; L.kill_switch = false; return L;
}

int main() {
    const double CAP = 30.0;
    // ── 1) Miktar hesabi: yuvarlamayla cap delinmesi ─────────────────────────────
    {
        Lcg r{7}; long naive_breach = 0, new_breach = 0, nonmax = 0, n = 0;
        const double steps[] = {1e-8, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 0.1, 1.0};
        for (int i = 0; i < 400000; ++i) {
            const double step  = steps[r.next() % 8];
            const double price = (0.0005 + (r.next() % 2000000) / 100.0) ;       // 0.0005 .. 20000
            const double want  = 1e6;                                             // Kelly: tavandan buyuk
            const double qnaive = std::round((CAP / price) / step) * step;        // eski round_qty (en yakin)
            if (qnaive * price > CAP) ++naive_breach;
            const double q = cap_qty(price, want, CAP, step);
            if (q * price > CAP) ++new_breach;
            if (q > 0.0 && (q + step) * price <= CAP) ++nonmax;                   // gereksiz eksik dolum
            ++n;
        }
        std::printf("      [T1] n=%ld  naive_round_breach=%ld  cap_qty_breach=%ld  non_maximal=%ld\n", n, naive_breach, new_breach, nonmax);
        CHECK(naive_breach > 0);     // round-to-nearest cap'i GERCEKTEN deler (regresyon kaniti)
        CHECK(new_breach == 0);      // cap_qty hicbir durumda delmez
        CHECK(nonmax == 0);          // ve tavana en yakin adimi doldurur
    }
    {   // T1b: strategy_core.hpp'deki MEVCUT formul: floor_qty(min(q,(cap/px)*(1-1e-12))) -- uretim yolu kilitlenir
        Lcg r{11}; long breach = 0, under = 0;
        const double steps[] = {1e-8, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 0.1, 1.0};
        for (int i = 0; i < 400000; ++i) {
            const double step = steps[r.next() % 8], price = 0.0005 + (r.next() % 2000000) / 100.0;
            double q = std::min(1e6, (CAP / price) * (1.0 - 1e-12));
            q = double(int64_t(std::floor(q / step))) * step;
            if (q * price > CAP) ++breach;
            if (q > 0.0 && (q + step) * price <= CAP) ++under;
        }
        std::printf("      [T1b] mevcut formul: breach=%ld under_filled=%ld\n", breach, under);
        CHECK(breach == 0);
    }
    {   // Testnet'te gozlenen notional'lar (29.9988 / 29.9364) cap altinda, tam esik PASS
        const double step = 0.001, price = 29.999;
        const double q = cap_qty(price, 5.0, CAP, step);
        CHECK(q * price <= CAP);
        Limits L = mk(); L.f = {step, 0.001, step, 1e9, 5.0};
        Order ok{"BTCUSDT", true, q, price};   CHECK(final_check(ok, L) == Verdict::PASS);
    }

    // ── 2) Yuvarlama sonrasi 30'u asarsa emir NASIL reddedilir ───────────────────
    {
        Limits L = mk(); const double step = 0.00001, price = 95000.0;
        const double q  = cap_qty(price, 1.0, CAP, step);                     // 0.00031578 -> 29.9991
        Order good{"BTCUSDT", true, q, price};            CHECK(final_check(good, L) == Verdict::PASS);
        Order up  {"BTCUSDT", true, q + step, price};     CHECK(up.qty * up.price > CAP);
        CHECK(final_check(up, L) == Verdict::POSITION_CAP);                   // 1 adim yukari yuvarlanmis emir => RED
        Order exact{"BTCUSDT", true, 30.0 / 100.0, 100.0}; Limits L2 = mk(); L2.f = {0.01, 0.01, 0.01, 1e9, 5.0};
        CHECK(final_check(exact, L2) == Verdict::PASS);                       // tam 30.0 gecer
        Order over{"BTCUSDT", true, 0.30000001, 100.0};   L2.f.step = 1e-8;
        CHECK(final_check(over, L2) == Verdict::POSITION_CAP);                // 30.000001 reddedilir
        Limits L3 = mk(); L3.pos_cap_usd = 0.0;           CHECK(final_check(good, L3) == Verdict::POSITION_CAP);   // limit yok = fail-closed
        Limits L4 = mk(); L4.pos_cap_usd = std::numeric_limits<double>::quiet_NaN();
        CHECK(final_check(good, L4) == Verdict::POSITION_CAP);
    }

    // ── 3) final_order_firewall kapsami: sembol, bakiye, filtre, maruziyet ───────
    {
        Limits L = mk(); const double q = 0.0003, p = 95000.0;                // 28.5 USDT
        Order o{"BTCUSDT", true, q, p};                   CHECK(final_check(o, L) == Verdict::PASS);
        Order bad_sym{"ETHUSDT", true, q, p};             CHECK(final_check(bad_sym, L) == Verdict::SYMBOL_MISMATCH);
        Order nul{nullptr, true, q, p};                   CHECK(final_check(nul, L) == Verdict::SYMBOL_MISMATCH);
        Order nanq{"BTCUSDT", true, std::nan(""), p};     CHECK(final_check(nanq, L) == Verdict::BAD_INPUT);
        Order negq{"BTCUSDT", true, -1.0, p};             CHECK(final_check(negq, L) == Verdict::BAD_INPUT);
        Order infp{"BTCUSDT", true, q, INFINITY};         CHECK(final_check(infp, L) == Verdict::BAD_INPUT);
        Order stepf{"BTCUSDT", true, 0.000305, p};        CHECK(final_check(stepf, L) == Verdict::QTY_STEP);
        Order tickf{"BTCUSDT", true, q, 95000.005};       CHECK(final_check(tickf, L) == Verdict::PRICE_TICK);
        Order tiny{"BTCUSDT", true, 0.00001, p};          CHECK(final_check(tiny, L) == Verdict::NOTIONAL_MIN);
        Limits Lb = mk(); Lb.free_quote_usd = 10.0;       CHECK(final_check(o, Lb) == Verdict::INSUFFICIENT_BALANCE);
        Order sell{"BTCUSDT", false, q, p};               CHECK(final_check(sell, Lb) == Verdict::PASS);       // SELL quote bakiyesine bakmaz
        Limits Le = mk(); Le.other_exposure_usd = 10.0;   CHECK(final_check(o, Le) == Verdict::EXPOSURE_CAP);  // 10 acik + 28.5 > 30
        Limits Lk = mk(); Lk.kill_switch = true;          CHECK(final_check(o, Lk) == Verdict::KILL_SWITCH);
        Limits Lo = mk(); Lo.order_cap_usd = 20.0;        CHECK(final_check(o, Lo) == Verdict::ORDER_CAP);
    }

    // ── 4) Toplam maruziyet: max() / (pos+locked) / (pos+pending) ────────────────
    {
        // (a) Bekleyen (dolmamis) canli emir: pos=0, locked=0 (gonderimde eklenmez). Eski max() => 0 (KOR).
        const double pend = pending_remainder(true, 29.99, 0.0);
        CHECK(std::max(0.0, 0.0) == 0.0);                                    // eski formul: maruziyet 0 goruyor
        CHECK(total_exposure(0.0, pend) == 29.99);                           // yeni: 29.99
        // (b) Dolu pozisyon: locked_in_orders = giris maliyeti = pos_usd'nin kopyasi. Naif toplam CIFT SAYAR.
        const double pos = 29.99, locked = 29.99;
        CHECK(pos + locked > CAP);                                           // naif (pos+locked) = 59.98 => her dolumda SAHTE kill-switch
        CHECK(total_exposure(pos, pending_remainder(false, 29.99, 29.99)) <= CAP);   // dogru: 29.99
        // (c) Kismi dolum: 10 doldu, 20 bekliyor => 30 (limitte, tetiklemez); 31 => tetikler
        CHECK(total_exposure(10.0, pending_remainder(true, 30.0, 10.0)) == 30.0);
        CHECK(total_exposure(10.0, pending_remainder(true, 31.0, 10.0)) > CAP);
        // (d) EXIT bekleyen emir maruziyeti artirmaz
        CHECK(pending_remainder(false, 30.0, 0.0) == 0.0);
        CHECK(total_exposure(-5.0, -1.0) == 0.0);                            // negatif girdi sifira kenetlenir
    }

    // ── 5) 429 / 418 ─────────────────────────────────────────────────────────────
    {
        RateGate g; int64_t t = 1'000'000;
        CHECK(g.allow(t, true) && g.allow(t, false));
        g.on_response(429, 0, 900, t);                                       // Retry-After yok => ussel (2s)
        CHECK(g.banned(t + 1999) && !g.banned(t + 2001));
        g.on_response(429, 0, 900, t + 2001);                                // ikinci vurus => 4s
        CHECK(g.wait_ms(t + 2001) == 4000);
        g.on_response(200, -1, 100, t + 7000); CHECK(g.strikes == 0);        // basari => sayac sifirlanir
        RateGate b; b.on_response(418, 5, 0, t);                             // Retry-After=5s olsa da >= 120s
        CHECK(b.wait_ms(t) == 120'000 && !b.allow(t + 60'000, true));
        RateGate w; w.on_response(200, -1, 1000, t);                         // 1000/1200 > %80 => okuma durur, emir durmaz
        CHECK(!w.allow(t + 1, false) && w.allow(t + 1, true));
        CHECK(w.allow(t + 61'000, false));                                   // dakika dolunca agirlik sifirlanir
    }

    // ── 6) Soguk baslangic karari ────────────────────────────────────────────────
    {
        // [PHASE0-GUARD] StartupProbe tabanli kapi: LOCK -> ORDER GATE -> BALANCE GATE
        auto make_probe = [](bool lock, int open_orders, double base, double min_notional, double pos_cap) {
            StartupProbe p{};
            p.lock_file_exists = lock; p.open_orders_verified = true; p.open_orders = open_orders;
            p.cancel_attempts = 0; p.balance_verified = true; p.base_value_usd = base;
            p.min_notional = min_notional; p.pos_cap_usd = pos_cap;
            return p;
        };
        CHECK(startup_decision(make_probe(true,  0, 0.0, 5.0, 30.0))  == StartAction::REFUSE_LOCKED);
        CHECK(startup_decision(make_probe(false, 2, 0.0, 5.0, 30.0))  == StartAction::CANCEL_OPEN_ORDERS_THEN_RECHECK);
        CHECK(startup_decision(make_probe(true,  2, 99.0, 5.0, 30.0)) == StartAction::REFUSE_LOCKED);   // kilit yetim emirden once gelir
        CHECK(startup_decision(make_probe(false, 0, 0.0, 5.0, 30.0))  == StartAction::CLEAN_START);
        CHECK(startup_decision(make_probe(false, 0, 2.0, 5.0, 30.0))  == StartAction::IGNORE_DUST);
        CHECK(startup_decision(make_probe(false, 0, 25.0, 5.0, 30.0)) == StartAction::ADOPT_POSITION);
        CHECK(startup_decision(make_probe(false, 0, 80.0, 5.0, 30.0)) == StartAction::FLATTEN_REDUCE_ONLY);
        CHECK(startup_decision(make_probe(false, 0, 25.0, 5.0, 0.0))  == StartAction::FLATTEN_REDUCE_ONLY);   // limit yok => fail-closed

        // [PHASE0-GUARD] ORDER GATE: open-orders dogrulanmadi => REFUSE_ORDERS_UNVERIFIED
        { StartupProbe p = make_probe(false, 0, 0.0, 5.0, 30.0); p.open_orders_verified = false;
          CHECK(startup_decision(p) == StartAction::REFUSE_ORDERS_UNVERIFIED); }
        CHECK(startup_decision(make_probe(false, -1, 0.0, 5.0, 30.0)) == StartAction::REFUSE_ORDERS_UNVERIFIED);
        { StartupProbe p{}; p.lock_file_exists = false;
          CHECK(startup_decision(p) == StartAction::REFUSE_ORDERS_UNVERIFIED); }

        // [PHASE0-GUARD] Iptal denemesi siniri (MAX_STARTUP_CANCEL_ATTEMPTS=3)
        { StartupProbe p = make_probe(false, 1, 0.0, 5.0, 30.0);
          p.cancel_attempts = 3; CHECK(startup_decision(p) == StartAction::REFUSE_CANCEL_STUCK);
          p.cancel_attempts = 4; CHECK(startup_decision(p) == StartAction::REFUSE_CANCEL_STUCK);
          p.cancel_attempts = 2; CHECK(startup_decision(p) == StartAction::CANCEL_OPEN_ORDERS_THEN_RECHECK); }

        // [PHASE0-GUARD] BALANCE GATE: dogrulanmamis/gecersiz bakiye => REFUSE_BALANCE_UNVERIFIED
        { StartupProbe p = make_probe(false, 0, 0.0, 5.0, 30.0); p.balance_verified = false;
          CHECK(startup_decision(p) == StartAction::REFUSE_BALANCE_UNVERIFIED); }
        CHECK(startup_decision(make_probe(false, 0, std::numeric_limits<double>::quiet_NaN(), 5.0, 30.0)) == StartAction::REFUSE_BALANCE_UNVERIFIED);
        CHECK(startup_decision(make_probe(false, 0, std::numeric_limits<double>::infinity(), 5.0, 30.0))  == StartAction::REFUSE_BALANCE_UNVERIFIED);
        CHECK(startup_decision(make_probe(false, 0, -1.0, 5.0, 30.0)) == StartAction::REFUSE_BALANCE_UNVERIFIED);

        // [PHASE0-GUARD] Tamamen varsayilan probe => fail-closed (kilitli)
        CHECK(startup_decision(StartupProbe{}) == StartAction::REFUSE_LOCKED);

        // [PHASE0-GUARD] Sinirlar: base==min_notional ve base==pos_cap dahil => ADOPT_POSITION
        CHECK(startup_decision(make_probe(false, 0, 5.0, 5.0, 30.0))  == StartAction::ADOPT_POSITION);
        CHECK(startup_decision(make_probe(false, 0, 30.0, 5.0, 30.0)) == StartAction::ADOPT_POSITION);
    }
    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
