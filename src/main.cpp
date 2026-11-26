// ============================================================================
// KUSAK V13 — HFT Execution Engine (Tier-1 Specification Entegrasyonu)
//
// [V13-SPSC]      OrderChannel: std::queue+mutex+CV → çift OrderSpscRing (lock-free)
// [V13-STRUCT]    OrderTask/OrderResult: std::string/heap → FixedString (zero heap)
//                 NOT: Spec 64B hedefler; 17B(sym)+33B(coid)+35B(skalalar) = 85B > 64B.
//                 Çözüm: 128B stride, alignas(64) korundu (2 cache line).
// [V13-MAP]       order_map: std::unordered_map → FlatOrderMap (O(1), L1-cache dostu)
// [V13-BACKOFF]   run_order_thread: cv.wait_for → adaptif _mm_pause / tsc_pause_cycles
// [V13-HOOKS]     Ana döngü: enforce_timeouts + monitor_drops kancaları eklendi
// [V13-CLOCK]     Tüm zaman ölçümleri: mono_ns() (CLOCK_MONOTONIC_RAW)
// [V13-SPSC-OWN]  SPSC üretici sahipliği: order thread tasks ring'e YAZMIYOR.
//                 PARTIALLY_FILLED/PENDING → result ring → strategy thread polling.
// [ARCH-V2-FIX]   vpin_velocity_tracker.hpp uyum denetimi (8 çağrı noktası):
//                 VPINConfig alan adları (*_scaled), vpin()->vpin_x1e6(),
//                 update_trade() 1e6 ölçekleme, set_close_callback imzası.
//                 Detay için ilgili satırlardaki [ARCH-V2-FIX] etiketlerine bakın.
//
// ← V12'den taşınan tüm iş mantığı fix etiketleri geçerliliğini korur.
// ============================================================================
// [FIX-INCLUDE-TOP] strategy_core.hpp artik KENDI butun bagimliliklarini
// (config.hpp, engine/types.hpp, vpin_velocity_tracker.hpp, vb.) kendisi
// #include eder ve normalize_terminal_fill_state/enforce_timeouts gibi
// dogrudan bagimliliklarini kendi icinde tanimlar -- main.cpp icindeki
// daha sonraki tanimlarina bagimli DEGILDIR. Bu yuzden dosyanin EN
// TEPESINE, standart kutuphanelerin yanina alinabildi (#pragma once
// sayesinde ayni header'lerin main.cpp'de tekrar #include edilmesi
// zararsizdir).
#include "strategy_core.hpp"
#include "order_guard.hpp"   // [FIX-P1-STARTUP] kusak::guard::startup_decision(StartupProbe) / floor_to_step / is_multiple

#include <cassert>
#include "latency_monitor.hpp"
#include <research_types.hpp>
#include <config.hpp>
#include <logger.hpp>
#include "vpin_velocity_tracker.hpp"
#include "regime_detector.hpp"
#include "auto_param_adapter.hpp"
#include "risk_scorer.hpp"
#include "telemetry_sink.hpp"
#include "slippage_models.hpp"
#include "adverse_selection_tracker.hpp"
#include "binance_rest_client.hpp"
#include "binance_http_client.hpp"
#include "exchange_info_manager.hpp"
#include "ws_client.hpp"
#include "ws_client_multipath.hpp"

#include "infra/time.hpp"
#include "engine/market_event.hpp"
#include "infra/spsc_ring.hpp"
#include "engine/types.hpp"

#include <atomic>
#include <cmath>
#include <limits>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <immintrin.h>   // _mm_pause
#include <x86intrin.h>   // __rdtsc
#include <iostream>
#include <memory>
#include <optional>
#include <concepts>
#include <string_view>
#include <thread>
#ifdef _WIN32
#  include <Windows.h>
#else
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>
#endif
struct Fx1e6 {
    int64_t raw;
    explicit constexpr Fx1e6(int64_t r) noexcept : raw(r) {}
    static constexpr Fx1e6 from_double(double d) noexcept {
        return Fx1e6{static_cast<int64_t>(d * 1e6)};
    }
    constexpr double to_double() const noexcept {
        return static_cast<double>(raw) / 1e6;
    }
    operator double() const = delete; 
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "market_ring head/tail lock-free olmali");
// ─────────────────────────────────────────────────────────────────────────────
// Global stop flag
// ─────────────────────────────────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "g_stop must be lock-free for signal handler safety");

extern "C" void signal_handler(int) noexcept {
    g_stop.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-CLOCK] g_tsc_ns_mult tanımı — header (infra/time.hpp) extern bildirir
// ─────────────────────────────────────────────────────────────────────────────
uint64_t g_tsc_ns_mult = 0;  // kalibrasyonla doldurulur

// ─────────────────────────────────────────────────────────────────────────────
// g_log_ring — global instance (template LogRing infra/spsc_ring.hpp'den gelir)
// ─────────────────────────────────────────────────────────────────────────────
static LogRing<8192> g_log_ring;


// [FIX-SEPARATION-1] JSON parse yardimcilari (json_find_string_view vb.)
// artik utils.hpp icinde -- strateji karar mantigiyla ilgisi olmayan
// saf REST/JSON ayristirma fonksiyonlari izole edildi.
#include "utils.hpp"

// [FIX-SELFCONTAINED-1/2] enforce_timeouts ve normalize_terminal_fill_state
// artik strategy_core.hpp icinde -- oradan asagida #include edilecek.


static bool make_symbol_orderid_query(const FixedString<16>& symbol,
                                      uint64_t order_id,
                                      char* out,
                                      size_t out_size) noexcept {
    if (!out || out_size < 32) return false;
    char* p = out;
    std::memcpy(p, "symbol=", 7); p += 7;
    const size_t sym_len = std::strlen(symbol.c_str());
    std::memcpy(p, symbol.c_str(), sym_len); p += sym_len;
    std::memcpy(p, "&orderId=", 9); p += 9;
    auto [ptr, ec] = std::to_chars(p, out + out_size - 1, order_id);
    if (ec != std::errc{}) return false;
    *ptr = '\0';
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// [FIX-FAZ0] Hakem Faz 0 yamalari: lock-free, mutex/sleep yok.
// ════════════════════════════════════════════════════════════════════════════
// Sinirli spin butcesi: en kotu durum gecikmesi sabit.
inline constexpr unsigned kPushSpinBudget = 64;
// epsilon() yalniz 1.0 civari icin gecerli; buyukluge olcekli goreli marj.
inline constexpr double   kNotionalRelTol = 8.0 * std::numeric_limits<double>::epsilon();

// Gonderim kapisi sayaclari (StrategyContext header'i degistirilmeden dosya kapsaminda).
alignas(64) inline std::atomic<uint32_t> g_inflight_orders{0};
alignas(64) inline std::atomic<uint64_t> g_dropped_results{0};
alignas(64) inline std::atomic<bool>     g_fw_result_lost{false};
static_assert(std::atomic<uint32_t>::is_always_lock_free, "inflight sayaci lock-free olmali");

#if defined(__x86_64__) || defined(_M_X64)
  #define HFT_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__)
  #define HFT_CPU_RELAX() asm volatile("yield" ::: "memory")
#else
  #define HFT_CPU_RELAX() ((void)0)
#endif

// [FIX-FAZ0-1] trading_paused plain bool ise de thread-safe erisim (C++20 atomic_ref).
// NOT: Ayni degiskene baska dosyalarda (header) duz erisim varsa orada da ayni
// yardimcilarla erisilmeli; aksi halde race o noktalarda surer.
FORCE_INLINE bool paused_load(bool& flag) noexcept {
    return std::atomic_ref<bool>(flag).load(std::memory_order_acquire);
}
FORCE_INLINE void paused_store(bool& flag, bool v) noexcept {
    std::atomic_ref<bool>(flag).store(v, std::memory_order_seq_cst);
}
// Yalniz false->true gecisini kazanan thread true doner (yan etkiler tam bir kez).
FORCE_INLINE bool paused_try_set(bool& flag) noexcept {
    bool expected = false;
    return std::atomic_ref<bool>(flag).compare_exchange_strong(
        expected, true, std::memory_order_seq_cst, std::memory_order_relaxed);
}

// [FIX-FAZ0-2] push_result basarisiz/kuyruk dolu -> sinirli spin, sonra fail-safe.
// Bloklama, sleep, mutex yok. false => sonuc teslim edilemedi.
template <class Channel, class Result>
FORCE_INLINE bool push_result_bounded(Channel& ch, const Result& r) noexcept {
    for (unsigned i = 0; i < kPushSpinBudget; ++i) {
        if (LIKELY(ch.push_result(r))) return true;
        HFT_CPU_RELAX();
    }
    g_dropped_results.fetch_add(1, std::memory_order_relaxed);
    g_fw_result_lost.store(true, std::memory_order_release);
    return false;
}

// [FIX-P36-C17] Order thread'in TUM sonuc teslimleri bu yoldan: sinirli spin; teslim edilemezse sonuc kayiptir
// (slot SUBMITTED'da kalabilir, borsadaki durum bilinmez) -> kill switch ONCE tetiklenir (fail-closed).
// Kayip slot C16 (SUBMITTED_LOST_TIMEOUT) ile UNKNOWN -> POLL yoluna toparlanir. SPSC: tek uretici order thread.
template <class Result>
FORCE_INLINE bool deliver_result(StrategyContext& ctx, OrderChannel<>& ch, const Result& r) noexcept {
    if (LIKELY(push_result_bounded(ch, r))) return true;
    ctx.kill_switch.store(true, std::memory_order_seq_cst);
    ctx.logger.critical("ORDER_THREAD", "SONUC TESLIM EDILEMEDI (sonuc halkasi dolu) local_id=", r.local_order_id,
                        " status=", static_cast<int>(r.status), " -- KILL-SWITCH AKTIF");
    return false;
}

// [FIX-FAZ0-3] Notional: sinirda emin degilsek REDDET (fail-closed). true => yeterli.
FORCE_INLINE bool notional_ok(double qty, double px, double min_notional) noexcept {
    if (!(qty > 0.0) || !std::isfinite(qty) || !(px > 0.0) || !std::isfinite(px)) return false;
    const double notional = qty * px;
    if (!std::isfinite(notional)) return false;
    return notional >= min_notional * (1.0 + kNotionalRelTol);
}

// [FIX-P37-C20] Cold-path log yardimcisi: yanit govdesinin ilk 120 karakteri, yalniz yazdirilabilir ASCII
// (satir sonu/kontrol karakteri -> '.'). Govde curl hata metni veya borsa JSON'udur; anahtar icermez.
static std::string body_snippet(const std::string& b) {
    std::string o; o.reserve(std::min<size_t>(b.size(), 120));
    for (size_t i = 0; i < b.size() && o.size() < 120; ++i) {
        const unsigned char c = static_cast<unsigned char>(b[i]);
        o.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
    }
    return o;
}

FORCE_INLINE const char* order_endpoint(bool futures) noexcept {
    return futures ? "/fapi/v1/order" : "/api/v3/order";
}

template<typename CfgT>
static int read_core_from_cfg_or_env(const CfgT& cfg,
                                     bool strategy) noexcept {
    int core = -1;
    if constexpr (requires { cfg.strategy_core; cfg.order_core; }) {
        core = strategy ? static_cast<int>(cfg.strategy_core)
                        : static_cast<int>(cfg.order_core);
    }
    if (core >= 0) return core;
    const char* env_name = strategy ? "KUSAK_STRATEGY_CORE" : "KUSAK_ORDER_CORE";
    const char* v = std::getenv(env_name);
    if (!v) return -1;
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == v || parsed < 0) return -1;
    return static_cast<int>(parsed);
}

