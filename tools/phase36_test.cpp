// [PHASE36-TEST] Faz 3.6 kirmizi testleri: C14 (MARKET EXIT yerel filtre), C15 (belirsiz SEND sonucu),
// C16 (SUBMITTED kayip sonuc zaman asimi), C17 (istemci yok / sonuc halkasi dolu).
// Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude -I$HOME/kusak_build/_deps/nlohmann_json-src/include tools/phase36_test.cpp -o ~/kusak_build/tools/phase36_test -lssl -lcrypto -lcurl -lpthread
// Kural: testler DAVRANIS seviyesindedir (istek sayisi, sonuc durumu, slot FSM, kill switch); yeni API adina baglanmaz,
// bu yuzden mevcut kodla derlenir. Beklentiler duzeltmeden ONCE yazildi; sonuca gore DEGISTIRILMEZ.
// Kapsam siniri: ctx.http_client null; ag yok; yalniz bellek ici sahte HttpFunc. Kaynak (include/, src/) degismez.
// Test EDILEMEYENLER (statik kanit olarak kaydedilir): cold_post retry kaldirma (somut curl sinifi),
// C18 startup FLATTEN FUTURES reddi (su_* yollari http_client ister), g_fw_result_lost'un strateji tarafinda okunmasi.
#include <cstdint>
#define main kusak_main
#include "../src/main.cpp"
#undef main

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kusak;
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

// ---------------------------------------------------------------------------
// Programlanabilir sahte borsa. body "@NEW" / "@FILLED" ise yanit JSON'u uretilir; do_throw ise HttpFunc istisna atar.
// ---------------------------------------------------------------------------
struct Call { std::string url; std::string body; };
struct FakeExchange {
    std::mutex mu;
    std::vector<Call> calls;
    long status = 200;
    std::string body = "@NEW";
    bool do_throw = false;
    uint64_t order_id = 777;

    void program(long st, const char* b, bool thr = false, uint64_t oid = 777) {
        std::lock_guard<std::mutex> g(mu);
        calls.clear(); status = st; body = b; do_throw = thr; order_id = oid;
    }
    size_t ncalls() { std::lock_guard<std::mutex> g(mu); return calls.size(); }
    Call first() { std::lock_guard<std::mutex> g(mu); return calls.empty() ? Call{} : calls[0]; }

    static std::string param(const std::string& b, const char* key) {
        const std::string k = std::string(key) + "=";
        size_t p = 0;
        while (true) {
            p = b.find(k, p);
            if (p == std::string::npos) return "";
            if (p == 0 || b[p - 1] == '&') break;
            ++p;
        }
        size_t e = b.find('&', p);
        return b.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
    }
    static bool has_param(const std::string& b, const char* key) {
        const std::string k = std::string(key) + "=";
        return b.rfind(k, 0) == 0 || b.find("&" + k) != std::string::npos;
    }

    exchange::HttpResponse handle(const std::string& url, const std::string& b) {
        std::lock_guard<std::mutex> g(mu);
        calls.push_back({url, b});
        if (do_throw) throw std::runtime_error("sim: transport exception");
        exchange::HttpResponse r;
        r.status_code = status;
        const std::string coid = param(b, "newClientOrderId");
        if (body == "@NEW") {
            r.body = "{\"symbol\":\"BTCUSDT\",\"orderId\":" + std::to_string(order_id) + ",\"clientOrderId\":\"" + coid +
                     "\",\"status\":\"NEW\",\"executedQty\":\"0.00000000\"}";
        } else if (body == "@FILLED") {
            r.body = "{\"symbol\":\"BTCUSDT\",\"orderId\":" + std::to_string(order_id) + ",\"clientOrderId\":\"" + coid +
                     "\",\"status\":\"FILLED\",\"executedQty\":\"0.29000000\",\"price\":\"0.00000000\",\"cummulativeQuoteQty\":\"29.00000000\"}";
        } else {
            r.body = body;
        }
        return r;
    }
};
static FakeExchange g_fake;

