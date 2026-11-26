// [PHASE37-TEST] Faz 3.7 kirmizi testleri: C19a (kanitli "gonderilmedi"), C19b (UNKNOWN + -2013 cozumu), C20 (loglama).
// Derleme: g++ -std=c++20 -O2 -Wall -Wextra -Iinclude -I$HOME/kusak_build/_deps/nlohmann_json-src/include tools/phase37_test.cpp -o ~/kusak_build/tools/phase37_test -lssl -lcrypto -lcurl -lpthread
// Kural: DAVRANIS seviyesinde; mevcut kodla derlenir. HttpResponse::request_sent alani henuz yoksa (requires-ifadesi)
// sahte borsa "gonderilmedi" kanitini veremez ve N1 FAIL verir (beklenen kirmizi). Beklentiler sonuca gore DEGISTIRILMEZ.
// Spesifikasyon (proje sahibi 2026-11-24, C19a+C19b+C20):
//  C19a: transport hatasi (http<=0) + request_sent=false => SEND sonucu ERROR "NOT_SENT" -> REJECTED; UNKNOWN yok, KS yok.
//        request_sent varsayilani true (kanit yoksa belirsiz). HTTP yaniti varsa request_sent etkisiz.
//  C19b: UNKNOWN + exchange_order_id==0 slot icin POLL sonucu ERROR "HTTP400_2013" art arda >= 3 kez VE
//        (son -2013 sonucunun ts_recv_ns - UNKNOWN'a giren sonucun ts_recv_ns) >= recvWindow_ms + 10 s
//        => slot REJECTED (error "NOTFOUND_RSLV"); awaiting_fill=false; active_client_oid bos; kill switch ACIK KALIR.
//        Arada baska bir sonuc (ornegin HTTP500) gelirse seri sifirlanir. Borsa ID'si olan UNKNOWN slot bu kurala girmez.
//  C20 : belirsiz SEND sonucunda govde (curl hata metni) loglanir; C19b seri ilerlemesi "UNKNOWN_NOTFOUND streak=" ve
//        cozum "NOTFOUND_RSLV" ile loglanir.
// Kapsam siniri: ctx.http_client null; ag yok. Kaynak (include/, src/) degismez.
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
// Sahte borsa: transport (-1) yaniti; request_sent alani varsa ayarlanir.
// Uye kontrolu sablona bagli (sablon disi requires-ifadesi eksik uyede sert hata verir).
// ---------------------------------------------------------------------------
template <class T> constexpr bool has_request_sent_v = requires(T& x) { x.request_sent = false; };
template <class T> void set_request_sent(T& r, int flag) {
    if constexpr (has_request_sent_v<T>) {
        if (flag == 0) r.request_sent = false;
        if (flag == 1) r.request_sent = true;
    } else {
        (void)r; (void)flag;
    }
}
struct FakeExchange {
    std::mutex mu;
    size_t n = 0;
    long status = -1;
    std::string body = "curl: Timeout was reached";
    int sent_flag = -1;            // -1: dokunma (varsayilan), 0: false, 1: true
    std::string last_body;
    void program(long st, const char* b, int sent) { std::lock_guard<std::mutex> g(mu); n = 0; status = st; body = b; sent_flag = sent; }
    size_t ncalls() { std::lock_guard<std::mutex> g(mu); return n; }
    exchange::HttpResponse handle(const std::string&, const std::string& params) {
        std::lock_guard<std::mutex> g(mu);
        ++n; last_body = params;
        exchange::HttpResponse r;
        r.status_code = status; r.body = body;
        set_request_sent(r, sent_flag);
        return r;
    }
};
static FakeExchange g_fake;
static constexpr bool kHasRequestSent = has_request_sent_v<exchange::HttpResponse>;