static bool pin_current_thread_to_core(int core) noexcept {
    if (core < 0) return true;
#ifdef _WIN32
    const DWORD logical_cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (logical_cores == 0 || static_cast<DWORD>(core) >= logical_cores) return false;
    const DWORD_PTR mask = (DWORD_PTR(1) << core);
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    const long logical_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (logical_cores <= 0 || core >= logical_cores) return false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// [V13-HOOKS] monitor_drops — spec §5 Silent Drop Detection
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t g_last_drop_count = 0;

FORCE_INLINE void monitor_drops(SpscRing<131072>& ring,
                                 kusak::log::Logger& logger) noexcept {
    const uint64_t now_drops = ring.dropped();
    if (UNLIKELY(now_drops != g_last_drop_count)) {
        logger.error("DROP_MONITOR",
            "Market event drop tespit edildi: total=", now_drops,
            " delta=", now_drops - g_last_drop_count);
        g_last_drop_count = now_drops;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// run_order_thread
// [V13-BACKOFF]:   cv.wait_for → adaptif _mm_pause / tsc_pause_cycles
// [V13-SPSC-OWN]:  PARTIALLY_FILLED/PENDING → yalnızca result push;
//                  order thread tasks ring'e YAZMIYOR (SPSC kuralı)
// [V13-STRUCT]:    std::string → FixedString.c_str()
// [V13-CLOCK]:     mono_ns()
// ─────────────────────────────────────────────────────────────────────────────
static void run_order_thread(StrategyContext& ctx) noexcept
{
    OrderChannel<1024>& ch = ctx.order_channel;
    uint32_t idle = 0;

    while (!ch.stop.load(std::memory_order_acquire)) {
        OrderTask task{};

        if (LIKELY(ch.pop_task(task))) {
            idle = 0;

            // ── CANCEL: exchange cancel çağrısı dene, sonucu aktar ──────────
            if (task.kind == TaskKind::CANCEL) {
                bool cancel_confirmed = true;  // http_client yoksa (dry-run) davranis degismez
                char cancel_err[16] = {0};
                if (ctx.http_client) {
                    const bool futures = (ctx.cfg.market == "FUTURES");
                    char params[96];
                    if (LIKELY(make_symbol_orderid_query(task.symbol, task.order_id,
                                                         params, sizeof(params)))) {
                        auto del = ctx.http_client->delete_signed(order_endpoint(futures), params);
                        cancel_confirmed = del.ok();
                        if (!cancel_confirmed) {
                            std::snprintf(cancel_err, sizeof(cancel_err), "CANC_HTTP%ld", del.status_code);
                            ctx.logger.warn("ORDER_THREAD",
                                "CANCEL borsa tarafindan reddedildi/basarisiz: id=",
                                task.order_id, " http=", del.status_code);
                        }
                    } else {
                        panic_critical("PANIC: CANCEL query build failed");
                        return;
                    }
                }
                OrderResult res{};
                res.order_id       = task.order_id;
                res.local_order_id = task.local_order_id;
                res.is_exit        = task.is_exit;
                res.is_buy         = (task.side == OrderSide::BUY);
                res.status         = cancel_confirmed ? OrderStatus::CANCELED : OrderStatus::ERROR;
                if (!cancel_confirmed)
                    res.error_msg.set(cancel_err, static_cast<uint8_t>(std::strlen(cancel_err)));
                res.ts_recv_ns     = tsc_ns();
                (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
                continue;
            }

            // ── POLL: REST sorgu ─────────────────────────────────────────────
            if (task.kind == TaskKind::POLL) {
                if (!ctx.http_client) {
                    ctx.poll_in_flight.store(false, std::memory_order_release);  // [FIX-BACKPRESSURE]
                    continue;
                }
                const bool futures = (ctx.cfg.market == "FUTURES");
                char params[160];
                // [FIX-P0-02] sorgu kimligi: borsa orderId, yoksa gecerli client_oid; ikisi de yoksa agsiz ERROR
                if (UNLIKELY(!make_poll_query(task, params, sizeof(params)))) {
                    ctx.poll_in_flight.store(false, std::memory_order_release);
                    OrderResult nres{};
                    nres.status         = OrderStatus::ERROR;
                    nres.local_order_id = task.local_order_id;
                    nres.order_id       = task.order_id;
                    nres.is_exit        = task.is_exit;
                    nres.is_buy         = (task.side == OrderSide::BUY);
                    nres.ts_recv_ns     = tsc_ns();
                    constexpr char nmsg[] = "NET_NOID";
                    nres.error_msg.set(nmsg, static_cast<uint8_t>(sizeof(nmsg) - 1));
                    (void)deliver_result(ctx, ch, nres);   // [FIX-P36-C17]
                    continue;
                }
                auto raw = ctx.http_client->get_signed(order_endpoint(futures), params);
                ctx.poll_in_flight.store(false, std::memory_order_release);  // [FIX-BACKPRESSURE]

                OrderResult res{};
                res.order_id       = task.order_id;
                res.local_order_id = task.local_order_id;
                res.is_exit        = task.is_exit;
                res.is_buy         = (task.side == OrderSide::BUY);
                res.ts_recv_ns     = tsc_ns();

                if (!raw.ok()) {
                    res.status = OrderStatus::ERROR;
                    char emsg[16];
                    // [FIX-P0-02] -2013 (Order does not exist): kesin "yok" DEGIL, UNKNOWN'da kalir (HTTP oneki belirsiz)
                    const std::string_view ebody{raw.body.data(), raw.body.size()};
                    if (raw.status_code == 400 &&
                        (ebody.find("\"code\":-2013") != std::string_view::npos ||
                         ebody.find("\"code\": -2013") != std::string_view::npos)) {
                        std::snprintf(emsg, sizeof(emsg), "HTTP400_2013");
                    } else {
                        std::snprintf(emsg, sizeof(emsg), "HTTP%ld", raw.status_code);
                    }
                    res.error_msg.set(emsg, static_cast<uint8_t>(std::strlen(emsg)));
                    // [FIX-P37-C20] POLL hata sonucu artik sessiz degil (-2013 serisi dispatch'te ayrica loglanir)
                    ctx.logger.warn("POLL", "POLL hatasi err=", emsg, " local_id=", task.local_order_id,
                                    " sorgu=", (task.order_id ? "orderId" : "origClientOrderId"),
                                    " govde='", body_snippet(raw.body), "'");
                    (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
                    continue;
                }

                const std::string_view body{raw.body.data(), raw.body.size()};
                const auto status_sv = json_find_string_view(body, "\"status\"");
                if (!status_sv.has_value()) {
                    res.status = OrderStatus::ERROR;
                    constexpr char emsg[] = "JSON_STATUS";
                    res.error_msg.set(emsg, static_cast<uint8_t>(sizeof(emsg) - 1));
                    (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
                    continue;
                }
                // ── [ULL-OP1] Status decode: branch cascade infazı ────────────
                // Raw buffer üzerinden tek 8-byte unaligned load + length-gated
                // 64-bit magic compare. Geçici string/string_view operator==
                // çağrısı yok, strcmp yok, locale yok. x86-64 LE varsayımı.
                //
                // Magic'ler (little-endian, sıfır-padded ilk 8 byte):
                //   FILLED            (6B): 0x0000'4445'4C4C'4946
                //   PARTIALLY_FILLED (16B): ilk 8B = 'PARTIALL'
                //                            0x4C4C'4149'5452'4150
                //   CANCELED          (8B): 0x4445'4C45'434E'4143
                //   REJECTED          (8B): 0x4445'5443'454A'4552
                //   EXPIRED           (7B): 0x0044'4552'4950'5845
                //
                // Buffer overrun garantisi: status_sv, JSON içindeki tırnaklı
                // token gövdesini gösterir; sonrasında en az kapanış '"' bulunur,
                // bu yüzden 8-byte unaligned load buffer sınırı içindedir.
                {
                    const char* __restrict__ sp = status_sv->data();
                    const std::size_t        sl = status_sv->size();

                    std::uint64_t w = 0;
                    std::memcpy(&w, sp, std::min<std::size_t>(sl, 8));

                    constexpr std::uint64_t M_FILLED   = 0x0000'4445'4C4C'4946ULL;
                    constexpr std::uint64_t M_PARTIAL8 = 0x4C4C'4149'5452'4150ULL;
                    constexpr std::uint64_t M_CANCELED = 0x4445'4C45'434E'4143ULL;
                    constexpr std::uint64_t M_REJECTED = 0x4445'5443'454A'4552ULL;
                    constexpr std::uint64_t M_EXPIRED  = 0x0044'4552'4950'5845ULL;

                    OrderStatus st = OrderStatus::PENDING;
                    if      (sl == 6  && w == M_FILLED)                       st = OrderStatus::FILLED;
                    else if (sl == 16 && w == M_PARTIAL8)                     st = OrderStatus::PARTIALLY_FILLED;
                    else if (sl == 8  && (w == M_CANCELED || w == M_REJECTED)) st = OrderStatus::CANCELED;
                    else if (sl == 7  && w == M_EXPIRED)                      st = OrderStatus::CANCELED;

                    res.status = st;
                }

                std::string_view num{};
                if (json_find_number_view(body, "\"executedQty\"", num)) {
                    parse_double_sv(num, res.filled_qty);
                }
                if (json_find_number_view(body, "\"avgPrice\"", num)) {
                    parse_double_sv(num, res.avg_price);
                }
                if (res.avg_price < 1e-12 && json_find_number_view(body, "\"price\"", num)) {
                    parse_double_sv(num, res.avg_price);
                }
                if (json_find_number_view(body, "\"origQty\"", num)) {
                    parse_double_sv(num, res.remaining_qty);
                    if (res.filled_qty > 0.0 && res.remaining_qty >= res.filled_qty) {
                        res.remaining_qty -= res.filled_qty;
                    }
                }
                res.status = normalize_terminal_fill_state(res.status, res.remaining_qty);
                if (json_find_number_view(body, "\"orderId\"", num)) {
                    parse_u64_sv(num, res.exchange_order_id);
                }

                (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
                continue;
            }

            // ── SEND / EXIT: yeni emir gönder ───────────────────────────────
            if (task.kind == TaskKind::SEND || task.kind == TaskKind::EXIT) {
                // [FIX-P36-C17] Istemci yok: emir ag'a CIKMADI -> kesin REJECTED sonuc (eskiden sessiz continue:
                // slot SUBMITTED'da takiliyordu). dry_run'da SEND/EXIT order thread'e gelmez; bu dal yalniz
                // dry_run=false + istemci kurulamamis durumdadir.
                if (UNLIKELY(!ctx.live_order_client.has_value())) {
                    OrderResult nr{};
                    nr.order_id       = task.order_id;
                    nr.local_order_id = task.local_order_id;
                    nr.is_exit        = (task.kind == TaskKind::EXIT);
                    nr.is_buy         = (task.side == OrderSide::BUY);
                    nr.status         = OrderStatus::REJECTED;   // borsaya hic gitmedi
                    nr.ts_recv_ns     = tsc_ns();
                    constexpr char ne[] = "NO_CLIENT";
                    nr.error_msg.set(ne, static_cast<uint8_t>(sizeof(ne) - 1));
                    ctx.logger.critical("ORDER_THREAD", "live_order_client YOK: emir gonderilmedi kind=",
                                        (task.kind == TaskKind::EXIT ? "EXIT" : "SEND"), " qty=", task.qty);
                    (void)deliver_result(ctx, ch, nr);
                    continue;
                }

                kusak::exchange::RestOrderClient::OrderRequest req;
                req.symbol     = task.symbol.c_str();
                req.side       = (task.side == OrderSide::BUY) ? "BUY" : "SELL";
                req.type       = (task.type == OrderType::LIMIT_MAKER) ? "LIMIT_MAKER"  // [FIX-MVP-MAKER]
                                : (task.type == OrderType::LIMIT)      ? "LIMIT"
                                                                        : "MARKET";
                req.time_in_force = "GTC"; // LIMIT_MAKER icin build_order_params bunu zaten yok sayar
                req.quantity   = task.qty;
                req.client_oid = task.client_oid.c_str();
                if (task.type == OrderType::LIMIT || task.type == OrderType::LIMIT_MAKER)
                    req.price = task.price;
                req.reduce_only = exit_needs_reduce_only(task, ctx.cfg.market == "FUTURES");   // [FIX-P0-03] yalniz FUTURES EXIT

                // ════════════════════════════════════════════════════════════════════════════
                // [FIX-P0-FW] ZORUNLU SON BARIYER — SEND *ve* EXIT: borsaya giden TEK kapi burasi.
                // new_order() cagrisindan hemen once, tuketici (order thread) tarafinda, koşulsuz.
                // Bu satirlar ile new_order arasina baska kod / erken-cikis / muafiyet girmemeli.
                // ════════════════════════════════════════════════════════════════════════════
                {
                    const FwVerdict fv = final_order_firewall(ctx, task);
                    if (UNLIKELY(fv != FwVerdict::PASS)) {
                        OrderResult fr{};
                        fr.order_id       = task.order_id;
                        fr.local_order_id = task.local_order_id;
                        fr.is_exit        = (task.kind == TaskKind::EXIT);
                        fr.is_buy         = (task.side == OrderSide::BUY);
                        fr.status         = OrderStatus::REJECTED;   // borsaya hic gitmedi
                        fr.ts_recv_ns     = tsc_ns();
                        constexpr char e[] = "FW_REJECT";
                        fr.error_msg.set(e, static_cast<uint8_t>(sizeof(e) - 1));
                        ctx.logger.critical("FIREWALL", "FINAL REJECT kind=",
                                            (task.kind == TaskKind::EXIT ? "EXIT" : "SEND"),
                                            " verdict=", fw_verdict_name(fv),   // [FIX-P0-03]
                                            " qty=", task.qty, " px=", task.price);
                        // [FIX-FAZ0] Fail-closed: kill switch sonuc teslimine bagli degil, ONCE tetiklenir.
                        // seq_cst: asagidaki gonderim kapisiyla Dekker tarzi eslesme.
                        ctx.kill_switch.store(true, std::memory_order_seq_cst);   // invariant ihlali => trip
                        // Kuyruk doluysa sinirli spin; basarisizsa sayac+bayrak (push_result_bounded icinde).
                        (void)push_result_bounded(ch, fr);
                        continue;
                    }
                }

                // ════════════════════════════════════════════════════════════════════════
                // [FIX-FAZ0-TOCTOU] GONDERIM KAPISI: kontrol ile new_order arasindaki
                // check-then-act penceresini seq_cst siralamasiyla kapatir.
                // Gonderici: inflight++ -> kill_switch oku.  Tetikleyici: kill_switch yaz -> inflight oku.
                // Ikisi seq_cst oldugundan en az biri digerini gorur:
                //   - gonderici kill_switch'i gorurse emir GITMEZ,
                //   - gormezse tetikleyici inflight>0 gorur (ks_service cancel-all kapsami).
                // EXIT (azaltan yon) kapidan muaf: kill switch sonrasi pozisyon kapatma engellenmez.
                // ════════════════════════════════════════════════════════════════════════
                const bool gate_applies = (task.kind != TaskKind::EXIT);
                g_inflight_orders.fetch_add(1, std::memory_order_seq_cst);
                if (UNLIKELY(gate_applies && ctx.kill_switch.load(std::memory_order_seq_cst))) {
                    g_inflight_orders.fetch_sub(1, std::memory_order_release);
                    OrderResult gr{};
                    gr.order_id       = task.order_id;
                    gr.local_order_id = task.local_order_id;
                    gr.is_exit        = false;
                    gr.is_buy         = (task.side == OrderSide::BUY);
                    gr.status         = OrderStatus::REJECTED;   // borsaya hic gitmedi
                    gr.ts_recv_ns     = tsc_ns();
                    constexpr char ge[] = "KS_GATE";
                    gr.error_msg.set(ge, static_cast<uint8_t>(sizeof(ge) - 1));
                    ctx.logger.critical("FIREWALL", "KILL_SWITCH GATE: emir gonderilmedi qty=",
                                        task.qty, " px=", task.price);
                    (void)push_result_bounded(ch, gr);
                    continue;
                }
                auto resp = ctx.live_order_client->new_order(req, task.price);
                g_inflight_orders.fetch_sub(1, std::memory_order_release);

                OrderResult res{};
                res.order_id       = task.order_id;
                res.local_order_id = task.local_order_id;
                res.is_exit        = (task.kind == TaskKind::EXIT);
                res.is_buy         = (task.side == OrderSide::BUY);
                res.ts_recv_ns     = tsc_ns();
                if (task.ts_send_ns != 0 && res.ts_recv_ns > task.ts_send_ns)   // [PHASE3-KS] MAX_LATENCY: gonderim->cevap
                    ctx.order_rtt_ns.store(res.ts_recv_ns - task.ts_send_ns, std::memory_order_relaxed);

                if (resp.success) {
                    res.exchange_order_id = resp.order_id;
                    res.order_id          = resp.order_id;

                    if (resp.status == "FILLED") {
                        res.status      = OrderStatus::FILLED;
                        res.filled_qty  = resp.fill_qty > 0.0 ? resp.fill_qty : task.qty;
                        res.avg_price   = resp.fill_price > 0.0 ? resp.fill_price : task.price;
                        ctx.logger.info("ORDER_THREAD",
                            "Emir aninda DOLDU: id=", resp.order_id);

                    } else if (resp.status == "PARTIALLY_FILLED") {
                        // [V13-SPSC-OWN]: Kendi tasks ring'ine YAZMİYOR
                        // Strategy thread awaiting_fill=true görünce 1s'te poll atar
                        res.status     = OrderStatus::PARTIALLY_FILLED;
                        res.filled_qty = resp.fill_qty;
                        res.avg_price  = resp.fill_price;
                        res.remaining_qty = std::max(0.0, task.qty - resp.fill_qty);   // [PHASE2-FSM] 0 kalirsa normalize PARTIAL'i FILLED yapiyordu
                        ctx.logger.info("ORDER_THREAD",
                            "Emir KISMI DOLDU: id=", resp.order_id,
                            " -- strategy thread polling devralacak");

                    } else {
                        // PENDING/NEW → strategy thread 1s'te poll atar
                        res.status = OrderStatus::PENDING;
                        ctx.logger.info("ORDER_THREAD",
                            "Emir beklemede (", resp.status,
                            ") -- strategy thread polling devralacak: id=", resp.order_id);
                    }
                } else {
                    res.status = OrderStatus::ERROR;
                    // Kısa hata kodu (16B limit)
                    // [FIX-P36-C15] Belirsiz sonuc (transport/timeout, 5xx, kodsuz 4xx, 2xx orderId'siz, istisna,
                    // -1006/-1007) ONCE ele alinir: emir borsada olabilir. "NET_" oneki is_ambiguous_error ile
                    // dispatch'te UNKNOWN + kill switch + origClientOrderId POLL zincirini calistirir.
                    if (resp.outcome == kusak::SendOutcome::NOT_SENT) {
                        // [FIX-P37-C19A] curl kaniti: istegin tek bayti bile gonderilmedi -> kesin "gitmedi".
                        // "NET_" oneki YOK: is_ambiguous_error false -> REJECTED; UNKNOWN / kill switch yok.
                        constexpr char e[] = "NOT_SENT";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.error("ORDER_THREAD", "EMIR GONDERILMEDI (baglanti asamasi, istek bayti yok) http=",
                                         resp.http_status, (task.kind == TaskKind::EXIT ? " kind=EXIT" : " kind=SEND"),
                                         " govde='", body_snippet(resp.raw_body), "'");   // [FIX-P37-C20]
                    } else if (resp.outcome == kusak::SendOutcome::AMBIGUOUS) {
                        constexpr char e[] = "NET_UNKNOWN";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.critical("ORDER_THREAD", "BELIRSIZ EMIR SONUCU (emir borsada olabilir) http=",
                                            resp.http_status, " code=", resp.error.code,
                                            (task.kind == TaskKind::EXIT ? " kind=EXIT" : " kind=SEND"),
                                            " govde='", body_snippet(resp.raw_body), "'");   // [FIX-P37-C20]
                    } else if (resp.error.is_rate_limit()) {
                        constexpr char e[] = "RATE_LIMIT";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.warn("ORDER_THREAD", "RATE LIMIT (-1003)");
                    } else if (resp.error.is_bad_api_key()) {
                        constexpr char e[] = "BAD_API_KEY";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.critical("ORDER_THREAD",
                            "GECERSIZ API KEY -- KILL-SWITCH AKTIF");
                        ctx.kill_switch.store(true, std::memory_order_seq_cst);
                    } else if (resp.error.is_timestamp()) {
                        constexpr char e[] = "BAD_TIMESTAMP";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.critical("ORDER_THREAD",
                            "ZAMAN DAMGASI HATASI (-1021) -- KILL-SWITCH AKTIF");
                        ctx.kill_switch.store(true, std::memory_order_seq_cst);
                    } else if (resp.error.is_invalid_signature()) {
                        constexpr char e[] = "BAD_SIGNATURE";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.critical("ORDER_THREAD",
                            "GECERSIZ IMZA (-1022) -- KILL-SWITCH AKTIF");
                        ctx.kill_switch.store(true, std::memory_order_seq_cst);
                    } else if (resp.error.is_min_notional()) {
                        constexpr char e[] = "MIN_NOTIONAL";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.error("ORDER_THREAD", "MIN_NOTIONAL (-1013)");
                    } else if (resp.error.is_percent_price()) {
                        constexpr char e[] = "PCT_PRICE";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                        ctx.logger.warn("ORDER_THREAD", "PERCENT_PRICE (-1111)");
                    } else {
                        // [FIX-P36-C15] Buraya yalniz kesin retler (LOCAL_REJECT / EXCHANGE_REJECT) gelir.
                        // Eski "code==0 -> NET_UNKNOWN" dali ulasilamazdi; belirsizlik artik resp.outcome ile ustte.
                        constexpr char e[] = "ORDER_ERR";
                        res.error_msg.set(e, static_cast<uint8_t>(sizeof(e)-1));
                    }
                    const auto err_desc = resp.error.describe();
                    ctx.logger.error("ORDER_THREAD", "Emir hatasi: ",
                        std::string_view(err_desc.data(), std::min<size_t>(err_desc.size(), 80)));
                }

                (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
            }

        } else {
            // ── [V13-BACKOFF] Adaptif bekleme — spec §2 order_loop ──────────
            if (idle < 64) {
                _mm_pause();
            } else if (idle < 256) {
                for (int i = 0; i < 16; ++i) _mm_pause();
            } else {
                tsc_pause_cycles(500); // ~0.5µs
            }
            ++idle;
        }
    }

    // Graceful shutdown: kalan görevleri tükelt
    OrderTask drain_task{};
    while (ch.pop_task(drain_task)) {
        // Kalan SEND/EXIT emirlerini iptal edilmiş sonuç olarak bildir
        OrderResult res{};
        res.order_id       = drain_task.order_id;
        res.local_order_id = drain_task.local_order_id;
        res.status         = OrderStatus::REJECTED;   // [PHASE2-FSM] borsaya hic gitmedi
        res.ts_recv_ns     = tsc_ns();
        res.is_exit        = drain_task.is_exit;
        (void)deliver_result(ctx, ch, res);   // [FIX-P36-C17]
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-SHUTDOWN-DEADLOCK] join_with_timeout
//
// std::thread'in yerlesik bir "sureli join" ozelligi yok -- join() cagirdiginda
// hedef thread bitene kadar SINIRSIZ bekler. Eger o thread bir yerde (bloklayici
// bir syscall, sonsuz dongu, vs.) takili kalmissa, ana thread de sonsuza kadar
// takilir -- tam olarak yasadigimiz "Ctrl+C calismiyor" sorunu.
//
// Cozum: hedef thread'i (t) BASKA bir "bekci" (watchdog) thread'e devrediyoruz
// (std::move ile sahiplik tasiniyor). Bekci, gercek join()'i kendi uzerinde
// yapar. Ana thread ise bekci'nin bitirdigini gosteren bir bayragi, EN FAZLA
// `timeout` suresi kadar bekler:
//   - Bekci zamaninda biterse: bekci'yi de join edip normal devam ederiz.
//   - Bekci zamaninda bitmezse: bekci'yi detach ederiz (ana thread ARTIK ona
//     dokunmaz) ve devam ederiz. Hedef thread er ya da gec biterse bekci de
//     sessizce biter; hic bitmezse bile process sonlandiginda isletim sistemi
//     tum thread'leri zorla temizler -- cokme ya da kaynak sizintisi olmaz.
//
// ONEMLI: Ana thread, timeout sonrasi ASLA orijinal `t` thread'ine dokunmaz
// (ne join ne detach cagirir) -- bu, std::thread nesnesi uzerinde es zamanli
// iki islemden (veri yarisi/UB) kacinmak icin BILINCLI bir tasarim kararidir.
// Sahiplik tamamen bekci'ye devredilmis olur.
// ─────────────────────────────────────────────────────────────────────────────
static void join_with_timeout(std::thread t,
                               std::chrono::milliseconds timeout,
                               const char* name) noexcept
{
    if (!t.joinable()) return;

    auto finished = std::make_shared<std::atomic<bool>>(false);

    std::thread watchdog([th = std::move(t), finished]() mutable {
        th.join();
        finished->store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!finished->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (finished->load(std::memory_order_acquire)) {
        watchdog.join();
    } else {
        std::fprintf(stderr,
            "[SHUTDOWN] UYARI: '%s' thread'i %lldms icinde kapanamadi -- "
            "arka planda birakiliyor, program yine de devam edecek.\n",
            name, static_cast<long long>(timeout.count()));
        std::fflush(stderr);
        watchdog.detach();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-P1-STARTUP] Soguk baslangic uzlastirmasi (reconciliation)
// Kor MARKET SELL YOKTUR. Karar order_guard.hpp::startup_decision() verir:
//   kilit -> yetim emirler -> bakiye (dust / adopt / flatten).
// Flatten = YALNIZ SELL, qty <= serbest base, LIMIT+IOC, fiyat = bid*(1-slip) (slippage SINIRLI),
// kademeli (30/60/120 bps) ve yakinsamazsa baslangic REDDEDILIR (fail-closed).
// Tum HTTP cagrilari istemcideki merkezi RateGate'ten gecer (ban sirasinda aga cikmaz).
// ─────────────────────────────────────────────────────────────────────────────
struct SuMarketInfo { double tick = 0.0; double min_notional = 0.0; };
struct SuBalances   { bool ok = false; double quote_free = 0.0; double base_free = 0.0; double base_locked = 0.0; };

static bool su_num(std::string_view body, const char* key, double& out) noexcept {
    std::string_view num{};
    if (!json_find_number_view(body, key, num)) return false;
    out = 0.0;
    parse_double_sv(num, out);
    return out > 0.0 && std::isfinite(out);
}

static bool su_fetch_market_info(StrategyContext& ctx, bool futures, SuMarketInfo& mi) {
    const std::string& sym = ctx.cfg.symbol;
    const std::string q = "symbol=" + sym;
    auto r = futures ? ctx.http_client->get_public("/fapi/v1/exchangeInfo", "", 1)
                     : ctx.http_client->get_public("/api/v3/exchangeInfo", q.c_str(), 20);
    if (!r.ok()) return false;
    std::string_view body{r.body};
    const std::string key = "\"symbol\":\"" + sym + "\"";
    const size_t p = body.find(key);
    if (p == std::string_view::npos) return false;
    std::string_view v = body.substr(p);
    const size_t nx = v.find("\"symbol\":\"", key.size());
    if (nx != std::string_view::npos) v = v.substr(0, nx);
    if (!su_num(v, "\"tickSize\"", mi.tick)) return false;
    if (!su_num(v, "\"minNotional\"", mi.min_notional) &&
        !su_num(v, "\"notional\"",    mi.min_notional)) return false;   // fail-closed: filtre yoksa baslama
    return true;
}

static bool su_fetch_bid(StrategyContext& ctx, bool futures, double& bid) {
    const std::string q = "symbol=" + ctx.cfg.symbol;
    auto r = ctx.http_client->get_public(futures ? "/fapi/v1/ticker/bookTicker" : "/api/v3/ticker/bookTicker",
                                         q.c_str(), 2);
    if (!r.ok()) return false;
    return su_num(std::string_view{r.body}, "\"bidPrice\"", bid);
}

static int su_count_open_orders(StrategyContext& ctx, bool futures, bool& ok) {
    const std::string q = "symbol=" + ctx.cfg.symbol;
    auto r = ctx.http_client->get_signed(futures ? "/fapi/v1/openOrders" : "/api/v3/openOrders", q.c_str(), 6);
    ok = r.ok() && !r.body.empty() && r.body[0] == '[';
    if (!ok) return 0;
    int n = 0;
    for (size_t pos = r.body.find("\"orderId\""); pos != std::string::npos;
         pos = r.body.find("\"orderId\"", pos + 9)) ++n;
    return n;
}

static bool su_cancel_all_open_orders(StrategyContext& ctx, bool futures) {
    const std::string q = "symbol=" + ctx.cfg.symbol;
    auto r = ctx.http_client->delete_signed(futures ? "/fapi/v1/allOpenOrders" : "/api/v3/openOrders", q.c_str());
    return r.ok();
}

static SuBalances su_balances(StrategyContext& ctx, const std::string& quote_asset, const std::string& base_asset) {
    SuBalances out;
    try {
        const auto bals = ctx.http_client->fetch_account_balances();
        for (const auto& b : bals) {
            if (b.asset == quote_asset) { out.quote_free = b.free; out.ok = true; }
            else if (b.asset == base_asset) { out.base_free = b.free; out.base_locked = b.locked; }
        }
    } catch (...) { out.ok = false; }
    return out;
}

// Reduce-only flatten: asla BUY, asla MARKET, asla serbest bakiyeden fazla qty.
static bool su_flatten_reduce_only(StrategyContext& ctx, const std::string& quote_asset,
                                   const std::string& base_asset, bool futures, const SuMarketInfo& mi) {
    static constexpr double SLIP_BPS[3] = {30.0, 60.0, 120.0};   // en kotu satis = bid*(1-%1.2)
    const std::string& sym = ctx.cfg.symbol;

    for (int i = 0; i < 3; ++i) {
        if (g_stop.load(std::memory_order_acquire)) return false;

        const SuBalances b = su_balances(ctx, quote_asset, base_asset);
        double bid = 0.0;
        if (!b.ok || !su_fetch_bid(ctx, futures, bid)) {
            ctx.logger.critical("STARTUP", "FLATTEN: bakiye/bid okunamadi -- fail-closed.");
            return false;
        }
        const double qty = ctx.filter_validator.round_qty(sym, b.base_free);
        // [FIX-FAZ0-3] goreli tolerans + isfinite, fail-closed (bkz. notional_ok)
        if (!(qty > 0.0) || qty > b.base_free || !notional_ok(qty, bid, mi.min_notional)) {
            ctx.logger.info("STARTUP", "FLATTEN: kalan pozisyon dust/sifir -- tamam.");
            return true;
        }
        const double px = kusak::guard::floor_to_step(bid * (1.0 - SLIP_BPS[i] / 10000.0), mi.tick);
        if (!(px > 0.0) || !kusak::guard::is_multiple(px, mi.tick) || !std::isfinite(qty * px)) {
            ctx.logger.critical("STARTUP", "FLATTEN: gecersiz fiyat/qty px=", px, " qty=", qty);
            return false;
        }

        kusak::exchange::RestOrderClient::OrderRequest req;
        req.symbol        = sym;
        req.side          = "SELL";                 // reduce-only: yalniz azaltan yon
        req.type          = "LIMIT";                // MARKET YASAK
        req.time_in_force = "IOC";                  // kalan kisim bekleyen emir olarak kalmaz
        req.quantity      = qty;
        req.price         = px;
        {
            using namespace std::chrono;
            req.client_oid = "KUSAK_FLAT_" + std::to_string(
                duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count()) + "_" + std::to_string(i);
        }
        ctx.logger.warn("STARTUP", "FLATTEN deneme ", i + 1, "/3 SELL IOC qty=", qty, " px=", px,
                        " (bid=", bid, " slip_bps=", SLIP_BPS[i], ")");
        auto resp = ctx.live_order_client->new_order(req, px);
        if (resp.success) ctx.logger.info("STARTUP", "FLATTEN emri kabul: id=", resp.order_id);
        else              ctx.logger.error("STARTUP", "FLATTEN emri reddedildi: ", resp.error.describe());

        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // bakiye yayilimi (yalniz boot yolu)
    }

    const SuBalances b = su_balances(ctx, quote_asset, base_asset);
    double bid = 0.0;
    if (!b.ok || !su_fetch_bid(ctx, futures, bid)) return false;
    const bool flat = (b.base_free + b.base_locked) * bid < mi.min_notional;
    if (!flat)
        ctx.logger.critical("STARTUP", "FLATTEN yakinsamadi: kalan base=", b.base_free + b.base_locked,
                            " -- limitli kademeler tukendi, MARKET'e DUSULMEZ.");
    return flat;
}

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-STARTUP-GATE] kusak::startup -- order_guard.hpp'nin StartupProbe kapisini suren, hook-tabanli
// (sanal cagri / std::function / heap YOK) uzlastirma dongusu. Bakiye okumasi YALNIZ open_orders==0
// KANITLANDIKTAN sonra yapilir; dogrulama aninda yeni emir cikarsa (TOCTOU) sonuc ilerleme sayilmaz.
// ─────────────────────────────────────────────────────────────────────────────
namespace kusak::startup {

template <class H>
concept Hooks = requires(H& h, uint32_t round, kusak::guard::StartAction a, int n) {
    { h.lock_file_exists() }        -> std::convertible_to<bool>;
    { h.query_open_orders() }       -> std::same_as<std::optional<int>>;     // nullopt = sorgu BASARISIZ
    { h.cancel_all_open_orders() }  -> std::convertible_to<bool>;            // sonuc yalniz bilgi; dogrulama recheck ile
    { h.read_base_value_usd() }     -> std::same_as<std::optional<double>>;  // su_balances sarmalayicisi
    { h.on_round(round, a, n) };                                             // log/telemetri
};

struct StartupOutcome {
    kusak::guard::StartAction  action;
    kusak::guard::StartupProbe probe;
    uint32_t                   cancel_rounds;
};

// Yalniz bu 4 sonuc islem/devralma yoluna girebilir; digerleri = BASLAMA.
[[nodiscard]] constexpr bool may_proceed(kusak::guard::StartAction a) noexcept {
    using A = kusak::guard::StartAction;
    return a == A::CLEAN_START || a == A::ADOPT_POSITION || a == A::IGNORE_DUST || a == A::FLATTEN_REDUCE_ONLY;
}

namespace detail {
// OS'a donmeden SINIRLI bekleme (REST iptalinin borsada yansimasi icin).
inline void bounded_spin_us(uint32_t us) noexcept {
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + std::chrono::microseconds(us);
    uint32_t spins = 0;
    while (clk::now() < deadline) kusak::spin::adaptive_spin(spins);
}
} // namespace detail

// Hook'lar hata durumunda nullopt/false dondurmeli (istisna firlatmamali). Her belirsizlik FAIL-CLOSED.
// Garanti: read_base_value_usd() YALNIZ startup_orders_clear(probe)==true iken cagrilir.
template <Hooks H>
[[nodiscard]] StartupOutcome startup_reconcile(H& h, double min_notional, double pos_cap_usd,
                                               uint32_t settle_us = 50'000) noexcept
{
    using kusak::guard::StartAction;
    constexpr uint32_t MAX_ROUNDS = kusak::guard::MAX_STARTUP_CANCEL_ATTEMPTS;   // 3 tur

    kusak::guard::StartupProbe p{};                    // varsayilanlar fail-closed
    p.min_notional = min_notional;
    p.pos_cap_usd  = pos_cap_usd;

    // GATE 0: kilit
    p.lock_file_exists = static_cast<bool>(h.lock_file_exists());
    if (p.lock_file_exists) return {StartAction::REFUSE_LOCKED, p, 0};

    const auto refresh_orders = [&]() noexcept {       // HER cagri TAZE REST sorgusu
        const std::optional<int> n = h.query_open_orders();
        p.open_orders_verified = n.has_value() && *n >= 0;
        p.open_orders          = p.open_orders_verified ? *n : -1;
    };

    // GATE 1: open_orders == 0 KANITLANANA dek dongu. Ust sinir: MAX_ROUNDS iptal + MAX_ROUNDS dogrulanamayan sorgu.
    uint32_t unverified = 0;
    for (uint32_t guard_iter = 0; guard_iter < 2 * MAX_ROUNDS + 2; ++guard_iter) {
        refresh_orders();
        if (kusak::guard::startup_orders_clear(p)) break;

        const StartAction a = kusak::guard::startup_decision(p);
        h.on_round(p.cancel_attempts, a, p.open_orders);

        if (a == StartAction::CANCEL_OPEN_ORDERS_THEN_RECHECK) {
            ++p.cancel_attempts;                       // karar fonksiyonu 3'te CANCEL_STUCK'a doner
            (void)h.cancel_all_open_orders();          // basari iddiasi YOK: dogrulama bir sonraki taze sorgu
            detail::bounded_spin_us(settle_us);
            continue;
        }
        if (a == StartAction::REFUSE_ORDERS_UNVERIFIED && ++unverified < MAX_ROUNDS) {
            detail::bounded_spin_us(settle_us);        // gecici REST hatasi: sinirli yeniden dene
            continue;
        }
        return {a, p, p.cancel_attempts};              // LOCKED / CANCEL_STUCK / UNVERIFIED -> BASLAMA
    }
    if (!kusak::guard::startup_orders_clear(p))        // dongu butcesi bitti, kanit yok
        return {StartAction::REFUSE_CANCEL_STUCK, p, p.cancel_attempts};

    // GATE 2: bakiye. Buraya YALNIZ orders_clear ile gelinir; yine de savunma amacli yeniden kontrol.
    if (!kusak::guard::startup_may_read_balance(p))
        return {StartAction::REFUSE_ORDERS_UNVERIFIED, p, p.cancel_attempts};

    const std::optional<double> bv = h.read_base_value_usd();
    p.balance_verified = bv.has_value();
    p.base_value_usd   = bv.value_or(std::numeric_limits<double>::quiet_NaN());

    // TOCTOU: bakiye okunurken yeni emir olusmus olabilir -> karardan ONCE son taze dogrulama.
    refresh_orders();
    const StartAction fin = kusak::guard::startup_decision(p);   // open_orders>0 ise CANCEL/STUCK doner
    h.on_round(p.cancel_attempts, fin, p.open_orders);
    return {fin, p, p.cancel_attempts};
}

} // namespace kusak::startup

// [FIX-STARTUP-HOOKS] kusak::startup::Hooks concept'ini saglayan sarmalayici. Tum HTTP istemcideki RateGate'ten gecer.
// Istisna gecirmez: her hata nullopt/false'a cevrilir (startup_reconcile<H> noexcept'tir, belirsizlik FAIL-CLOSED).
struct StartupHooks {
    StrategyContext&   ctx;
    const std::string& quote_asset;
    const std::string& base_asset;
    bool               futures;

    // Yalniz GATE 2 penceresinde (orders_clear dogrulanmis) doldurulur; caller sermaye icin buradan okur.
    SuBalances last_bal{};
    double     last_bid        = 0.0;
    double     last_base_value = 0.0;

    void reset_snapshot() noexcept { last_bal = SuBalances{}; last_bid = 0.0; last_base_value = 0.0; }

    // ks_boot_check() kilit dosyasini ctx.kill_switch'e yansitir.
    [[nodiscard]] bool lock_file_exists() noexcept {
        return ctx.kill_switch.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<int> query_open_orders() noexcept {
        try {
            bool ok = false;
            const int n = su_count_open_orders(ctx, futures, ok);
            if (!ok || n < 0) return std::nullopt;
            return n;
        } catch (...) { return std::nullopt; }
    }

    [[nodiscard]] bool cancel_all_open_orders() noexcept {
        try { return su_cancel_all_open_orders(ctx, futures); }
        catch (...) { return false; }
    }

    // su_balances + bid -> base_value_usd. startup_reconcile<H> bunu YALNIZ open_orders==0 kanitlandiktan sonra cagirir.
    [[nodiscard]] std::optional<double> read_base_value_usd() noexcept {
        try {
            reset_snapshot();
            const SuBalances b = su_balances(ctx, quote_asset, base_asset);
            if (!b.ok) return std::nullopt;
            const double base_total = b.base_free + b.base_locked;
            if (!std::isfinite(base_total) || base_total < 0.0) return std::nullopt;
            double bid = 0.0, value = 0.0;
            if (base_total > 0.0) {
                if (!su_fetch_bid(ctx, futures, bid) || !(bid > 0.0) || !std::isfinite(bid)) return std::nullopt;
                value = base_total * bid;
                if (!std::isfinite(value)) return std::nullopt;
            }
            last_bal = b; last_bid = bid; last_base_value = value;
            return value;
        } catch (...) { return std::nullopt; }
    }

    void on_round(uint32_t cancel_attempts, kusak::guard::StartAction a, int open_orders) noexcept {
        ctx.logger.info("STARTUP", "cancel_attempts=", cancel_attempts, " karar=", static_cast<int>(a),
                        " open_orders=", open_orders);
    }
};
static_assert(kusak::startup::Hooks<StartupHooks>, "StartupHooks concept'i saglamali");

// true => baslayabilir. false => main() 1 dondurur.
// Dis dongu ("epoch", en fazla 3): her epoch kusak::startup::startup_reconcile ile BASTAN, taze dogrulama yapar.
// Hicbir dal "kor devam" etmez: yalniz may_proceed() olan sonuclar ilerler; TOCTOU'da yeni emir bulunursa
// (CANCEL_OPEN_ORDERS_THEN_RECHECK) epoch tekrarlanir; flatten sonrasi da tum kapilar yeniden gecilir.
static bool startup_reconcile(StrategyContext& ctx, const std::string& quote_asset, bool futures) {
    if (ctx.cfg.dry_run || !ctx.http_client || !ctx.live_order_client.has_value()) return true;

    const std::string& sym = ctx.cfg.symbol;
    std::string base_asset;
    if (sym.size() > quote_asset.size() &&
        sym.compare(sym.size() - quote_asset.size(), quote_asset.size(), quote_asset) == 0)
        base_asset = sym.substr(0, sym.size() - quote_asset.size());
    if (base_asset.empty()) {
        ctx.logger.critical("STARTUP", "Base asset sembolden turetilemedi: ", sym);
        return false;
    }

    SuMarketInfo mi;
    if (!su_fetch_market_info(ctx, futures, mi)) {
        ctx.logger.critical("STARTUP", "tickSize/minNotional okunamadi -- fail-closed.");
        return false;
    }

    using A = kusak::guard::StartAction;
    const double pos_cap = ctx.cfg.max_position_usd;      // 0/NaN => guard FLATTEN (fail-closed)
    StartupHooks hooks{ctx, quote_asset, base_asset, futures};

    constexpr int MAX_EPOCHS = 3;
    for (int epoch = 0; epoch < MAX_EPOCHS; ++epoch) {
        if (g_stop.load(std::memory_order_acquire)) return false;
        hooks.reset_snapshot();

        const kusak::startup::StartupOutcome out =
            kusak::startup::startup_reconcile(hooks, mi.min_notional, pos_cap);
        const A act = out.action;
        ctx.logger.info("STARTUP", "epoch=", epoch, " karar=", static_cast<int>(act),
                        " cancel_rounds=", out.cancel_rounds, " open_orders=", out.probe.open_orders,
                        " base_value_usd=", hooks.last_base_value, " quote_free=", hooks.last_bal.quote_free);

        switch (act) {
        // ── KESIN RED: motoru durdur ───────────────────────────────────────────
        case A::REFUSE_LOCKED:
            ctx.logger.critical("STARTUP", "KILIT aktif -- baslangic REDDEDILDI (restart ile atlatilamaz).");
            return false;
        case A::REFUSE_ORDERS_UNVERIFIED:
            ctx.logger.critical("STARTUP", "acik emir sayisi DOGRULANAMADI -- fail-closed.");
            return false;
        case A::REFUSE_CANCEL_STUCK:
            ctx.logger.critical("STARTUP", "yetim emirler iptal edilemedi (", out.cancel_rounds,
                                " tur) -- fail-closed, borsada elle kontrol edin.");
            return false;
        case A::REFUSE_BALANCE_UNVERIFIED:
            ctx.logger.critical("STARTUP", "bakiye/bid DOGRULANAMADI -- fail-closed.");
            return false;

        // ── TOCTOU: bakiye penceresinde yeni emir cikti -> bu sonuc ile ILERLENEMEZ, taze epoch ──
        case A::CANCEL_OPEN_ORDERS_THEN_RECHECK:
            ctx.logger.warn("STARTUP", "TOCTOU: dogrulama aninda yeni acik emir (n=", out.probe.open_orders,
                            ") -- epoch ", epoch + 1, "/", MAX_EPOCHS, " ile bastan.");
            continue;

        // ── Pozisyon dallari (orders==0 + bakiye KANITLI) ──────────────────────
        case A::ADOPT_POSITION:
            if (hooks.last_bal.quote_free >= ctx.cfg.min_free_usdt) {
                // [FIX-P35] C13: ADOPT pozisyonu yerel kopyaya kurulur (adopt_decision/adopt_position).
                const double adopt_step = ctx.filter_validator.step_size_of(ctx.cfg.symbol);
                const AdoptDecision ad = adopt_decision(futures, hooks.last_bal.base_free,
                                                                      hooks.last_bal.base_locked, adopt_step);
                if (ad == AdoptDecision::REFUSE_FUTURES) {
                    ctx.logger.critical("STARTUP", "FUTURES ADOPT desteklenmiyor: yon/miktar bilinmiyor; fail-closed");
                    return false;
                }
                if (ad == AdoptDecision::REFUSE_LOCKED) {
                    ctx.logger.critical("STARTUP", "ADOPT reddedildi: base_locked>0, acik emir yok kaniti ile celisiyor");
                    return false;
                }
                const double adopt_qty = ctx.filter_validator.floor_qty(ctx.cfg.symbol, hooks.last_bal.base_free);
                const double ref_px    = hooks.last_bid;
                if (!adopt_position(ctx, PositionSide::LONG, adopt_qty, ref_px)) {
                    ctx.logger.critical("STARTUP", "ADOPT basarisiz: gecersiz qty/ref_px qty=", adopt_qty,
                                        " ref_px=", ref_px, " -- fail-closed");
                    return false;
                }
                ctx.logger.warn("STARTUP", "Mevcut pozisyon cap icinde, kontrollu devam: ", base_asset,
                                " deger_usd=", hooks.last_base_value, " qty=", adopt_qty,
                                " ref_px=", ref_px, " (entry_price=referans, gercek giris degil)");
                ctx.current_capital = hooks.last_bal.quote_free;
                fw_publish_exposure(ctx, FwVerify::EVIDENCE);   // current_capital degisti: free_quote yeniden yayinlanir (EVIDENCE korunur)
                return true;
            }
            [[fallthrough]];   // quote yetersiz => islem acilamaz => reduce-only flatten
        case A::FLATTEN_REDUCE_ONLY:
            // [FIX-P36-C18] su_flatten_reduce_only SPOT semantigindedir (/api/v3/account bakiyesi, sabit SELL,
            // reduceOnly yok). FUTURES'ta yon/miktar bilinmiyor: dogrudan FLATTEN karari ve ADOPT quote-yetersiz
            // fallthrough'u dahil FUTURES baslatmasi REDDEDILIR (FUTURES ADOPT karariyla tutarli; fail-closed).
            if (futures) {
                ctx.logger.critical("STARTUP", "FUTURES reduce-only FLATTEN desteklenmiyor: yon/miktar bilinmiyor; "
                                    "fail-closed, baslatma reddedildi. deger_usd=", hooks.last_base_value);
                return false;
            }
            ctx.logger.warn("STARTUP", "Reduce-only FLATTEN: ", base_asset, " deger_usd=", hooks.last_base_value,
                            " pos_cap=", pos_cap);
            if (!su_flatten_reduce_only(ctx, quote_asset, base_asset, futures, mi)) return false;
            continue;          // flatten sonrasi TUM kapilar (emir + bakiye + TOCTOU) yeniden gecilir

        case A::IGNORE_DUST:
        case A::CLEAN_START:
            if (!(hooks.last_bal.quote_free >= ctx.cfg.min_free_usdt)) {
                ctx.logger.critical("STARTUP", "KRITIK: serbest ", quote_asset, " (", hooks.last_bal.quote_free,
                                    ") min_free_usdt (", ctx.cfg.min_free_usdt, ") altinda.");
                return false;
            }
            ctx.current_capital = hooks.last_bal.quote_free;
            fw_publish_exposure(ctx, FwVerify::EVIDENCE);   // [FIX-P0-03] CLEAN/DUST: pozisyon yok -> kural 1 ile verified=false; sonraki onayli giris FILLED ile true olur
            return true;
        }

        // Switch disi (bilinmeyen enum degeri): ASLA ilerleme.
        ctx.logger.critical("STARTUP", "taninmayan karar=", static_cast<int>(act), " -- fail-closed.");
        return false;
    }
    ctx.logger.critical("STARTUP", "Uzlastirma yakinsamadi (", MAX_EPOCHS, " epoch) -- fail-closed.");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// [V13-CLOCK]: Tüm interval ölçümleri mono_ns() ile
// [V13-HOOKS]: Ana döngüye enforce_timeouts + monitor_drops eklendi
// [V13-SPSC]:  tasks_cv.notify_all() kaldırıldı (CV yok, stop atomik yeterli)
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── [TIER-1 TSC] Kalibrasyon — main başında bir kez ─────────────────────
    {
        auto cal = calibrate_tsc();
        g_tsc_ns_mult = cal.ns_mult;
        std::fprintf(stderr, "[TSC] Calibrated: ns_mult=%llu tsc_per_ns=%llu\n",
            (unsigned long long)cal.ns_mult, (unsigned long long)cal.tsc_per_ns);
    }

    const std::string config_path = (argc > 1) ? argv[1] : "kusak_config.json";
    kusak::config::ConfigManager config_mgr(config_path);
    kusak::config::KusakConfig   cfg;

    if (!config_mgr.load(cfg)) {
        std::cerr << "[main] Config bulunamadi -- varsayilan degerlerle devam.\n";
        kusak::config::ConfigManager::write_example(config_path, cfg);
        std::cerr << "[main] Ornek config yazildi: " << config_path << "\n";
    }

    // =================================================================
    // KUSAK V13: ÇEVRESEL DEĞİŞKEN (ENV) OVERRIDE MEKANİZMASI
    // =================================================================
    if (const char* env_api_key = std::getenv("API_KEY")) {
        if (std::strlen(env_api_key) > 0) cfg.api_key = env_api_key;
    }
    if (const char* env_api_secret = std::getenv("API_SECRET")) {
        if (std::strlen(env_api_secret) > 0) cfg.api_secret = env_api_secret;
    }
    if (const char* env_testnet = std::getenv("TESTNET")) {
        std::string tn_str = env_testnet;
        if (tn_str == "true" || tn_str == "1" || tn_str == "TRUE" || tn_str == "EVET") {
            cfg.testnet = true;
        } else if (tn_str == "false" || tn_str == "0" || tn_str == "FALSE" || tn_str == "HAYIR") {
            cfg.testnet = false;
        }
    }
    // =================================================================

    const bool        futures_usdtm = (cfg.market == "FUTURES");
    const std::string quote_asset   = cfg.quote_asset.empty() ? "USDT" : cfg.quote_asset;

    // ── Erken guard — segfault koruması ──────────────────────────────────────────
    if (!cfg.dry_run && (cfg.api_key.empty() || cfg.api_secret.empty())) {
        std::cerr << "[HATA] dry_run=false ama api_key/api_secret bos! "
                  << "Lutfen kusak_config.json dosyasina api_key ve api_secret ekleyin.\n";
        return 1;
    }
    if (cfg.symbol.empty()) {
        std::cerr << "[HATA] Config'de 'symbol' alani bos!\n";
        return 1;
    }
    if (cfg.market.empty()) {
        std::cerr << "[HATA] Config'de 'market' alani bos! (SPOT veya FUTURES)\n";
        return 1;
    }
    fprintf(stderr, "[GUARD] Config OK: symbol=%s market=%s dry_run=%s\n",
        cfg.symbol.c_str(), cfg.market.c_str(), cfg.dry_run ? "true" : "false");

    auto ctx_ptr = std::make_unique<StrategyContext>(cfg);
    StrategyContext& ctx = *ctx_ptr;

    ctx.logger.info("MAIN", "KUSAK V13 HFT baslatiliyor [TIER-1 SPEC]",
        " sembol=",  cfg.symbol,
        " market=",  cfg.market,
        " dry_run=", cfg.dry_run ? "EVET" : "HAYIR",
        " testnet=", cfg.testnet ? "EVET" : "HAYIR");

    if (cfg.dry_run) {
        if (ctx.current_capital <= 0.0) {
            ctx.logger.critical("MAIN",
                "[DRY-RUN] initial_capital=0! kusak_config.json'da initial_capital girin.");
            return 1;
        }
        ctx.logger.info("MAIN",
            "[DRY-RUN] Canli bakiye sorgulanmiyor. Sermaye=",
            ctx.current_capital, " ", quote_asset);
    }

    if (true) {
        if (cfg.api_key.empty() || cfg.api_secret.empty()) {
            std::cerr << "[main] HATA: dry_run=false ama api_key/api_secret bos!\n";
            return 1;
        }
        kusak::net::BinanceHttpClient::Config hc;
        hc.api_key            = cfg.api_key.c_str();
        hc.api_secret         = cfg.api_secret.c_str();
        hc.testnet            = cfg.testnet;
        hc.market             = cfg.market.c_str();
        hc.connect_timeout_ms = 3000;
        hc.request_timeout_ms = 8000;
        hc.recv_window_ms     = static_cast<int>(cfg.recvWindow_ms);
        hc.verify_ssl         = true;
        hc.max_retries        = 3;
        ctx.http_client = std::make_shared<kusak::net::BinanceHttpClient>(hc);
        ctx.logger.info("MAIN", "BinanceHttpClient hazir -- ",
            cfg.testnet ? "TESTNET" : "MAINNET",
            " market=", hc.market);
    }

    std::unique_ptr<kusak::net::ExchangeInfoManager> ei_mgr;
    if (ctx.http_client) {
        ei_mgr = std::make_unique<kusak::net::ExchangeInfoManager>(
            ctx.http_client,
            ctx.filter_validator,
            std::vector<std::string>{cfg.symbol},
            cfg.exchangeinfo_ttl_s,
            [&ctx](const std::string& m){ ctx.logger.error("EXINFO", m); });
        ei_mgr->set_market(futures_usdtm);
        try {
            ei_mgr->start();
            ctx.logger.info("MAIN", "ExchangeInfo yuklendi: ", cfg.symbol);
        } catch (const std::exception& e) {
            ctx.logger.critical("MAIN", "ExchangeInfo fetch basarisiz: ", e.what());
            return 1;
        }
    }

    if (!cfg.dry_run && ctx.http_client) {
        ctx.live_order_client.emplace(
            ctx.filter_validator,
            ctx.http_client->make_signed_post_func());
        ctx.live_order_client->set_market_endpoints(futures_usdtm);
        ctx.live_order_client->set_account_http_func(
            ctx.http_client->make_signed_get_func());
        ctx.logger.info("MAIN", "RestOrderClient CANLI",
            " endpoints=", futures_usdtm ? "FUTURES(/fapi)" : "SPOT(/api/v3)");
    }

    if (!cfg.dry_run && ctx.http_client) {
        bool raw_fetched = false;
        std::string raw_body;
        try {
            const std::string acct_ep = futures_usdtm ? "/fapi/v2/account" : "/api/v3/account";
            auto raw_resp = ctx.http_client->get_signed(acct_ep.c_str(), "", 10);
            if (raw_resp.ok()) {
                raw_body    = raw_resp.body;
                raw_fetched = true;
                ctx.logger.info("STARTUP_BANNER",
                    ">>> CANLI BAGLANTI DOGRULANDI <<< ",
                    acct_ep, " HTTP ", raw_resp.status_code);
                if (cfg.log_raw_account_json)
                    ctx.logger.info("ACCOUNT_RAW",
                        acct_ep, " raw (ilk 1000 karakter): ", raw_body.substr(0, 1000));
                const std::string acct_type = json_extract_string(raw_body, "accountType");
                if (!acct_type.empty()) {
                    ctx.logger.info("ACCOUNT", "accountType=", acct_type);
                    if (acct_type == "UNIFIED" || acct_type == "PORTFOLIO_MARGIN")
                        ctx.logger.warn("ACCOUNT",
                            "DIKKAT: Hesap tipi '", acct_type,
                            "' -- PAPI/SAPI endpoint gerekebilir.");
                }
            }
        } catch (const std::exception& e) {
            ctx.logger.warn("ACCOUNT", "Ham account sorgusu exception: ", e.what());
        } catch (...) {
            ctx.logger.warn("ACCOUNT", "Ham account sorgusu bilinmeyen hata");
        }

        double bal_free = 0.0, bal_locked = 0.0;
        bool   bal_ok   = false;
        try {
            auto bals = ctx.http_client->fetch_account_balances();
            for (const auto& b : bals) {
                if (b.asset == quote_asset) {
                    bal_free   = b.free;
                    bal_locked = b.locked;
                    bal_ok     = true;
                    break;
                }
            }
        } catch (const std::exception& e) {
            ctx.logger.error("ACCOUNT", "fetch_account_balances() exception: ", e.what());
        }

        ctx.logger.info("ACCOUNT", quote_asset,
            " bakiye -- free=", bal_free, " locked=", bal_locked);

        // [FIX-P1-STARTUP] Eski "Auto-Recovery" kor MARKET SELL blogu TAMAMEN KALDIRILDI.
        // Dusuk quote / acik pozisyon / yetim emir karari startup_reconcile() (startup_decision) icindedir;
        // ks_boot_check() sonrasi, thread'ler baslamadan once calisir.
        (void)bal_ok;
        ctx.current_capital = bal_free;   // gecici; startup_reconcile() dogrulanmis degerle ustune yazar
        ctx.logger.info("ACCOUNT",
            "Ilk quote okumasi: ", ctx.current_capital, " ", quote_asset);
    }

    ctx.logger.info("MAIN", "UserDataStream devre disi -- REST polling ile takip.");

    // [ARCH-V2-FIX] CloseCallback: void(VPINAlarm,double) -> void(VPINAlarm,int64_t)
    ctx.vpin.set_close_callback([&](kusak::research::VPINAlarm, int64_t) {
        ctx.vpin_force_exit.store(true, std::memory_order_release);
    });

    std::string sym_lower = cfg.symbol;
    for (auto& c : sym_lower) c = static_cast<char>(std::tolower(c));

    kusak::ws::BinanceWsClient::Config ws_cfg;
    ws_cfg.symbol        = sym_lower;
    ws_cfg.enable_depth  = true;
    ws_cfg.enable_ticker = true;
    ws_cfg.enable_trade  = true;
    ws_cfg.verbose       = cfg.verbose;

    auto market_ws = std::make_unique<kusak::ws::BinanceWsClient>(
        ws_cfg,
        nullptr,
        [&ctx](const kusak::ws::BookTicker& bk) noexcept {
            MarketEvent ev;
            ev.tag    = MarketEvent::Tag::BOOK_TICKER;
            ev.ticker = bk;
            ctx.market_ring.try_push(ev);
        },
        [&ctx](const kusak::ws::AggTrade& t) noexcept {
            MarketEvent ev;
            ev.tag   = MarketEvent::Tag::AGG_TRADE;
            ev.trade = t;
            ctx.market_ring.try_push(ev);
        }
    );

    fw_publish_limits(ctx);   // [PHASE1-FW] order thread baslamadan once limitleri yayinla
    ks_boot_check(ctx);   // [PHASE3-KS] kilit dosyasi varsa KILITLI basla (restart ile atlatilamaz)

    // [FIX-P1-STARTUP] Uzlastirma: order/strateji thread'leri HENUZ yok; kilit durumu ks_boot_check'ten sonra okunur.
    if (!startup_reconcile(ctx, quote_asset, futures_usdtm)) {
        ctx.logger.critical("MAIN", "Startup uzlastirmasi basarisiz/reddedildi -- cikiliyor.");
        if (ei_mgr) ei_mgr->stop();
        return 1;
    }

    std::thread market_thread([&]() {
        try { market_ws->run(g_stop); }
        catch (const std::exception& e) {
            ctx.logger.error("WS_MARKET", "Fatal: ", e.what());
        } catch (...) {
            ctx.logger.error("WS_MARKET", "Fatal unknown exception");
        }
    });

    const int strategy_core = read_core_from_cfg_or_env(ctx.cfg, true);
    if (!pin_current_thread_to_core(strategy_core)) {
        ctx.logger.warn("AFFINITY", "strategy_thread pin basarisiz core=", strategy_core);
    } else if (strategy_core >= 0) {
        ctx.logger.info("AFFINITY", "strategy_thread pinned core=", strategy_core);
    }

    std::thread order_thread([&]() {
        const int order_core = read_core_from_cfg_or_env(ctx.cfg, false);
        if (!pin_current_thread_to_core(order_core)) {
            ctx.logger.warn("AFFINITY", "order_thread pin basarisiz core=", order_core);
        } else if (order_core >= 0) {
            ctx.logger.info("AFFINITY", "order_thread pinned core=", order_core);
        }
        run_order_thread(ctx);
    });

    std::thread telemetry_thread([&]() {
        // [TELEMETRY-THREAD-ONLY] flush yalnız bu thread'de: komutla (10s rapor) + kendi 1s zamanlayıcısıyla.
        constexpr uint64_t TELEMETRY_FLUSH_NS = 1'000'000'000ULL;
        uint64_t last_flush_ns = tsc_ns();
        while (!g_stop.load(std::memory_order_acquire)) {
            TelemetryCmd cmd{};
            if (ctx.telemetry_cmd_ring.pop(cmd)) {
                if (cmd == TelemetryCmd::FLUSH_CSV) {
                    ctx.telemetry.flush_to_csv();
                } else if (cmd == TelemetryCmd::STOP) {
                    break;
                }
            } else {
                const uint64_t t_ns = tsc_ns();
                if (t_ns - last_flush_ns >= TELEMETRY_FLUSH_NS) {
                    last_flush_ns = t_ns;
                    ctx.telemetry.flush_to_csv();
                }
                _mm_pause();
            }
        }
        ctx.telemetry.flush_to_csv();
    });

    ctx.logger.info("MAIN", "Sistem hazir. CTRL+C ile durdurabilirsiniz.");

    // ── [V13] Strateji hot loop ───────────────────────────────────────────────
    // [V13-CLOCK]: Tüm interval hesaplamaları mono_ns() ile yapılıyor
    uint64_t last_report_ns       = tsc_ns();
    uint64_t last_reload_ns       = last_report_ns;
    uint64_t last_poll_ns         = 0;
    uint64_t last_poll_skip_warn_ns = 0;   // [FIX-P0-02] POLL uretilmedi uyarisi icin 10 sn throttle
    uint64_t last_timeout_check_ns = last_report_ns;

    constexpr uint64_t RELOAD_INTERVAL_NS  = 1'000'000'000ULL;   // 1s
    constexpr uint64_t REPORT_INTERVAL_NS  = 10'000'000'000ULL;  // 10s
    constexpr uint64_t POLL_INTERVAL_NS    = 1'000'000'000ULL;   // 1s

    constexpr uint32_t BATCH = 128;
    MarketEvent batch[BATCH];

    uint32_t idle_spin = 0;

    while (!g_stop.load(std::memory_order_acquire)) {

        // ── Piyasa verisi işle ────────────────────────────────────────────────
        const uint32_t n = ctx.market_ring.drain(batch, BATCH);
        if (n == 0) {
            if (idle_spin < 64) {
                _mm_pause();
            } else if (idle_spin < 256) {
                for (int i = 0; i < 8; ++i) _mm_pause();
            } else {
                tsc_pause_cycles(200);
            }
            ++idle_spin;
        } else {
            idle_spin = 0;
            for (uint32_t i = 0; i < n; ++i) {
                if (batch[i].tag == MarketEvent::Tag::AGG_TRADE)
                    process_agg_trade(ctx, batch[i].trade);
                else
                    process_book_ticker(ctx, batch[i].ticker);
            }
        }

        // ── [spec §2] NON-BLOCKING emir sonuçlarını işle ─────────────────────
        {
            OrderResult res{};
            while (ctx.order_channel.pop_result(res))
                dispatch_order_result(ctx, res);
        }

        const uint64_t now_ns = tsc_ns();

        // ── POLL emir sorgusu (strateji thread üretici) ───────────────────────
        // [V13-SPSC-OWN]: POLL task'ı order thread değil strateji thread üretir
        if (ctx.awaiting_fill && !cfg.dry_run && now_ns - last_poll_ns >= POLL_INTERVAL_NS
            && !ctx.poll_in_flight.load(std::memory_order_acquire)) {   // [FIX-BACKPRESSURE]
            if (now_ns - last_poll_ns >= POLL_INTERVAL_NS) {
                last_poll_ns = now_ns;
                OrderTask pt{};
                // [FIX-P0-02] kimlik slottaki borsa ID veya UNKNOWN+client_oid; yerel ID ASLA order_id olmaz
                if (!build_poll_task(ctx, pt)) {
                    // POLL uretilmedi (ornegin SUBMITTED + borsa ID yok): spin/log seli olmasin
                    if (now_ns - last_poll_skip_warn_ns >= 10'000'000'000ULL) {
                        last_poll_skip_warn_ns = now_ns;
                        ctx.logger.warn("POLL", "POLL uretilmedi: borsa ID yok ve UNKNOWN+client_oid yok -- SEND yaniti/timeout bekleniyor, local_id=", ctx.fsm_key);
                    }
                } else if (UNLIKELY(!ctx.order_channel.push_task(pt))) {
                    // push basarisiz -- bayragi ACIK BIRAKMA, bir sonraki
                    // dongude tekrar denensin.
                    ctx.logger.error("POLL", "POLL task drop! Ring dolu.");
                } else {
                    // [HAKEM ONAYLI FIX] Gorev basariyla atildi, isigi KIRMIZI yap!
                    ctx.poll_in_flight.store(true, std::memory_order_release);
                }
            }
        }

        // ── [V13-HOOKS] Ghost Exit Killer (spec §4) — 500ms throttle ──────────
       // ── [V13-HOOKS] Ghost Exit Killer (spec §4) — 500ms throttle ──────────
        if (now_ns - last_timeout_check_ns >= 500'000'000ULL) {
            last_timeout_check_ns = now_ns;
            enforce_timeouts(ctx, ctx.flat_order_map, ctx.order_channel, ctx.cfg.symbol);

            // [FIX-TTL] enforce_timeouts, "borsadan ne zamandir haber
            // alamadik"i olcer ve her basarili POLL cevabinda sifirlanir --
            // emir surekli sorgulanip "hala acik" cevabi alinsa bile SONSUZA
            // KADAR bekleyebilir. Bu kontrol ise "emir GONDERILELI ne kadar
            // oldu"yu olcer, POLL basarisindan bagimsizdir.
            if (ctx.awaiting_fill && ctx.fsm_key != 0 &&
                (now_ns - ctx.awaiting_fill_since_ns) > 30'000'000'000ULL) {
                // [PATCH-P2] Tek kapi: EXIT iptal edilmez; CANCEL_PENDING'deyken ikinci iptal gitmez;
                // terminal yalniz borsa teyidiyle (CANCELED) ya da yarisi kazanan FILLED ile olusur.
                if (request_entry_cancel(ctx, ctx.fsm_key, now_ns)) {
                    ctx.logger.warn("TTL", "Emir 30s icinde dolmadi -- otomatik CANCEL gonderildi, local_id=", ctx.fsm_key);
                    ctx.awaiting_fill_since_ns = now_ns;  // tekrar tekrar CANCEL spam'i onle
                }
            }
            // ── [V13-HOOKS] Silent Drop Monitor (spec §5) ────────────────────
            monitor_drops(ctx.market_ring, ctx.logger);

            // ── [KD-6] Tick-bagimsiz kill-switch taramasi ────────────────────
            ctx.ws_disconnect_total.store(kusak::ws::g_ws_disconnect_total.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);   // [PHASE3-KS]
            if (!ctx.kill_switch.load(std::memory_order_acquire) && check_kill_switch(ctx)) {
                ctx.kill_switch.store(true, std::memory_order_seq_cst);
                ctx.logger.critical("KILL_SWITCH",
                    "check_kill_switch tetiklendi (500ms taramasi, tick-bagimsiz)");
            }
        }

        // ── Konfigürasyon yeniden yükleme (1s) ───────────────────────────────
        if (now_ns - last_reload_ns >= RELOAD_INTERVAL_NS) {
            last_reload_ns = now_ns;
            if (config_mgr.check_reload(ctx.cfg)) {
                ctx.risk_scorer.set_weights(ctx.cfg.risk_weights);
                ctx.risk_scorer.set_thresholds(ctx.cfg.risk_thresholds);
                fw_publish_limits(ctx);   // [PHASE1-FW]
                ctx.logger.info("CONFIG", "Konfigurasyon yeniden yuklendi");
            }
            check_resume_conditions(ctx);
        }

        // ── Telemetri raporu (10s) ────────────────────────────────────────────
        if (now_ns - last_report_ns >= REPORT_INTERVAL_NS) {
            last_report_ns = now_ns;
            if (UNLIKELY(!ctx.telemetry_cmd_ring.push(TelemetryCmd::FLUSH_CSV))) {
                ctx.telemetry_cmd_drops.fetch_add(1, std::memory_order_relaxed);
            }
            // [ARCH-V2-FIX] vpin() -> vpin_x1e6(); flush_prometheus double beklediğinden /VPIN_SCALE ile geri ölçeklendi.
            ctx.telemetry.flush_prometheus(
                ctx.latency.tick_to_trade_p50(),
                ctx.latency.tick_to_trade_p99(),
                static_cast<double>(ctx.vpin.vpin_x1e6()) / static_cast<double>(kusak::research::VPIN_SCALE),
                static_cast<int>(ctx.regime.current_regime()));

            if (ctx.http_client)
                ctx.logger.debug("HTTP",
                    "req=",    ctx.http_client->total_requests(),
                    " retry=", ctx.http_client->total_retries(),
                    " rl=",    ctx.http_client->rate_limit_hits());
            const uint64_t td = ctx.order_channel.task_drops();
            const uint64_t rd = ctx.order_channel.result_drops();
            const uint64_t cd = ctx.telemetry_cmd_drops.load(std::memory_order_relaxed);
            if (UNLIKELY(td || rd || cd)) {
                ctx.logger.error("DROP_MONITOR",
                    "task_drop=", td, " result_drop=", rd, " telemetry_drop=", cd);
            }
            // [FIX-P36-C17] g_fw_result_lost artik strateji tarafinda okunur. Kill switch order thread'de
            // (deliver_result / FW_REJECT) zaten acildi; burada yalniz sessiz kalmamasi saglanir (bayrak temizlenmez).
            if (UNLIKELY(g_fw_result_lost.load(std::memory_order_acquire))) {
                ctx.logger.critical("DROP_MONITOR", "ORDER SONUCU KAYBOLDU: dropped_results=",
                    g_dropped_results.load(std::memory_order_relaxed),
                    " kill_switch=", static_cast<int>(ctx.kill_switch.load(std::memory_order_acquire)));
            }

            if (ctx.awaiting_fill)
                ctx.logger.info("POLL",
                    "Bekleyen emir id=", ctx.active_order_id,
                    " -- fill bekleniyor");

            if (ctx.position.is_open() && !ctx.awaiting_fill)
                ctx.logger.info("POSITION",
                    "ACIK ", (ctx.position.is_long() ? "LONG" : "SHORT"),
                    " entry=",  ctx.position.entry_price,
                    " qty=",    ctx.position.qty,
                    " stop=",   ctx.position.stop_loss,
                    " trail=",  ctx.position.trail_stop);

            if (paused_load(ctx.trading_paused))
                ctx.logger.warn("SUSPEND",
                    "Trading DURAKLATILDI -- resume bekleniyor");

            if (cfg.verbose) {
                ctx.vpin.print_status();
                ctx.regime.print_diagnostics();
                ctx.telemetry.print_summary();
                ctx.latency.print_to_stdout();
            }
        }

        // ── Kill switch kontrol ───────────────────────────────────────────────
        // [FIX-LOST-UPDATE]: kill_switch asla burada false'a çekilmez.
        // Yalnızca check_resume_conditions → trading_paused=false yolu geçerli reset'tir.
        if (ctx.kill_switch.load(std::memory_order_acquire)) {
            // [FIX-FAZ0-1] Atomik CAS: yalniz false->true gecisini kazanan thread yan etkileri calistirir.
            if (paused_try_set(ctx.trading_paused)) {
                ctx.logger.critical("KILL_SWITCH",
                    "Aktif -- trading duraklatiliyor");
                if (g_inflight_orders.load(std::memory_order_seq_cst) != 0)
                    ctx.logger.critical("KILL_SWITCH",
                        "Ucusta emir var -- cancel-all (ks_service) kapsaminda");
                ctx.vpin_last_alarm_ns = now_ns;  // [V13-CLOCK]
                
                // ── [KD-9] Notifier entegrasyonu ─────────────────────────────
                if (ctx.notifier) {
                    ctx.notifier->send_circuit_breaker("KILL_SWITCH aktif -- trading duraklatildi");
                }
            }
            // [PHASE3-KS] CANCEL ALL (FSM) -> SAFE MODE -> (flatten_on_trip) -> manuel reset (kilit dosyasi).
            // Eski tek-seferlik flatten buradan kaldirildi: awaiting_fill acikken atlaniyor ve bir daha denenmiyordu.
            ks_service(ctx);
        }
    }
    // ── Graceful shutdown ─────────────────────────────────────────────────────
    ctx.logger.info("MAIN", "Durdurma sinyali alindi -- kapatiliyor...");

    // 1. ExchangeInfo manager'ı durdur
    if (ei_mgr) ei_mgr->stop();

    // 2. Market WS thread'ini durdur — artık yeni event gelmez
    // [FIX-SHUTDOWN-DEADLOCK] En fazla 1500ms bekle; connect()/SSL_connect()
    // yeniden-baglanma denemesinde takili kalsa bile Ctrl+C artik asilmiyor.
    join_with_timeout(std::move(market_thread), std::chrono::milliseconds(1500), "market_thread");

    // 2b. [PATCH-P7] Yeni GIRIS emri uretimi kapansin (drain sirasinda islenen tick'ler giris acmasin)
    paused_store(ctx.trading_paused, true);

    // 3. Kalan market eventlerini drain et — yeni task'lar üretilebilir
    {
        MarketEvent drain_buf[BATCH];
        uint32_t rem;
        while ((rem = ctx.market_ring.drain(drain_buf, BATCH)) > 0) {
            for (uint32_t i = 0; i < rem; ++i) {
                if (drain_buf[i].tag == MarketEvent::Tag::AGG_TRADE)
                    process_agg_trade(ctx, drain_buf[i].trade);
                else
                    process_book_ticker(ctx, drain_buf[i].ticker);
            }
        }
    }

    // 3b. [PATCH-P7] while(active_orders>0): order_thread HALA canli iken acik emirleri iptal et / reconcile et.
    //     Sinirli bekleme (3s) -- sonsuz dongu Ctrl+C'yi asardi. Kalan varsa ZORLA silinmez, CRITICAL loglanir.
    {
        const size_t left = drain_orders_on_shutdown(ctx, 3000);
        if (left > 0) {
            ctx.logger.critical("SHUTDOWN", "KAPANISTA ACIK EMIR KALDI: active_orders=", left,
                " -- borsada (open orders) ELLE kontrol edin!");
            for (size_t i = 0; i < MAX_ORDERS; ++i) {
                const OrderSlot& s = ctx.flat_order_map.slots[i];
                if (s.state == SlotState::ACTIVE)
                    ctx.logger.critical("SHUTDOWN", "  local_id=", s.order_id, " ex_id=", s.exchange_order_id,
                        " state=", order_state_name(s.fsm_state), " filled=", s.filled_qty);
            }
        }
        if (!ctx.flat_order_map.consistent())
            ctx.logger.critical("SHUTDOWN", "FlatOrderMap INVARIANT IHLALI: active_slots=",
                ctx.flat_order_map.active_slots, " bitmap=", ctx.flat_order_map.active_count());
    }

    // 4. [FIX-SHUTDOWN-RACE]: Önce order thread'i durdur ve bitmesini bekle.
    //    Thread join'den sonra ring'e yeni sonuç yazılamaz — drain kayıpsız.
    ctx.order_channel.stop.store(true, std::memory_order_release);
    // [FIX-SHUTDOWN-DEADLOCK] En fazla 1500ms bekle; curl retry firtinasi ya da
    // abort_progress_cb'nin bir kenar durumu yuzunden takili kalsa bile
    // Ctrl+C artik asilmiyor.
    join_with_timeout(std::move(order_thread), std::chrono::milliseconds(1500), "order_thread");

    // 5. Şimdi kalan tüm order sonuçlarını güvenle drain et
    {
        OrderResult res{};
        while (ctx.order_channel.pop_result(res))
            dispatch_order_result(ctx, res);
    }

    // 6. Telemetri thread'i durdur
    ctx.telemetry_cmd_ring.push(TelemetryCmd::STOP);
    // [FIX-SHUTDOWN-DEADLOCK] En fazla 1000ms bekle; logger'da beklenmeyen
    // bir kilitlenme olsa bile Ctrl+C artik asilmiyor.
    join_with_timeout(std::move(telemetry_thread), std::chrono::milliseconds(1000), "telemetry_thread");

    // [FIX-REPORT-FLUSH] Ctrl+C sonrasi cok hizli kapanma (artik ~ms
    // mertebesinde) yuzunden std::cout'un buffer'da kalmis olma ihtimaline
    // karsi, her rapordan hemen sonra ACIK flush yapiyoruz. Ayrica raporun
    // basladigini/bittigini gosteren belirgin isaretler ekliyoruz -- terminal
    // ciktisinda kaybolmasin diye.
    std::cout << "\n=== KAPANIS RAPORU BASLIYOR ===\n" << std::flush;
    ctx.telemetry.print_summary();
    std::cout << std::flush;
    ctx.latency.dump_report();
    std::cout << std::flush;
    ctx.adverse.print_summary();
    std::cout << "=== KAPANIS RAPORU BITTI ===\n" << std::flush;

    ctx.logger.info("MAIN",
        "KUSAK V13 HFT kapandi -- hata=", ctx.logger.total_errors(),
        " active_orders=", ctx.flat_order_map.active_count());
    return 0;
}