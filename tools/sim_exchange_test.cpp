// [SIM-TEST] Sahte borsa entegrasyon testi (gercek run_order_thread + gercek RestOrderClient + bellek ici FakeExchange).
// Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude -I$HOME/kusak_build/_deps/nlohmann_json-src/include tools/sim_exchange_test.cpp -o ~/kusak_build/tools/sim_exchange_test -lssl -lcrypto -lcurl -lpthread
// Amac: SEND/EXIT/POLL kimligi, hata esleme, firewall/KS kapisi ve SUBMITTED zaman asimi davranisini order thread uzerinden olcmek.
// Beklenti: I10 ve I11 mevcut kodda FAIL (bilinen aciklar); digerleri olculen davranisi kaydeder. Beklentiler sonuca gore DEGISTIRILMEZ.
// Kapsam siniri: ctx.http_client null; ag yok; yalniz bellek ici sahte HttpFunc. Kaynak (include/, src/) degismez.
#include <cstdint>
#define main kusak_main
#include "../src/main.cpp"
#undef main

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace kusak;
static int fails = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("  FAIL line %d: %s\n",__LINE__,#c); ++fails;} else std::printf("  ok: %s\n",#c);}while(0)

// ---------------------------------------------------------------------------
// FakeExchange: mutex korumali; her cagriyi kaydeder; programlanmis yanit doner.
// HttpFunc imzasi: HttpResponse(const std::string& endpoint, const std::string& params)
// Transport hatasi: uretimdeki cold_post ile ayni bicimde status_code=-1, body="curl: ..." doner.
// ---------------------------------------------------------------------------
enum class Mode { NEW_ORD, FILLED, ERR_2013, TRANSPORT };
struct Call { std::string url; std::string body; };
struct FakeExchange {
    std::mutex mu;
    std::vector<Call> calls;            // sahne bazli (reset ile temizlenir)
    std::vector<std::string> all_urls;  // tum sahneler boyunca
    Mode mode = Mode::NEW_ORD;
    uint64_t order_id = 777;
    std::string exec_qty = "0";
    std::string avg_px = "0";
    bool futures_style = false;         // true: avgPrice alani; false: SPOT tarzi price="0.00000000"

    void reset(Mode m, uint64_t oid, const char* qty, const char* px, bool fut) {
        std::lock_guard<std::mutex> g(mu);
        calls.clear(); mode = m; order_id = oid; exec_qty = qty; avg_px = px; futures_style = fut;
    }
    size_t ncalls() { std::lock_guard<std::mutex> g(mu); return calls.size(); }
    Call first() { std::lock_guard<std::mutex> g(mu); return calls.empty() ? Call{} : calls[0]; }

    static std::string param(const std::string& body, const char* key) {
        const std::string k = std::string(key) + "=";
        size_t p = 0;
        while (true) {
            p = body.find(k, p);
            if (p == std::string::npos) return "";
            if (p == 0 || body[p - 1] == '&') break;
            ++p;
        }
        size_t e = body.find('&', p);
        return body.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
    }

    exchange::HttpResponse handle(const std::string& url, const std::string& body) {
        std::lock_guard<std::mutex> g(mu);
        calls.push_back({url, body});
        all_urls.push_back(url);
        exchange::HttpResponse r;
        const std::string coid = param(body, "newClientOrderId");
        switch (mode) {
        case Mode::TRANSPORT:
            r.status_code = -1; r.body = "curl: Timeout was reached"; return r;
        case Mode::ERR_2013:
            r.status_code = 400; r.body = "{\"code\":-2013,\"msg\":\"Order does not exist.\"}"; return r;
        case Mode::NEW_ORD:
            r.status_code = 200;
            r.body = "{\"symbol\":\"BTCUSDT\",\"orderId\":" + std::to_string(order_id) +
                     ",\"clientOrderId\":\"" + coid + "\",\"status\":\"NEW\",\"executedQty\":\"0.00000000\"}";
            return r;
        case Mode::FILLED:
            r.status_code = 200;
            r.body = "{\"symbol\":\"BTCUSDT\",\"orderId\":" + std::to_string(order_id) +
                     ",\"clientOrderId\":\"" + coid + "\",\"status\":\"FILLED\",\"executedQty\":\"" + exec_qty + "\"," +
                     (futures_style ? "\"avgPrice\":\"" + avg_px + "\"}"
                                    : "\"price\":\"0.00000000\",\"cummulativeQuoteQty\":\"" + avg_px + "\"}");
            return r;
        }
        return r;
    }
};
static FakeExchange g_fake;

// ---------------------------------------------------------------------------
// Yardimcilar (exit_firewall_test / phase35_test kalibi)
// ---------------------------------------------------------------------------
static const char* vname(FwVerdict v) {
    switch (v) {
        case FwVerdict::PASS: return "PASS"; case FwVerdict::BAD_INPUT: return "BAD_INPUT";
        case FwVerdict::POSITION_CAP: return "POSITION_CAP"; case FwVerdict::ORDER_CAP: return "ORDER_CAP";
        case FwVerdict::SYMBOL_MISMATCH: return "SYMBOL_MISMATCH"; case FwVerdict::FILTER: return "FILTER";
        case FwVerdict::EXPOSURE_CAP: return "EXPOSURE_CAP"; case FwVerdict::INSUFFICIENT_BALANCE: return "INSUFFICIENT_BALANCE";
        case FwVerdict::EXIT_POS_UNKNOWN: return "EXIT_POS_UNKNOWN"; case FwVerdict::EXIT_NOT_REDUCING: return "EXIT_NOT_REDUCING";
    }
    return "?";
}
static config::KusakConfig mkcfg(const char* market) {
    config::KusakConfig cfg; cfg.symbol = "BTCUSDT"; cfg.dry_run = false; cfg.testnet = true; cfg.market = market;
    cfg.max_position_usd = 30.0; cfg.ks.max_order_notional_usd = 30.0; cfg.initial_capital = 1000.0;
    cfg.write_csv = false; cfg.write_prometheus = false;
    cfg.ks.lock_file = "/tmp/sim_exchange_test_kill.lock";
    return cfg;
}
static std::unique_ptr<StrategyContext> fresh(const config::KusakConfig& c) {
    auto p = std::make_unique<StrategyContext>(c);
    std::memset(static_cast<void*>(p->flat_order_map.slots), 0, sizeof(p->flat_order_map.slots));
    exchange::SymbolFilterCache fc; fc.symbol = c.symbol; fc.tick_size = 0.01; fc.step_size = 0.00001;
    fc.min_qty = 0.00001; fc.max_qty = 9000.0; fc.min_notional = 5.0; fc.min_price = 0.01; fc.max_price = 1e9;
    p->filter_validator.update_filter(fc);
    fw_publish_limits(*p);
    // Yalniz bellek ici sahte HttpFunc; ctx.http_client NULL kalir.
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
static void open_real(StrategyContext& ctx, bool is_buy, double px, double qty, uint64_t L = 1, uint64_t ex = 9001) {
    mk_entry_slot(ctx, L, qty, is_buy);
    dispatch_order_result(ctx, mk_res(L, OrderStatus::FILLED, ex, qty, px, false, is_buy));
}
// Giris SEND task'i: maybe_send_order cok kapili oldugundan elle kurulur (production alanlariyla ayni).
static OrderTask mk_send(StrategyContext& ctx, uint64_t L, double px, double qty, bool is_buy, const char* coid_txt) {
    OrderTask t{}; t.kind = TaskKind::SEND; t.type = OrderType::LIMIT_MAKER; t.tif = TimeInForce::GTC;
    t.side = is_buy ? OrderSide::BUY : OrderSide::SELL; t.price = px; t.qty = qty;
    t.order_id = L; t.local_order_id = L; t.is_exit = false; t.ts_send_ns = tsc_ns();
    t.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
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
struct Runner {   // order thread yasam dongusu
    StrategyContext& ctx; std::thread th;
    explicit Runner(StrategyContext& c) : ctx(c) { th = std::thread([&c] { run_order_thread(c); }); }
    ~Runner() { ctx.order_channel.stop.store(true, std::memory_order_release); if (th.joinable()) th.join(); }
};
static void print_res(const OrderResult& r) {
    std::printf("  sonuc: status=%d exch_id=%llu filled=%.5f avg=%.4f err='%s'\n", static_cast<int>(r.status),
        (unsigned long long)r.exchange_order_id, r.filled_qty, r.avg_price, r.error_msg.c_str());
}
static void print_body(const char* tag, const Call& c) {   // yalniz guvenli alanlar
    std::printf("  %s url=%s side=%s type=%s qty=%s reduceOnly=%s newClientOrderId=%s\n", tag, c.url.c_str(),
        FakeExchange::param(c.body, "side").c_str(), FakeExchange::param(c.body, "type").c_str(),
        FakeExchange::param(c.body, "quantity").c_str(),
        FakeExchange::param(c.body, "reduceOnly").empty() ? "(yok)" : FakeExchange::param(c.body, "reduceOnly").c_str(),
        FakeExchange::param(c.body, "newClientOrderId").c_str());
}
static OrderTask mk_exit_task(StrategyContext& ctx, OrderSide s, double px, double qty) {
    OrderTask t{}; t.kind = TaskKind::EXIT; t.type = OrderType::MARKET; t.tif = TimeInForce::GTC; t.side = s;
    t.price = px; t.qty = qty; t.is_exit = true; t.ts_send_ns = tsc_ns();
    t.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    const uint64_t k = ++ctx.fsm_exit_seq; t.local_order_id = k; t.order_id = 0;
    ctx.flat_order_map.insert(k, qty);
    for (auto st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(k, st);
    ctx.fsm_key = k; ctx.awaiting_fill = true; ctx.pending_is_buy = (s == OrderSide::BUY);
    const char* c = "KUSAK_EXIT_SIM"; t.client_oid.set(c, static_cast<uint8_t>(std::strlen(c)));
    ctx.active_client_oid = t.client_oid;
    return t;
}

int main() {
    g_tsc_ns_mult = calibrate_tsc().ns_mult;
    constexpr uint64_t NS = 1'000'000'000ULL;
    // I10 penceresi W: cold_post: request_timeout 8000 ms, max_retries=3 -> 4 deneme (curl hatasi/5xx'te yeniden dener),
    // aralarda (2^1+2^2+2^3)*200 ms = 2.8 s backoff => en kotu 4*8 + 2.8 = 34.8 s; +5 s pay => 39.8 s -> 40 s.
    constexpr uint64_t W_NS = 40ULL * NS;

    {   // ---------------- I1 ----------------
    std::printf("I1 SEND -> NEW(777): newClientOrderId=active_client_oid; exchange_order_id==777; POLL order_id==777\n");
    g_fake.reset(Mode::NEW_ORD, 777, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    Runner run(ctx);
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_SIM_I1");
    CHECK(ctx.order_channel.push_task(t));
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r); print_body("fake", g_fake.first());
    CHECK(g_fake.ncalls() == 1);
    CHECK(FakeExchange::param(g_fake.first().body, "newClientOrderId") == std::string(ctx.active_client_oid.c_str()));
    CHECK(r.exchange_order_id == 777);
    // I9 (SEND)
    CHECK(r.ts_recv_ns >= t.ts_send_ns); CHECK(r.ts_recv_ns - t.ts_send_ns < 10 * NS);
    dispatch_order_result(ctx, r);
    OrderTask pt{}; const bool b = build_poll_task(ctx, pt);
    std::printf("  slot=%s poll_task=%d poll.order_id=%llu\n", sname(slot_fsm(ctx, 1)), (int)b, (unsigned long long)pt.order_id);
    CHECK(b); CHECK(pt.order_id == 777);
    }
    {   // ---------------- I2 ----------------
    std::printf("I2 SEND -> transport hatasi: ERROR NET_UNKNOWN; slot UNKNOWN; kill_switch; POLL order_id==0 + client_oid\n");
    g_fake.reset(Mode::TRANSPORT, 0, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    Runner run(ctx);
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_SIM_I2");
    CHECK(ctx.order_channel.push_task(t));
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r); print_body("fake", g_fake.first());
    CHECK(r.status == OrderStatus::ERROR);
    CHECK(std::strcmp(r.error_msg.c_str(), "NET_UNKNOWN") == 0);
    dispatch_order_result(ctx, r);
    std::printf("  slot=%s kill_switch=%d\n", sname(slot_fsm(ctx, 1)), (int)ctx.kill_switch.load());
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    CHECK(ctx.kill_switch.load() == true);
    OrderTask pt{}; const bool b = build_poll_task(ctx, pt);
    std::printf("  poll_task=%d poll.order_id=%llu poll.client_oid='%s'\n", (int)b, (unsigned long long)pt.order_id, pt.client_oid.c_str());
    CHECK(b); CHECK(pt.order_id == 0);
    CHECK(std::string(pt.client_oid.c_str()) == FakeExchange::param(g_fake.first().body, "newClientOrderId"));
    }
    {   // ---------------- I3 ----------------
    std::printf("I3 SEND -> HTTP400 -2013 (beklenmedik): sonuc kaydedilir; slot UNKNOWN veya REJECTED; POLL -2013 yolu test EDILEMEDI\n");
    g_fake.reset(Mode::ERR_2013, 0, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    Runner run(ctx);
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_SIM_I3");
    CHECK(ctx.order_channel.push_task(t));
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r);
    dispatch_order_result(ctx, r);
    const int st = slot_fsm(ctx, 1);
    std::printf("  slot=%s kill_switch=%d (status=%d err='%s')\n", sname(st), (int)ctx.kill_switch.load(), (int)r.status, r.error_msg.c_str());
    CHECK(st == static_cast<int>(OrderState::UNKNOWN) || st == static_cast<int>(OrderState::REJECTED));
    }
    {   // ---------------- I4 ----------------
    std::printf("I4 SPOT LONG ~29 USD -> send_exit_order: SELL MARKET, reduceOnly YOK, FILLED -> kapanis\n");
    g_fake.reset(Mode::FILLED, 5001, "0.29000000", "29.00000000", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    open_real(ctx, true, 100.0, 0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    Runner run(ctx);
    const uint64_t t0 = tsc_ns();
    send_exit_order(ctx, 100.0, "I4");
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r); print_body("fake", g_fake.first());
    CHECK(g_fake.ncalls() == 1);
    const Call c = g_fake.first();
    CHECK(FakeExchange::param(c.body, "side") == "SELL");
    CHECK(FakeExchange::param(c.body, "type") == "MARKET");
    CHECK(std::atof(FakeExchange::param(c.body, "quantity").c_str()) <= 0.29 + 1e-9);
    CHECK(c.body.find("reduceOnly") == std::string::npos);
    // I9 (EXIT)
    CHECK(r.ts_recv_ns >= t0); CHECK(r.ts_recv_ns - t0 < 10 * NS);
    dispatch_order_result(ctx, r);
    std::printf("  is_open=%d fw_pos_side=%d\n", (int)ctx.position.is_open(), (int)ctx.fw_pos_side.load());
    CHECK(!ctx.position.is_open()); CHECK(ctx.fw_pos_side.load() == 0);
    }
    {   // ---------------- I5 ----------------
    std::printf("I5 FUTURES SHORT ~29 USD -> BUY EXIT: reduceOnly=true, FILLED -> kapanis\n");
    g_fake.reset(Mode::FILLED, 5002, "0.29000000", "100.00000000", true);
    auto cfg = mkcfg("FUTURES"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    open_real(ctx, false, 100.0, 0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    Runner run(ctx);
    send_exit_order(ctx, 100.0, "I5");
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r); print_body("fake", g_fake.first());
    const Call c = g_fake.first();
    CHECK(g_fake.ncalls() == 1);
    CHECK(c.url == "/fapi/v1/order");
    CHECK(FakeExchange::param(c.body, "side") == "BUY");
    CHECK(FakeExchange::param(c.body, "reduceOnly") == "true");
    dispatch_order_result(ctx, r);
    std::printf("  is_open=%d fw_pos_side=%d\n", (int)ctx.position.is_open(), (int)ctx.fw_pos_side.load());
    CHECK(!ctx.position.is_open());
    }
    {   // ---------------- I6 ----------------
    std::printf("I6 ks_latch + ks_service -> EXIT -> order thread -> FILLED -> kapanis; fake'e tam 1 istek\n");
    g_fake.reset(Mode::FILLED, 5003, "0.29000000", "29.00000000", false);
    auto cfg = mkcfg("SPOT"); cfg.ks.flatten_on_trip = true; auto P = fresh(cfg); StrategyContext& ctx = *P;
    open_real(ctx, true, 100.0, 0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    ctx.awaiting_fill = false; ctx.last_bid = 100.0; ctx.last_ask = 100.01; ctx.last_mid = 100.0;
    Runner run(ctx);
    ks_latch(ctx, "SIM_TRIP", 0.0, 0.0);
    ks_service(ctx);
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r); print_body("fake", g_fake.first());
    std::printf("  kill_switch=%d\n", (int)ctx.kill_switch.load());
    CHECK(g_fake.ncalls() == 1);
    dispatch_order_result(ctx, r);
    CHECK(!ctx.position.is_open());
    std::remove(cfg.ks.lock_file.c_str());
    }
    {   // ---------------- I7 ----------------
    std::printf("I7 LONG 0.29 iken elle EXIT qty 0.30: fake'e 0 istek; REJECTED FW_REJECT; kill_switch\n");
    g_fake.reset(Mode::FILLED, 5004, "0.30000000", "30.00000000", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    open_real(ctx, true, 100.0, 0.29);
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    OrderTask t = mk_exit_task(ctx, OrderSide::SELL, 100.0, 0.30);
    const FwVerdict dv = final_order_firewall(ctx, t);   // order thread ile ayni girdi; dogrudan hukum
    std::printf("  dogrudan firewall verdict=%s\n", vname(dv));
    CHECK(dv == FwVerdict::EXIT_NOT_REDUCING);
    // Order thread logunu yakala (stdout -> dosya)
    std::cout.flush(); std::fflush(stdout);
    const int saved = dup(1); const int fd = open("/tmp/sim_i7_stdout.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, 1); close(fd);
    OrderResult r{}; bool got = false;
    {
        Runner run(ctx);
        CHECK(ctx.order_channel.push_task(t));
        got = wait_result(ctx, r, 2000);
    }
    std::cout.flush(); std::fflush(stdout); dup2(saved, 1); close(saved);
    CHECK(got);
    print_res(r);
    std::ifstream in("/tmp/sim_i7_stdout.txt"); std::stringstream ss; ss << in.rdbuf();
    const bool logged = ss.str().find("verdict=EXIT_NOT_REDUCING") != std::string::npos;
    std::printf("  log'da verdict=EXIT_NOT_REDUCING yakalandi=%d\n", (int)logged);
    CHECK(logged);
    std::remove("/tmp/sim_i7_stdout.txt");
    CHECK(g_fake.ncalls() == 0);
    CHECK(r.status == OrderStatus::REJECTED);
    CHECK(std::strcmp(r.error_msg.c_str(), "FW_REJECT") == 0);
    CHECK(ctx.kill_switch.load() == true);
    }
    {   // ---------------- I8 ----------------
    std::printf("I8 cap'te (fw_position_usd 29) giris SEND BUY notional 5: fake'e 0 istek; REJECTED FW_REJECT\n");
    g_fake.reset(Mode::NEW_ORD, 888, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    ctx.fw_position_usd.store(29.0); ctx.fw_free_quote_usd.store(1000.0);
    Runner run(ctx);
    mk_entry_slot(ctx, 1, 0.05, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.05, true, "KUSAK_SIM_I8");
    std::printf("  dogrudan firewall verdict=%s\n", vname(final_order_firewall(ctx, t)));
    CHECK(ctx.order_channel.push_task(t));
    OrderResult r{}; const bool got = wait_result(ctx, r, 2000); CHECK(got);
    print_res(r);
    CHECK(g_fake.ncalls() == 0);
    CHECK(r.status == OrderStatus::REJECTED);
    CHECK(std::strcmp(r.error_msg.c_str(), "FW_REJECT") == 0);
    }
    {   // ---------------- I10 ----------------
    std::printf("I10 SEND yaniti dispatch edilmez (SUBMITTED kalir); W=40 s sonra enforce_timeouts slotu UNKNOWN yapmali (mevcutta FAIL beklenir)\n");
    g_fake.reset(Mode::NEW_ORD, 888, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_SIM_I10");
    OrderResult r{}; bool got = false;
    {
        Runner run(ctx);
        CHECK(ctx.order_channel.push_task(t));
        got = wait_result(ctx, r, 2000);   // pop edip at (dispatch YOK)
    }   // order thread durdu; asagida saat izolasyonu icin ctx.cfg degistirilebilir
    CHECK(got);
    std::printf("  atilan sonuc: status=%d exch_id=%llu; fake istek=%zu\n", (int)r.status, (unsigned long long)r.exchange_order_id, g_fake.ncalls());
    OrderSlot* s = ctx.flat_order_map.find(1);
    CHECK(s != nullptr);
    if (s) {
        std::printf("  once: slot=%s exchange_order_id=%llu\n", order_state_name(s->fsm_state), (unsigned long long)s->exchange_order_id);
        CHECK(s->fsm_state == OrderState::SUBMITTED); CHECK(s->exchange_order_id == 0);
        // Zaman izolasyonu (uretim API'si: mock_time_ns, yalniz dry_run iken gecerli). tsc_ns() WSL acilisindan beri
        // W'den kisa olabilir -> sabit sahte saat. enforce_timeouts dry_run'a bakmaz. Order thread yukarida durduruldu.
        ctx.cfg.dry_run = true; ctx.mock_time_ns = 200 * NS;
        s->last_update_ns = ctx.mock_time_ns - W_NS - NS;
        std::printf("  now=%.1fs last_update=%.1fs (fark=%.1fs > W=%.0fs)\n", ctx.current_time_ns() / 1e9, s->last_update_ns / 1e9,
            (ctx.current_time_ns() - s->last_update_ns) / 1e9, W_NS / 1e9);
    }
    enforce_timeouts(ctx, ctx.flat_order_map, ctx.order_channel, ctx.cfg.symbol);
    const int st = slot_fsm(ctx, 1);
    std::printf("  sonra: slot=%s kill_switch=%d\n", sname(st), (int)ctx.kill_switch.load());
    CHECK(st == static_cast<int>(OrderState::UNKNOWN));
    CHECK(ctx.kill_switch.load() == true);
    OrderTask pt{}; const bool b = build_poll_task(ctx, pt);
    std::printf("  poll_task=%d client_oid='%s' active_client_oid='%s'\n", (int)b, pt.client_oid.c_str(), ctx.active_client_oid.c_str());
    CHECK(b && std::string(pt.client_oid.c_str()) == std::string(ctx.active_client_oid.c_str()));
    }
    {   // ---------------- I11 ----------------
    std::printf("I11 live_order_client bos iken giris SEND: <=1 s icinde OrderResult donmeli (mevcutta FAIL beklenir: continue)\n");
    g_fake.reset(Mode::NEW_ORD, 999, "0", "0", false);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg); StrategyContext& ctx = *P;
    ctx.live_order_client.reset();
    Runner run(ctx);
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, "KUSAK_SIM_I11");
    CHECK(ctx.order_channel.push_task(t));
    OrderResult r{}; const bool got = wait_result(ctx, r, 1000);
    std::printf("  sonuc geldi=%d (status=%d err='%s'); fake istek=%zu\n", (int)got, (int)r.status, r.error_msg.c_str(), g_fake.ncalls());
    CHECK(got);
    CHECK(!got || r.status == OrderStatus::REJECTED || r.status == OrderStatus::ERROR);
    }
    {   // ---------------- Son ----------------
    std::printf("SON fake disina cikan cagri yok: tum URL'ler beklenen endpoint, http_client null\n");
    std::lock_guard<std::mutex> g(g_fake.mu);
    bool allok = true;
    for (const auto& u : g_fake.all_urls) if (u != "/api/v3/order" && u != "/fapi/v1/order") allok = false;
    std::printf("  fake toplam cagri=%zu\n", g_fake.all_urls.size());
    CHECK(allok);
    auto cfg = mkcfg("SPOT"); auto P = fresh(cfg);
    CHECK(P->http_client == nullptr);
    }
    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