static exchange::SymbolFilterCache mk_filter() {
    exchange::SymbolFilterCache fc; fc.symbol = "BTCUSDT"; fc.tick_size = 0.01; fc.step_size = 0.00001;
    fc.min_qty = 0.00001; fc.max_qty = 9000.0; fc.min_notional = 5.0; fc.min_price = 0.01; fc.max_price = 1e9;
    return fc;
}
static config::KusakConfig mkcfg() {
    config::KusakConfig cfg; cfg.symbol = "BTCUSDT"; cfg.dry_run = false; cfg.testnet = true; cfg.market = "SPOT";
    cfg.max_position_usd = 30.0; cfg.ks.max_order_notional_usd = 30.0; cfg.initial_capital = 1000.0;
    cfg.write_csv = false; cfg.write_prometheus = false; cfg.recvWindow_ms = 5000;
    cfg.ks.lock_file = "/tmp/phase37_test_kill.lock";
    return cfg;
}
static std::unique_ptr<StrategyContext> fresh() {
    auto p = std::make_unique<StrategyContext>(mkcfg());
    p->filter_validator.update_filter(mk_filter());
    fw_publish_limits(*p);
    p->live_order_client.emplace(p->filter_validator,
        [](const std::string& ep, const std::string& params) { return g_fake.handle(ep, params); });
    p->live_order_client->set_market_endpoints(false);
    p->exit_retry_not_before_ns = 0;
    p->fw_free_quote_usd.store(1000.0);
    return p;
}
static OrderResult mk_res(uint64_t local, OrderStatus st, uint64_t ex, uint64_t ts, const char* err = nullptr,
                          double filled = 0.0, double avg = 0.0, bool is_exit = false, bool is_buy = true) {
    OrderResult r{}; r.local_order_id = local; r.order_id = local; r.exchange_order_id = ex; r.status = st;
    r.filled_qty = filled; r.avg_price = avg; r.is_exit = is_exit; r.is_buy = is_buy; r.ts_recv_ns = ts;
    if (err) r.error_msg.set(err, static_cast<uint8_t>(std::strlen(err)));
    return r;
}
static void mk_entry_slot(StrategyContext& ctx, uint64_t L, double qty, bool is_buy) {
    ctx.fsm_entry_seq = L; ctx.flat_order_map.insert(L, qty);
    for (auto st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(L, st);
    ctx.fsm_key = L; ctx.active_order_id = L; ctx.awaiting_fill = true; ctx.pending_is_buy = is_buy;
}
static OrderTask mk_send(StrategyContext& ctx, uint64_t L, double px, double qty, bool is_buy, const char* coid) {
    OrderTask t{}; t.kind = TaskKind::SEND; t.type = OrderType::LIMIT_MAKER; t.tif = TimeInForce::GTC;
    t.side = is_buy ? OrderSide::BUY : OrderSide::SELL; t.price = px; t.qty = qty;
    t.order_id = L; t.local_order_id = L; t.is_exit = false; t.ts_send_ns = tsc_ns();
    t.symbol.set(ctx.cfg.symbol.c_str(), static_cast<uint8_t>(ctx.cfg.symbol.size()));
    t.client_oid.set(coid, static_cast<uint8_t>(std::strlen(coid)));
    ctx.active_client_oid = t.client_oid;
    return t;
}
static int slot_fsm(StrategyContext& ctx, uint64_t key) {
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
// stdout yakalama (logger stdout'a da yazar; sim_exchange_test I7 kalibi)
struct Capture {
    int saved = -1; std::string path;
    explicit Capture(const char* p) : path(p) {
        std::cout.flush(); std::fflush(stdout);
        saved = dup(1); const int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600); dup2(fd, 1); close(fd);
    }
    std::string end() {
        std::cout.flush(); std::fflush(stdout); dup2(saved, 1); close(saved); saved = -1;
        std::ifstream in(path); std::stringstream ss; ss << in.rdbuf(); std::remove(path.c_str()); return ss.str();
    }
    ~Capture() { if (saved >= 0) end(); }
};

// SEND -> order thread -> sonuc; log yakalanir
struct SendCase { bool got; OrderResult r; size_t n; std::string log; int slot; bool ks; bool poll; };
static SendCase run_send(long st, const char* body, int sent, const char* coid) {
    g_fake.program(st, body, sent);
    auto P = fresh(); StrategyContext& ctx = *P;
    SendCase c{};
    mk_entry_slot(ctx, 1, 0.29, true);
    OrderTask t = mk_send(ctx, 1, 100.0, 0.29, true, coid);
    {
        Capture cap("/tmp/phase37_cap.txt");
        {
            Runner run(ctx);
            if (ctx.order_channel.push_task(t)) c.got = wait_result(ctx, c.r, 3000);
        }
        c.log = cap.end();
    }
    c.n = g_fake.ncalls();
    if (c.got) dispatch_order_result(ctx, c.r);
    c.slot = slot_fsm(ctx, 1); c.ks = ctx.kill_switch.load();
    OrderTask pt{}; c.poll = build_poll_task(ctx, pt);
    std::printf("  sonuc: got=%d status=%d err='%s' istek=%zu slot=%s kill_switch=%d poll=%d\n", (int)c.got,
        (int)c.r.status, c.r.error_msg.c_str(), c.n, sname(c.slot), (int)c.ks, (int)c.poll);
    return c;
}

// UNKNOWN'a cekilmis giris slotu (NET_UNKNOWN sonucu, ts=t0). exch_id != 0 ise once NEW ile borsa ID'si yazilir.
static std::unique_ptr<StrategyContext> unknown_ctx(uint64_t t0, uint64_t exch_id, const char* coid) {
    auto P = fresh(); StrategyContext& ctx = *P;
    mk_entry_slot(ctx, 1, 0.29, true);
    (void)mk_send(ctx, 1, 100.0, 0.29, true, coid);
    if (exch_id) dispatch_order_result(ctx, mk_res(1, OrderStatus::PENDING, exch_id, t0 - 1));
    dispatch_order_result(ctx, mk_res(1, OrderStatus::ERROR, exch_id, t0, "NET_UNKNOWN"));
    return P;
}
static void feed(StrategyContext& ctx, uint64_t ts, const char* err, uint64_t exch_id = 0, uint64_t L = 1) {
    dispatch_order_result(ctx, mk_res(L, OrderStatus::ERROR, exch_id, ts, err));
}

int main() {
    g_tsc_ns_mult = calibrate_tsc().ns_mult;
    constexpr uint64_t NS = 1'000'000'000ULL;
    constexpr uint64_t W = 15ULL * NS;   // recvWindow_ms (5000) + 10 s pay
    std::printf("bilgi: HttpResponse::request_sent alani %s\n", kHasRequestSent ? "VAR" : "YOK (C19a kirmizi beklenir)");

    // ======================= N: C19a =======================
    {
    std::printf("N1 transport -1 + request_sent=false (kanitli gonderilmedi): ERROR NOT_SENT -> REJECTED, KS yok, POLL yok\n");
    SendCase c = run_send(-1, "curl: Couldn't connect to server", 0, "KUSAK_P37_N1");
    CHECK(c.got); CHECK(c.n == 1);
    CHECK(c.r.status == OrderStatus::ERROR);
    CHECK(std::strcmp(c.r.error_msg.c_str(), "NOT_SENT") == 0);
    CHECK(c.slot == static_cast<int>(OrderState::REJECTED));
    CHECK(c.ks == false);
    CHECK(c.poll == false);
    }
    {
    std::printf("N2 transport -1, request_sent varsayilan (kanit yok): NET_UNKNOWN -> UNKNOWN, KS acik (regresyon)\n");
    SendCase c = run_send(-1, "curl: Timeout was reached", -1, "KUSAK_P37_N2");
    CHECK(c.got);
    CHECK(std::strcmp(c.r.error_msg.c_str(), "NET_UNKNOWN") == 0);
    CHECK(c.slot == static_cast<int>(OrderState::UNKNOWN));
    CHECK(c.ks == true);
    CHECK(c.poll == true);
    std::printf("L1 (C20) belirsiz SEND logu curl hata metnini icerir\n");
    CHECK(c.log.find("curl: Timeout was reached") != std::string::npos);
    }
    {
    std::printf("N3 transport -1 + request_sent=true (acik kanit: gonderildi): NET_UNKNOWN -> UNKNOWN\n");
    SendCase c = run_send(-1, "curl: Operation timed out", 1, "KUSAK_P37_N3");
    CHECK(std::strcmp(c.r.error_msg.c_str(), "NET_UNKNOWN") == 0);
    CHECK(c.slot == static_cast<int>(OrderState::UNKNOWN));
    }
    {
    std::printf("N4 HTTP 503 + request_sent=false (yanit geldi => bayrak etkisiz): belirsiz kalir\n");
    SendCase c = run_send(503, "", 0, "KUSAK_P37_N4");
    CHECK(std::strcmp(c.r.error_msg.c_str(), "NET_UNKNOWN") == 0);
    CHECK(c.slot == static_cast<int>(OrderState::UNKNOWN));
    }
    {
    std::printf("L1b (C20) NOT_SENT logu curl hata metnini icerir\n");
    SendCase c = run_send(-1, "curl: Couldn't connect to server", 0, "KUSAK_P37_L1B");
    CHECK(c.log.find("curl: Couldn't connect to server") != std::string::npos);
    }

    // ======================= R: C19b =======================
    const uint64_t t0 = tsc_ns() + 1000 * NS;   // gelecekte sabit taban; mutlak saatten bagimsiz
    {
    std::printf("R1 UNKNOWN + 3x -2013, sure < W: UNKNOWN kalir\n");
    auto P = unknown_ctx(t0, 0, "KUSAK_P37_R1"); StrategyContext& ctx = *P;
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    for (int k = 1; k <= 3; ++k) feed(ctx, t0 + k * NS, "HTTP400_2013");
    std::printf("  slot=%s\n", sname(slot_fsm(ctx, 1)));
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    CHECK(ctx.awaiting_fill == true);
    }
    {
    std::printf("R2 UNKNOWN + 3x -2013, sure >= W: 2. sonra UNKNOWN, 3. sonra REJECTED; KS acik kalir; loglar\n");
    auto P = unknown_ctx(t0, 0, "KUSAK_P37_R2"); StrategyContext& ctx = *P;
    std::string log;
    {
        Capture cap("/tmp/phase37_cap_r2.txt");
        feed(ctx, t0 + W + 1 * NS, "HTTP400_2013");
        feed(ctx, t0 + W + 2 * NS, "HTTP400_2013");
        const int mid = slot_fsm(ctx, 1);
        feed(ctx, t0 + W + 3 * NS, "HTTP400_2013");
        log = cap.end();
        std::printf("  2. sonrasi slot=%s\n", sname(mid));
        CHECK(mid == static_cast<int>(OrderState::UNKNOWN));
    }
    OrderTask pt{};
    std::printf("  3. sonrasi slot=%s awaiting_fill=%d active_client_oid='%s' kill_switch=%d\n", sname(slot_fsm(ctx, 1)),
        (int)ctx.awaiting_fill, ctx.active_client_oid.c_str(), (int)ctx.kill_switch.load());
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::REJECTED));
    CHECK(ctx.awaiting_fill == false);
    CHECK(ctx.active_client_oid.empty());
    CHECK(ctx.kill_switch.load() == true);
    CHECK(!build_poll_task(ctx, pt));
    std::printf("L2 (C20) seri ve cozum loglanir\n");
    CHECK(log.find("UNKNOWN_NOTFOUND streak=") != std::string::npos);
    CHECK(log.find("NOTFOUND_RSLV") != std::string::npos);
    }
    {
    std::printf("R3 UNKNOWN + yalniz 2x -2013 (sure >= W): UNKNOWN kalir\n");
    auto P = unknown_ctx(t0, 0, "KUSAK_P37_R3"); StrategyContext& ctx = *P;
    feed(ctx, t0 + W + 1 * NS, "HTTP400_2013");
    feed(ctx, t0 + W + 2 * NS, "HTTP400_2013");
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    }
    {
    std::printf("R4 seri kesintisi: 2x -2013, HTTP500, 1x -2013 -> UNKNOWN; +2x -2013 -> REJECTED\n");
    auto P = unknown_ctx(t0, 0, "KUSAK_P37_R4"); StrategyContext& ctx = *P;
    feed(ctx, t0 + W + 1 * NS, "HTTP400_2013");
    feed(ctx, t0 + W + 2 * NS, "HTTP400_2013");
    feed(ctx, t0 + W + 3 * NS, "HTTP500");
    feed(ctx, t0 + W + 4 * NS, "HTTP400_2013");
    std::printf("  kesinti sonrasi slot=%s\n", sname(slot_fsm(ctx, 1)));
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    feed(ctx, t0 + W + 5 * NS, "HTTP400_2013");
    feed(ctx, t0 + W + 6 * NS, "HTTP400_2013");
    std::printf("  +2 sonrasi slot=%s\n", sname(slot_fsm(ctx, 1)));
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::REJECTED));
    }
    {
    std::printf("R5 borsa ID'li UNKNOWN + 3x -2013 (sure >= W): kurala girmez, UNKNOWN kalir\n");
    auto P = unknown_ctx(t0, 4242, "KUSAK_P37_R5"); StrategyContext& ctx = *P;
    std::printf("  baslangic slot=%s\n", sname(slot_fsm(ctx, 1)));
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    for (int k = 1; k <= 3; ++k) feed(ctx, t0 + W + k * NS, "HTTP400_2013", 4242);
    CHECK(slot_fsm(ctx, 1) == static_cast<int>(OrderState::UNKNOWN));
    }
    {
    std::printf("R6 EXIT UNKNOWN + 3x -2013 (sure >= W): EXIT slotu REJECTED, pozisyon ACIK kalir, awaiting_fill=false\n");
    auto P = fresh(); StrategyContext& ctx = *P;
    mk_entry_slot(ctx, 1, 0.29, true);
    dispatch_order_result(ctx, mk_res(1, OrderStatus::FILLED, 9001, t0 - 10 * NS, nullptr, 0.29, 100.0, false, true));
    CHECK(ctx.position.is_open());
    const uint64_t K = ++ctx.fsm_exit_seq;
    ctx.flat_order_map.insert(K, 0.29);
    for (auto st : {OrderState::CREATED, OrderState::PREFLIGHT, OrderState::SUBMITTED}) ctx.flat_order_map.transition_key(K, st);
    ctx.fsm_key = K; ctx.awaiting_fill = true; ctx.pending_is_buy = false;
    ctx.active_client_oid.set("KUSAK_P37_R6", 12);
    dispatch_order_result(ctx, mk_res(K, OrderStatus::ERROR, 0, t0, "NET_UNKNOWN", 0, 0, true, false));
    CHECK(slot_fsm(ctx, K) == static_cast<int>(OrderState::UNKNOWN));
    for (int k = 1; k <= 3; ++k)
        dispatch_order_result(ctx, mk_res(K, OrderStatus::ERROR, 0, t0 + W + k * NS, "HTTP400_2013", 0, 0, true, false));
    std::printf("  slot=%s is_open=%d awaiting_fill=%d\n", sname(slot_fsm(ctx, K)), (int)ctx.position.is_open(), (int)ctx.awaiting_fill);
    CHECK(slot_fsm(ctx, K) == static_cast<int>(OrderState::REJECTED));
    CHECK(ctx.position.is_open());
    CHECK(ctx.awaiting_fill == false);
    }

    std::remove("/tmp/phase37_test_kill.lock");
    std::printf("SONUC fails=%d\n", fails);
    return fails ? 1 : 0;
}