// ---------------------------------------------------------------------------
// Yardimcilar (sim_exchange_test kalibi)
// ---------------------------------------------------------------------------
static exchange::SymbolFilterCache mk_filter() {
    exchange::SymbolFilterCache fc; fc.symbol = "BTCUSDT"; fc.tick_size = 0.01; fc.step_size = 0.00001;
    fc.min_qty = 0.00001; fc.max_qty = 9000.0; fc.min_notional = 5.0; fc.min_price = 0.01; fc.max_price = 1e9;
    return fc;
}
static config::KusakConfig mkcfg(const char* market) {
    config::KusakConfig cfg; cfg.symbol = "BTCUSDT"; cfg.dry_run = false; cfg.testnet = true; cfg.market = market;
    cfg.max_position_usd = 30.0; cfg.ks.max_order_notional_usd = 30.0; cfg.initial_capital = 1000.0;
    cfg.write_csv = false; cfg.write_prometheus = false;
    cfg.ks.lock_file = "/tmp/phase36_test_kill.lock";
    return cfg;
}
static std::unique_ptr<StrategyContext> fresh(const config::KusakConfig& c) {
    auto p = std::make_unique<StrategyContext>(c);
    p->filter_validator.update_filter(mk_filter());
    fw_publish_limits(*p);
    p->live_order_client.emplace(p->filter_validator,
        [](const std::string& ep, const std::string& params) { return g_fake.handle(ep, params); });
    p->live_order_client->set_market_endpoints(c.market == "FUTURES");
    p->exit_retry_not_before_ns = 0;
    p->fw_free_quote_usd.store(1000.0);
    return p;
}
static OrderResult mk_res(uint64_t local, OrderStatus st, uint64_t ex, double filled, double avg, bool is_exit, bool is_buy) {
    OrderResult r{}; r.local_order_id = local; r.order_id = local; r.exchange_order_id = ex; r.status = st;
    r.filled_qty = filled; r.avg_price = avg; r.is_exit = is_exit; r.is_buy = is_buy; r.ts_recv_ns = tsc_ns();
    return r;
}
static void mk_entry_slot(StrategyContext& ctx, uint64_t L, double qty, bool is_buy) {
    ctx.fsm_entry_seq = L; ctx.flat_order_map.insert(L, qty);
    for (auto st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(L, st);
    ctx.fsm_key = L; ctx.active_order_id = L; ctx.awaiting_fill = true; ctx.pending_is_buy = is_buy;
}
static OrderTask mk_send(StrategyContext& ctx, uint64_t L, double px, double qty, bool is_buy, const char* coid_txt) {
    OrderTask t{}; t.kind = TaskKind::SEND; t.type = OrderType::LIMIT_MAKER; t.tif = TimeInForce::GTC;
    t.side = is_buy ? OrderSide::BUY : OrderSide::SELL; t.price = px; t.qty = qty;
    t.order_id = L; t.local_order_id = L; t.is_exit = false; t.ts_send_ns = tsc_ns();
    t.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    t.client_oid.set(coid_txt, static_cast<uint8_t>(std::strlen(coid_txt)));
    ctx.active_client_oid = t.client_oid;
    return t;
}
static OrderTask mk_exit_task(StrategyContext& ctx, OrderSide s, double px, double qty, const char* coid_txt) {
    OrderTask t{}; t.kind = TaskKind::EXIT; t.type = OrderType::MARKET; t.tif = TimeInForce::GTC; t.side = s;
    t.price = px; t.qty = qty; t.is_exit = true; t.ts_send_ns = tsc_ns();
    t.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    const uint64_t k = ++ctx.fsm_exit_seq; t.local_order_id = k; t.order_id = 0;
    ctx.flat_order_map.insert(k, qty);
    for (auto st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(k, st);
    ctx.fsm_key = k; ctx.awaiting_fill = true; ctx.pending_is_buy = (s == OrderSide::BUY);
    t.client_oid.set(coid_txt, static_cast<uint8_t>(std::strlen(coid_txt)));
    ctx.active_client_oid = t.client_oid;
    return t;
}
static int slot_fsm(StrategyContext& ctx, uint64_t key) {   // tombstone dahil; yok ise -1
    for (size_t i = 0; i < MAX_ORDERS; ++i) {
        const OrderSlot& s = ctx.flat_order_map.slots[i];
        if (s.state != SlotState::EMPTY && s.order_id == key) return static_cast<int>(s.fsm_state);
    }
    return -1;
}
static const char* sname(int s) { return s < 0 ? "YOK" : order_state_name(static_cast<OrderState>(s)); }
static bool wait_result(StrategyContext& ctx, OrderResult& r, int ms) {
    for (int i = 0; i < ms; ++i) {
        if (ctx.order_channel.pop_result(r)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
struct Runner {
    StrategyContext& ctx; std::thread th;
    explicit Runner(StrategyContext& c) : ctx(c) { th = std::thread([&c] { run_order_thread(c); }); }
    ~Runner() { ctx.order_channel.stop.store(true, std::memory_order_release); if (th.joinable()) th.join(); }
};
static void print_res(const OrderResult& r) {
    std::printf("  sonuc: status=%d exch_id=%llu err='%s'\n", static_cast<int>(r.status),
        (unsigned long long)r.exchange_order_id, r.error_msg.c_str());
}

// ---------------------------------------------------------------------------
// M: RestOrderClient::new_order dogrudan (C14). Yerel filtre reddi = sahte borsaya 0 istek.
// ---------------------------------------------------------------------------
struct MCase { exchange::RestOrderClient::OrderResponse resp; size_t n; Call c; };
static MCase run_new_order(bool futures, const char* type, const char* side, double qty, double price, double cur, bool ro = false) {
    g_fake.program(200, "@FILLED");
    exchange::FilterValidator fv; fv.update_filter(mk_filter());
    exchange::RestOrderClient cli(fv, [](const std::string& ep, const std::string& p) { return g_fake.handle(ep, p); });
    cli.set_market_endpoints(futures);
    exchange::RestOrderClient::OrderRequest req;
    req.symbol = "BTCUSDT"; req.side = side; req.type = type; req.quantity = qty; req.price = price;
    req.client_oid = "KUSAK_P36_M"; req.reduce_only = ro;
    if (std::strcmp(type, "MARKET") != 0) req.time_in_force = "GTC";
    MCase m; m.resp = cli.new_order(req, cur); m.n = g_fake.ncalls(); m.c = g_fake.first();
    std::printf("  new_order: success=%d code=%d http=%ld istek=%zu url=%s body_type=%s price_param=%s qty=%s reduceOnly=%s\n",
        (int)m.resp.success, (int)m.resp.error.code, m.resp.http_status, m.n, m.c.url.c_str(),
        FakeExchange::param(m.c.body, "type").c_str(),
        FakeExchange::has_param(m.c.body, "price") ? FakeExchange::param(m.c.body, "price").c_str() : "(yok)",
        FakeExchange::param(m.c.body, "quantity").c_str(),
        FakeExchange::has_param(m.c.body, "reduceOnly") ? FakeExchange::param(m.c.body, "reduceOnly").c_str() : "(yok)");
    return m;
}

// ---------------------------------------------------------------------------
// U: order thread uzerinden SEND sonucu siniflamasi (C15).
// ---------------------------------------------------------------------------
struct UCase { bool got; OrderResult r; size_t n; std::string sent_coid; int slot; bool ks; bool poll; OrderTask pt; };
static UCase run_send(long st, const char* body, bool thr, const char* coid) {
    g_fake.program(st, body, thr);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    UCase u{};
    {
        Runner run(ctx);
        mk_entry_slot(ctx, 1, 0.29, true);
        OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, coid);
        if (!ctx.order_channel.push_task(t)) { std::printf("  push_task basarisiz\n"); return u; }
        u.got = wait_result(ctx, u.r, 3000);
    }
    u.n = g_fake.ncalls();
    u.sent_coid = FakeExchange::param(g_fake.first().body, "newClientOrderId");
    if (u.got) dispatch_order_result(ctx, u.r);
    u.slot = slot_fsm(ctx, 1);
    u.ks = ctx.kill_switch.load();
    u.poll = build_poll_task(ctx, u.pt);
    print_res(u.r);
    std::printf("  istek=%zu slot=%s kill_switch=%d poll=%d poll.order_id=%llu poll.client_oid='%s'\n", u.n, sname(u.slot),
        (int)u.ks, (int)u.poll, (unsigned long long)u.pt.order_id, u.pt.client_oid.c_str());
    return u;
}
static void expect_ambiguous(const UCase& u) {
    CHECK(u.got);
    CHECK(u.n == 1);
    CHECK(u.r.status == OrderStatus::ERROR);
    CHECK(std::strcmp(u.r.error_msg.c_str(), "NET_UNKNOWN") == 0);
    CHECK(u.slot == static_cast<int>(OrderState::UNKNOWN));
    CHECK(u.ks == true);
    CHECK(u.poll && u.pt.order_id == 0 && std::string(u.pt.client_oid.c_str()) == u.sent_coid);
}
static void expect_definitive(const UCase& u) {
    CHECK(u.got);
    CHECK(u.n == 1);
    CHECK(u.slot != static_cast<int>(OrderState::UNKNOWN));
    CHECK(u.slot == static_cast<int>(OrderState::REJECTED));
}

int main() {
    g_tsc_ns_mult = calibrate_tsc().ns_mult;
    constexpr uint64_t NS = 1'000'000'000ULL;
    constexpr uint64_t W_NS = 40ULL * NS;   // sim_exchange_test I10 ile ayni pencere (plan §2 C16)
    const double NaN = std::numeric_limits<double>::quiet_NaN();

    // ======================= M: C14 MARKET yerel dogrulama =======================
    {
    std::printf("M1 SPOT MARKET SELL qty 0.29 ref 100: borsaya 1 istek, price parametresi YOK\n");
    MCase m = run_new_order(false, "MARKET", "SELL", 0.29, 0.0, 100.0);
    CHECK(m.n == 1);
    CHECK(m.resp.success);
    CHECK(FakeExchange::param(m.c.body, "type") == "MARKET");
    CHECK(!FakeExchange::has_param(m.c.body, "price"));
    }
    {
    std::printf("M2a MARKET ref fiyat 0: fail-closed, 0 istek\n");
    MCase m = run_new_order(false, "MARKET", "SELL", 0.29, 0.0, 0.0);
    CHECK(m.n == 0); CHECK(!m.resp.success);
    std::printf("M2b MARKET ref fiyat NaN: fail-closed, 0 istek\n");
    MCase m2 = run_new_order(false, "MARKET", "SELL", 0.29, 0.0, NaN);
    CHECK(m2.n == 0); CHECK(!m2.resp.success);
    std::printf("M2c MARKET ref fiyat +inf: fail-closed, 0 istek\n");
    MCase m3 = run_new_order(false, "MARKET", "SELL", 0.29, 0.0, std::numeric_limits<double>::infinity());
    CHECK(m3.n == 0); CHECK(!m3.resp.success);
    }
    {
    std::printf("M3a MARKET qty < min_qty: 0 istek\n");
    MCase a = run_new_order(false, "MARKET", "SELL", 0.000001, 0.0, 100.0);
    CHECK(a.n == 0); CHECK(!a.resp.success);
    std::printf("M3b MARKET qty step'e hizasiz (0.290005, step 1e-5): 0 istek\n");
    MCase b = run_new_order(false, "MARKET", "SELL", 0.290005, 0.0, 100.0);
    CHECK(b.n == 0); CHECK(!b.resp.success);
    std::printf("M3c MARKET qty > max_qty: 0 istek\n");
    MCase c = run_new_order(false, "MARKET", "SELL", 9001.0, 0.0, 100.0);
    CHECK(c.n == 0); CHECK(!c.resp.success);
    }
    {
    std::printf("M4 MARKET qty 0.01 x ref 100 = 1 < min_notional 5: NOTIONAL reddi (-1013), 0 istek\n");
    MCase m = run_new_order(false, "MARKET", "SELL", 0.01, 0.0, 100.0);
    CHECK(m.n == 0); CHECK(!m.resp.success);
    CHECK(m.resp.error.code == -1013);
    }
    {
    std::printf("M5a LIMIT regresyon: price 100 qty 0.29 -> 1 istek, price parametresi VAR\n");
    MCase a = run_new_order(false, "LIMIT", "BUY", 0.29, 100.0, 100.0);
    CHECK(a.n == 1); CHECK(FakeExchange::has_param(a.c.body, "price"));
    std::printf("M5b LIMIT regresyon: price tick'e hizasiz (100.005) -> 0 istek\n");
    MCase b = run_new_order(false, "LIMIT", "BUY", 0.29, 100.005, 100.0);
    CHECK(b.n == 0); CHECK(!b.resp.success);
    std::printf("M5c LIMIT regresyon: price 0 -> 0 istek\n");
    MCase c = run_new_order(false, "LIMIT", "BUY", 0.29, 0.0, 100.0);
    CHECK(c.n == 0); CHECK(!c.resp.success);
    std::printf("M5d LIMIT regresyon: notional 0.01 x 100 < 5 -> 0 istek, -1013\n");
    MCase d = run_new_order(false, "LIMIT", "BUY", 0.01, 100.0, 100.0);
    CHECK(d.n == 0); CHECK(d.resp.error.code == -1013);
    }
    {
    std::printf("M6 FUTURES MARKET BUY reduceOnly ref 100: 1 istek, /fapi/v1/order, reduceOnly=true, price YOK\n");
    MCase m = run_new_order(true, "MARKET", "BUY", 0.29, 0.0, 100.0, true);
    CHECK(m.n == 1);
    CHECK(m.c.url == "/fapi/v1/order");
    CHECK(FakeExchange::param(m.c.body, "reduceOnly") == "true");
    CHECK(!FakeExchange::has_param(m.c.body, "price"));
    }

    // ======================= U: C15 belirsiz / kesin sonuc =======================
    std::printf("U1a transport (-1, curl): belirsiz -> NET_UNKNOWN/UNKNOWN/KS/POLL(origClientOrderId)\n");
    expect_ambiguous(run_send(-1, "curl: Timeout was reached", false, "KUSAK_P36_U1A"));
    std::printf("U1b HTTP 500 bos govde: belirsiz\n");
    expect_ambiguous(run_send(500, "", false, "KUSAK_P36_U1B"));
    std::printf("U1c HTTP 503 Binance kodlu govde: 5xx her zaman belirsiz\n");
    expect_ambiguous(run_send(503, "{\"code\":-1001,\"msg\":\"Internal error\"}", false, "KUSAK_P36_U1C"));
    std::printf("U1d HTTP 400 kodu ayristirilamayan govde (WAF/HTML): belirsiz (fail-closed)\n");
    expect_ambiguous(run_send(403, "<html>forbidden</html>", false, "KUSAK_P36_U1D"));
    std::printf("U1e HTTP 200 ama orderId yok: belirsiz\n");
    expect_ambiguous(run_send(200, "{\"status\":\"NEW\"}", false, "KUSAK_P36_U1E"));
    std::printf("U1f HttpFunc istisna atar: belirsiz\n");
    expect_ambiguous(run_send(0, "", true, "KUSAK_P36_U1F"));
    std::printf("U1g Binance -1007 (backend timeout, durum bilinmiyor) [kod anlami DOGRULANMADI]: belirsiz\n");
    expect_ambiguous(run_send(400, "{\"code\":-1007,\"msg\":\"Timeout waiting for response from backend server.\"}", false, "KUSAK_P36_U1G"));
    std::printf("U1h Binance -1006 (beklenmedik yanit, durum bilinmiyor) [kod anlami DOGRULANMADI]: belirsiz\n");
    expect_ambiguous(run_send(400, "{\"code\":-1006,\"msg\":\"An unexpected response was received.\"}", false, "KUSAK_P36_U1H"));
    std::printf("U3 HTTP 400 -2010 (kesin ret): REJECTED, UNKNOWN degil\n");
    expect_definitive(run_send(400, "{\"code\":-2010,\"msg\":\"Account has insufficient balance for requested action.\"}", false, "KUSAK_P36_U3"));
    std::printf("U3b HTTP 429 -1003 (rate limit, islenmedi): REJECTED, UNKNOWN degil\n");
    expect_definitive(run_send(429, "{\"code\":-1003,\"msg\":\"Too many requests.\"}", false, "KUSAK_P36_U3B"));
    {
    std::printf("U5 HTTP 200 NEW (regresyon): PENDING/NEW, UNKNOWN degil, POLL orderId=777\n");
    UCase u = run_send(200, "@NEW", false, "KUSAK_P36_U5");
    CHECK(u.got); CHECK(u.r.exchange_order_id == 777);
    CHECK(u.slot != static_cast<int>(OrderState::UNKNOWN));
    CHECK(u.poll && u.pt.order_id == 777);
    }

    // ======================= T: C16 SUBMITTED kayip sonuc =======================
    auto submitted_case = [&](uint64_t age_ns, uint64_t exch_id, const char* coid, std::unique_ptr<StrategyContext>& out) {
        auto cfg = mkcfg("SPOT"); out = fresh(cfg); StrategyContext& ctx = *out;
        mk_entry_slot(ctx, 1, 0.29, true);
        (void)mk_send(ctx, 1, 100.0, 0.29, true, coid);
        OrderSlot* s = ctx.flat_order_map.find(1);
        if (!s) return;
        s->exchange_order_id = exch_id;
        ctx.cfg.dry_run = true; ctx.mock_time_ns = 200 * NS;   // sim_exchange_test I10 ile ayni saat izolasyonu
        s->last_update_ns = ctx.mock_time_ns - age_ns;
        enforce_timeouts(ctx, ctx.flat_order_map, ctx.order_channel, ctx.cfg.symbol);
        std::printf("  yas=%.1fs exch_id=%llu -> slot=%s kill_switch=%d\n", age_ns / 1e9, (unsigned long long)exch_id,
            sname(slot_fsm(ctx, 1)), (int)ctx.kill_switch.load());
    };
    {
    std::printf("T1 SUBMITTED, yas = W - 1 s: SUBMITTED kalir, kill switch kapali\n");
    std::unique_ptr<StrategyContext> P; submitted_case(W_NS - NS, 0, "KUSAK_P36_T1", P);
    CHECK(slot_fsm(*P, 1) == static_cast<int>(OrderState::SUBMITTED));
    CHECK(P->kill_switch.load() == false);
    }
    {
    std::printf("T2 SUBMITTED, borsa ID != 0, yas > W: SUBMITTED-kayip dali dokunmaz (UNKNOWN olmaz)\n");
    std::unique_ptr<StrategyContext> P; submitted_case(W_NS + NS, 4242, "KUSAK_P36_T2", P);
    CHECK(slot_fsm(*P, 1) != static_cast<int>(OrderState::UNKNOWN));
    }
    {
    std::printf("T3 SUBMITTED, yas > W -> UNKNOWN; ardindan gec gelen FILLED -> pozisyon acik, KS acik kalir\n");
    std::unique_ptr<StrategyContext> P; submitted_case(W_NS + NS, 0, "KUSAK_P36_T3", P);
    StrategyContext& ctx = *P;
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    CHECK(ctx.kill_switch.load() == true);
    ctx.cfg.dry_run = false; ctx.mock_time_ns = 0;
    dispatch_order_result(ctx, mk_res(1, OrderStatus::FILLED, 4243, 0.29, 100.0, false, true));
    std::printf("  gec FILLED sonrasi: slot=%s is_open=%d qty=%.5f kill_switch=%d\n", sname(slot_fsm(ctx, 1)),
        (int)ctx.position.is_open(), ctx.position.qty, (int)ctx.kill_switch.load());
    CHECK(ctx.position.is_open());
    CHECK(std::fabs(ctx.position.qty - 0.29) < 1e-9);
    CHECK(ctx.kill_switch.load() == true);
    }

    // ======================= L: C17 istemci yok / halka dolu =======================
    {
    std::printf("L1 sonuc halkasi dolu iken SEND cevabi: kayip sayaci +1 ve kill switch acik\n");
    g_fake.program(200, "@NEW", false, 778);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    // Halka, order thread baslamadan once test thread'inden doldurulur (thread baslatma senkronizasyon noktasi;
    // calisma sirasinda tek uretici order thread'dir).
    size_t filled = 0;
    { OrderResult dummy{}; dummy.local_order_id = 999999; dummy.status = OrderStatus::CANCELED;
      while (ctx.order_channel.push_result(dummy)) ++filled; }
    std::printf("  halkaya doldurulan sahte sonuc=%zu\n", filled);
    const uint64_t drops0 = g_dropped_results.load();
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_P36_L1");
    {
        Runner run(ctx);
        CHECK(ctx.order_channel.push_task(t));
        for (int i = 0; i < 2000 && !(g_fake.ncalls() == 1 && ctx.kill_switch.load()); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const uint64_t drops1 = g_dropped_results.load();
    std::printf("  istek=%zu dropped_results %llu -> %llu kill_switch=%d result_drops()=%llu\n", g_fake.ncalls(),
        (unsigned long long)drops0, (unsigned long long)drops1, (int)ctx.kill_switch.load(),
        (unsigned long long)ctx.order_channel.result_drops());
    CHECK(g_fake.ncalls() == 1);
    CHECK(drops1 >= drops0 + 1);
    CHECK(ctx.kill_switch.load() == true);
    }
    {
    std::printf("L2 live_order_client bos iken EXIT: <=1 s icinde REJECTED (NO_CLIENT), 0 istek\n");
    g_fake.program(200, "@FILLED");
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    ctx.live_order_client.reset();
    OrderTask t = mk_exit_task(ctx, OrderSide::SELL, 100.0, 0.29, "KUSAK_P36_L2");
    OrderResult r{}; bool got = false;
    {
        Runner run(ctx);
        CHECK(ctx.order_channel.push_task(t));
        got = wait_result(ctx, r, 1000);
    }
    std::printf("  sonuc geldi=%d status=%d err='%s' istek=%zu\n", (int)got, (int)r.status, r.error_msg.c_str(), g_fake.ncalls());
    CHECK(got);
    CHECK(got && r.status == OrderStatus::REJECTED);
    CHECK(got && r.is_exit);
    CHECK(g_fake.ncalls() == 0);
    }
    {
    std::printf("L3 live_order_client bos iken SEND: REJECTED; dispatch sonrasi slot UNKNOWN degil\n");
    g_fake.program(200, "@NEW");
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    ctx.live_order_client.reset();
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_P36_L3");
    OrderResult r{}; bool got = false;
    {
        Runner run(ctx);
        CHECK(ctx.order_channel.push_task(t));
        got = wait_result(ctx, r, 1000);
    }
    CHECK(got);
    CHECK(got && r.status == OrderStatus::REJECTED);
    if (got) dispatch_order_result(ctx, r);
    std::printf("  slot=%s\n", sname(slot_fsm(ctx, 1)));
    CHECK(slot_fsm(ctx, 1) != static_cast<int>(OrderState::UNKNOWN));
    }

    std::remove("/tmp/phase36_test_kill.lock");
    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
