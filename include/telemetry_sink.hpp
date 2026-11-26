// ============================================================================
// KUSAK V12 — RESEARCH FRAMEWORK
// telemetry_sink.hpp — Atomik Sayaçlar, Ring-Buffer Telemetri & Metrik Çıktı
//
// KAYNAK: Mimari Revizyon ve Canlıya Geçiş Planı — §3 Telemetri
//
//   Rapor tasarımı:
//     struct OrderStats { atomic<uint64_t> sent, filled, canceled; }
//     fill_rate = filled / sent
//     Metrik isimleri: HFT_latency_tick_to_trade_microseconds{symbol=...}
//     Ring-buffer'da ~50-100 byte/event; toplu disk yazımı
//
//   Bu dosya:
//     • TelemetryEvent : tek bir telemetri kaydı (50-100 byte)
//     • RingBuffer<N>  : lock-free SPSC halka tamponu (disk'e toplu yazım)
//     • TelemetrySink  : event kayıt, histogram, sayaç, CSV çıktı, Prometheus
//     • PrometheusExporter: text-format satırlarını oluşturur
//
// Thread safety: RingBuffer/ObiRing SPSC. Üretici = strateji thread; TEK tüketici = telemetri thread
// (flush_to_csv / flush_obi_csv yalnız oradan çağrılır; hot-path içinde flush YOK).
// ============================================================================

#pragma once

#include "research_types.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <limits>
#include <system_error>

namespace kusak::telemetry {

// ============================================================================
// §1  Telemetri Olay Türleri
// ============================================================================

enum class EventType : uint8_t {
    ORDER_SENT      = 0,
    ORDER_ACK       = 1,
    ORDER_FILLED    = 2,
    ORDER_CANCELED  = 3,
    ORDER_REJECTED  = 4,
    VPIN_ALARM      = 5,
    REGIME_CHANGE   = 6,
    LATENCY_SAMPLE  = 7,
    RISK_VETO       = 8,
};

inline const char* event_type_name(EventType e) noexcept {
    switch (e) {
        case EventType::ORDER_SENT:     return "ORDER_SENT";
        case EventType::ORDER_ACK:      return "ORDER_ACK";
        case EventType::ORDER_FILLED:   return "ORDER_FILLED";
        case EventType::ORDER_CANCELED: return "ORDER_CANCELED";
        case EventType::ORDER_REJECTED: return "ORDER_REJECTED";
        case EventType::VPIN_ALARM:     return "VPIN_ALARM";
        case EventType::REGIME_CHANGE:  return "REGIME_CHANGE";
        case EventType::LATENCY_SAMPLE: return "LATENCY_SAMPLE";
        case EventType::RISK_VETO:      return "RISK_VETO";
        default:                        return "UNKNOWN";
    }
}

// ============================================================================
// §2  Telemetri Olayı — Rapor §3: "~50-100 byte/event"
//
//   sizeof(TelemetryEvent) = 80 bytes
// ============================================================================

struct TelemetryEvent {
    uint64_t  ts_ns       = 0;      // Olay zamanı (ns)
    EventType type        = EventType::ORDER_SENT;
    uint8_t   _pad[7]     = {};

    // Emir bilgisi
    uint64_t  order_id    = 0;
    double    price       = 0.0;
    double    qty         = 0.0;

    // Gecikme bilgisi (ns)
    uint64_t  latency_ns  = 0;

    // Metrik değerler
    double    value1      = 0.0;  // vpin / risk_score / ...
    double    value2      = 0.0;  // hmm_prob / fill_price / ...

    // Sembol (8 karakter, null-terminated)
    char      symbol[8]   = {};

    void set_symbol(std::string_view s) noexcept {
        size_t n = std::min(s.size(), size_t(7));
        std::memcpy(symbol, s.data(), n);
        symbol[n] = '\0';
    }
};

static_assert(sizeof(TelemetryEvent) == 72, "TelemetryEvent boyutu 72 byte olmalı");

// ============================================================================
// §2b  OBI Forward-Return Olayı  [P1-OBI-TRACK]
//
//   obi_research/obi_telemetry_*.csv şemasının (obi_research.py ile birebir)
//   tek satırı. Ayrı struct: genel TelemetryEvent (72B, 9 olay türü, 16384'lük
//   halka) şişirilmez. sizeof = 128 (2 cache line), static_assert ile kilitli.
//   Düzen: 3×u64 (24) + 10×double (80) + 3×float (12) + u32 (4) + 2×u8 (2) = 122 → 128
// ============================================================================

inline constexpr size_t   OBI_N_HORIZONS = 7;
inline constexpr uint64_t OBI_HORIZON_MS[OBI_N_HORIZONS] = {10, 50, 100, 250, 500, 1000, 5000};

// Evrensel (wall) saat, µs. Yalnız sinyal anında 1 kez çağrılır (dosyalar arası birleştirme için).
inline uint64_t wall_clock_us() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

struct alignas(64) ObiEvent {
    uint64_t event_id       = 0;
    uint64_t signal_wall_us = 0;     // wall saat (µs) — Python birleştirme/dedupe anahtarı
    uint64_t signal_mono_ns = 0;     // ctx.current_time_ns() — yalnız aynı dosya içinde anlamlı
    double   signal_price   = 0.0;   // MID
    double   bid            = 0.0;
    double   ask            = 0.0;
    double   price_after[OBI_N_HORIZONS] = {};   // +10,+50,+100,+250,+500,+1000,+5000 ms MID
    float    obi            = 0.0f;
    float    spread_bps     = 0.0f;
    float    threshold      = 0.0f;
    uint32_t max_lag_us     = 0;     // horizon örnekleme gecikmesinin max'ı (tick'e bağlı)
    int8_t   side           = 0;     // +1 AL baskısı, -1 SAT baskısı
    uint8_t  n_filled       = 0;     // dolan horizon sayısı (tracker yalnız 7/7'yi yayınlar)
};

static_assert(sizeof(ObiEvent) == 128, "ObiEvent tam 128 byte (2 cache line) olmali");

// Lock-free SPSC halka (üretici: strateji thread'i, tüketici: telemetri thread'i). Heap yok.
template<size_t N = 1024>
class ObiRing {
    static_assert((N & (N - 1)) == 0, "N, 2'nin kuvveti olmalı");
public:
    bool push(const ObiEvent& ev) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head + 1 - tail_.load(std::memory_order_acquire) > N) return false;   // dolu
        buf_[head & (N - 1)] = ev;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    size_t pop_bulk(ObiEvent* out, size_t max_n) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        const size_t   n    = std::min<size_t>(max_n, static_cast<size_t>(head - tail));
        for (size_t i = 0; i < n; ++i) out[i] = buf_[(tail + i) & (N - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }
    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
private:
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    std::array<ObiEvent, N>           buf_{};
};

// ============================================================================
// §3  Lock-free SPSC Ring Buffer
//
//   Rapor: "Performans için zaman damgalarını ve sayaçları bellek içi saklayın
//            (lock-free ring-buffer). Daha sonra toplu halde disk/mmap'e yazılır."
// ============================================================================

template<size_t N = 16384>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N, 2'nin kuvveti olmalı");
public:
    static constexpr size_t CAPACITY = N;

    // Üretici (strategy thread)
    bool push(const TelemetryEvent& ev) noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > N)
            return false;  // Dolu
        buf_[head & (N - 1)] = ev;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Tüketici (logger/flush thread)
    bool pop(TelemetryEvent& ev) noexcept {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;  // Boş
        ev = buf_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Toplu çekme — dizi doldurur, gerçek alınan sayısını döndürür
    size_t pop_bulk(TelemetryEvent* out, size_t max_n) noexcept {
        size_t count = 0;
        while (count < max_n) {
            if (!pop(out[count])) break;
            ++count;
        }
        return count;
    }

    size_t size() const noexcept {
        uint64_t h = head_.load(std::memory_order_relaxed);
        uint64_t t = tail_.load(std::memory_order_relaxed);
        return static_cast<size_t>(h - t);
    }

    bool empty() const noexcept { return size() == 0; }

private:
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    std::array<TelemetryEvent, N>     buf_{};
};

// ============================================================================
// §4  Prometheus Metrikleri — Rapor §3 isim önerileri
//
//   Rapor: "HFT_latency_tick_to_trade_microseconds{symbol="BTCUSDT"}
//            hft_order_fill_count_total{symbol="BTCUSDT"}"
// ============================================================================

class PrometheusExporter {
public:
    explicit PrometheusExporter(std::string symbol = "BTCUSDT")
        : symbol_(std::move(symbol)) {}

    // Rapordaki isim şemasına uygun satırlar üretir
    std::string format_snapshot(
        uint64_t tick_to_trade_p50_us,
        uint64_t tick_to_trade_p99_us,
        uint64_t fills,
        uint64_t sent,
        uint64_t canceled,
        double   vpin,
        int      hmm_regime) const
    {
        std::ostringstream ss;
        auto lbl = [&]{ return "{symbol=\"" + symbol_ + "\"}"; };

        ss << "# HELP HFT_latency_tick_to_trade_microseconds Tick-to-trade latency\n"
           << "# TYPE HFT_latency_tick_to_trade_microseconds gauge\n"
           << "HFT_latency_tick_to_trade_microseconds_p50" << lbl()
           << " " << tick_to_trade_p50_us << "\n"
           << "HFT_latency_tick_to_trade_microseconds_p99" << lbl()
           << " " << tick_to_trade_p99_us << "\n"

           << "# HELP hft_order_fill_count_total Total filled orders\n"
           << "# TYPE hft_order_fill_count_total counter\n"
           << "hft_order_fill_count_total" << lbl() << " " << fills << "\n"

           << "# HELP hft_order_sent_total Total sent orders\n"
           << "# TYPE hft_order_sent_total counter\n"
           << "hft_order_sent_total" << lbl() << " " << sent << "\n"

           << "# HELP hft_order_canceled_total Total canceled orders\n"
           << "# TYPE hft_order_canceled_total counter\n"
           << "hft_order_canceled_total" << lbl() << " " << canceled << "\n"

           << "# HELP hft_vpin_current Current VPIN value\n"
           << "# TYPE hft_vpin_current gauge\n"
           << "hft_vpin_current" << lbl()
           << " " << std::fixed << std::setprecision(6) << vpin << "\n"

           << "# HELP hft_hmm_regime Current HMM regime (0=UNKNOWN,1=TREND,"
                                        "2=RANGE,3=TOXIC,4=DEAD)\n"
           << "# TYPE hft_hmm_regime gauge\n"
           << "hft_hmm_regime" << lbl() << " " << hmm_regime << "\n";

        return ss.str();
    }

    // Dosyaya yaz (Grafana Prometheus scrape için)
    bool write_to_file(const std::string& path, const std::string& content) const {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << content;
        return true;
    }

private:
    std::string symbol_;
};

// ============================================================================
// §5  TelemetrySink — Ana Sınıf
//
//   Rapor: "Telemetri örnek: ga('VPIN_event', vpinValue),
//            histogram('SendLatency', sendLatency)"
//
//   Kullanım:
//     TelemetrySink sink("BTCUSDT");
//     sink.record_order_sent(orderId, price, qty, ts_ns);
//     sink.record_latency(latencyNs);
//     sink.flush_to_csv();   // logger thread'de periyodik çağrılır
// ============================================================================

// Config, TelemetrySink dışında tanımlanmalı; GCC 13'te nested struct
// içinde default member initializer varken enclosing class constructor'ında
// = Config{} şeklinde default argüman kullanılamaz (C++20 §11.4.5).
struct TelemetrySinkConfig {
    std::string symbol           = "BTCUSDT";
    std::string csv_path         = "telemetry.csv";
    std::string prom_path        = "metrics.prom";
    size_t      flush_batch      = 512;
    bool        write_csv        = true;
    bool        write_prometheus = true;
    bool        verbose          = false;
    bool        write_obi_csv    = true;                 // [P1-OBI-TRACK]
    std::string obi_dir          = "obi_research";       // obi_research.py bu dizini okur
};

class TelemetrySink {
public:
    using Config = TelemetrySinkConfig;

    explicit TelemetrySink(Config cfg = Config{})
        : cfg_(std::move(cfg))
        , prom_(cfg_.symbol)
    {}

    // ── Kayıt metodları (hot-path, lock-free) ────────────────────────

    void record_order_sent(uint64_t order_id, double price, double qty,
                           uint64_t ts_ns) noexcept
    {
        sent_.fetch_add(1, std::memory_order_relaxed);
        TelemetryEvent ev{};
        ev.ts_ns    = ts_ns;
        ev.type     = EventType::ORDER_SENT;
        ev.order_id = order_id;
        ev.price    = price;
        ev.qty      = qty;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    void record_order_filled(uint64_t order_id, double fill_price,
                              double qty, uint64_t ts_ns) noexcept
    {
        filled_.fetch_add(1, std::memory_order_relaxed);
        TelemetryEvent ev{};
        ev.ts_ns    = ts_ns;
        ev.type     = EventType::ORDER_FILLED;
        ev.order_id = order_id;
        ev.price    = fill_price;
        ev.qty      = qty;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    void record_order_canceled(uint64_t order_id, uint64_t ts_ns) noexcept
    {
        canceled_.fetch_add(1, std::memory_order_relaxed);
        TelemetryEvent ev{};
        ev.ts_ns    = ts_ns;
        ev.type     = EventType::ORDER_CANCELED;
        ev.order_id = order_id;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    void record_order_rejected(uint64_t order_id, uint64_t ts_ns) noexcept
    {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        TelemetryEvent ev{};
        ev.ts_ns    = ts_ns;
        ev.type     = EventType::ORDER_REJECTED;
        ev.order_id = order_id;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    // Rapor: "histogram('SendLatency', sendLatency)"
    void record_latency(uint64_t latency_ns, uint64_t ts_ns = 0) noexcept
    {
        TelemetryEvent ev{};
        ev.ts_ns      = ts_ns;
        ev.type       = EventType::LATENCY_SAMPLE;
        ev.latency_ns = latency_ns;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);

        // Online histogram güncelle
        uint64_t us = latency_ns / 1000;
        lat_sum_us_.fetch_add(us, std::memory_order_relaxed);
        lat_count_.fetch_add(1, std::memory_order_relaxed);
        uint64_t cur_max = lat_max_us_.load(std::memory_order_relaxed);
        while (us > cur_max &&
               !lat_max_us_.compare_exchange_weak(cur_max, us,
                   std::memory_order_relaxed)) {}
    }

    // Rapor: "ga('VPIN_event', vpinValue)"
    void record_vpin_alarm(double vpin, double threshold,
                            uint64_t ts_ns = 0) noexcept
    {
        TelemetryEvent ev{};
        ev.ts_ns  = ts_ns;
        ev.type   = EventType::VPIN_ALARM;
        ev.value1 = vpin;
        ev.value2 = threshold;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    void record_risk_veto(double risk_score, uint64_t ts_ns = 0) noexcept
    {
        TelemetryEvent ev{};
        ev.ts_ns  = ts_ns;
        ev.type   = EventType::RISK_VETO;
        ev.value1 = risk_score;
        ev.set_symbol(cfg_.symbol);
        ring_.push(ev);
    }

    // ── [P1-OBI-TRACK] OBI forward-return kaydı ───────────────────────
    // Strateji thread'i çağırır (hot-path: 1 kopya + 1 release store, heap yok).
    void record_obi_event(const ObiEvent& ev) noexcept {
        if (!obi_ring_.push(ev))
            obi_drops_.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t obi_dropped() const noexcept { return obi_drops_.load(std::memory_order_relaxed); }

    // Telemetri thread'inde çağrılır (flush_to_csv içinden). Dosya: <obi_dir>/obi_telemetry_<unix_s>.csv
    size_t flush_obi_csv() {
        if (!cfg_.write_obi_csv || obi_ring_.empty()) return 0;
        if (!obi_opened_) {
            std::error_code ec;
            std::filesystem::create_directories(cfg_.obi_dir, ec);
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            obi_path_ = cfg_.obi_dir + "/obi_telemetry_" + std::to_string(secs) + ".csv";
        }
        std::ofstream f(obi_path_, std::ios::app);
        if (!f.is_open()) return 0;
        if (!obi_opened_) {
            f << "event_id,signal_wall_us,signal_mono_ns,side,threshold,obi,bid,ask,spread_bps,signal_price";
            for (size_t i = 0; i < OBI_N_HORIZONS; ++i) f << ",price_after_" << OBI_HORIZON_MS[i] << "ms";
            f << ",max_lag_us,n_filled\n";
            obi_opened_ = true;
        }
        std::array<ObiEvent, 256> batch;
        size_t total = 0, n;
        while ((n = obi_ring_.pop_bulk(batch.data(), batch.size())) > 0) {
            for (size_t i = 0; i < n; ++i) {
                const ObiEvent& e = batch[i];
                f << e.event_id << ',' << e.signal_wall_us << ',' << e.signal_mono_ns << ','
                  << static_cast<int>(e.side) << ',' << std::fixed
                  << std::setprecision(3) << e.threshold << ','
                  << std::setprecision(6) << e.obi << ','
                  << std::setprecision(8) << e.bid << ',' << e.ask << ','
                  << std::setprecision(4) << e.spread_bps << ','
                  << std::setprecision(8) << e.signal_price;
                for (size_t h = 0; h < OBI_N_HORIZONS; ++h) f << ',' << e.price_after[h];
                f << ',' << e.max_lag_us << ',' << static_cast<int>(e.n_filled) << '\n';
            }
            total += n;
        }
        return total;
    }

    // ── İstatistik sorguları ──────────────────────────────────────────

    uint64_t total_sent()     const noexcept { return sent_.load(std::memory_order_relaxed); }
    uint64_t total_filled()   const noexcept { return filled_.load(std::memory_order_relaxed); }
    uint64_t total_canceled() const noexcept { return canceled_.load(std::memory_order_relaxed); }
    uint64_t total_rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }

    double fill_rate() const noexcept {
        uint64_t s = total_sent();
        return (s > 0) ? static_cast<double>(total_filled()) / s : 0.0;
    }
    double cancel_rate() const noexcept {
        uint64_t s = total_sent();
        return (s > 0) ? static_cast<double>(total_canceled()) / s : 0.0;
    }

    double mean_latency_us() const noexcept {
        uint64_t cnt = lat_count_.load(std::memory_order_relaxed);
        return (cnt > 0) ? static_cast<double>(
            lat_sum_us_.load(std::memory_order_relaxed)) / cnt : 0.0;
    }
    uint64_t max_latency_us() const noexcept {
        return lat_max_us_.load(std::memory_order_relaxed);
    }

    // ── Flush (logger thread'de çağrılır) ─────────────────────────────

    // [TELEMETRY-THREAD-ONLY] Halkaların TEK tüketicisi. Yalnız telemetri thread'i çağırır
    // (main.cpp: FLUSH_CSV komutu, 1s zamanlayıcı ve kapanış). Strateji thread'i ASLA çağırmaz
    // -> SPSC sözleşmesi (1 üretici / 1 tüketici) korunur. Halka tamamen boşaltılır.
    size_t flush_to_csv() {
        flush_obi_csv();   // [P1-OBI-TRACK]
        if (!cfg_.write_csv) return 0;
        if (csv_opened_ && ring_.empty()) return 0;   // boşken dosyayı açıp kapatma
        bool need_header = !csv_opened_;
        std::ofstream f(cfg_.csv_path, std::ios::app);
        if (!f.is_open()) return 0;
        csv_opened_ = true;

        if (need_header)
            f << "ts_ns,type,order_id,price,qty,latency_ns,value1,value2,symbol\n";

        TelemetryEvent batch[512];
        size_t total = 0, n;
        while ((n = ring_.pop_bulk(batch, 512)) > 0) {
            for (size_t i = 0; i < n; ++i) {
                auto& e = batch[i];
                f << e.ts_ns << ","
                  << event_type_name(e.type) << ","
                  << e.order_id << ","
                  << std::fixed << std::setprecision(6)
                  << e.price << "," << e.qty << ","
                  << e.latency_ns << ","
                  << e.value1 << "," << e.value2 << ","
                  << e.symbol << "\n";
            }
            total += n;
        }
        return total;
    }

    // Prometheus text formatını dosyaya yaz
    void flush_prometheus(uint64_t lat_p50_us = 0, uint64_t lat_p99_us = 0,
                           double vpin = 0.0, int regime = 0) {
        if (!cfg_.write_prometheus) return;
        auto content = prom_.format_snapshot(
            lat_p50_us, lat_p99_us,
            total_filled(), total_sent(), total_canceled(),
            vpin, regime);
        prom_.write_to_file(cfg_.prom_path, content);
    }

    void print_summary() const {
        uint64_t s = total_sent(), f = total_filled();
        uint64_t c = total_canceled(), r = total_rejected();
        std::cout << "\n─── TelemetrySink [" << cfg_.symbol << "] ────────────────\n"
                  << std::fixed << std::setprecision(3)
                  << "  Gönderilen    : " << s << "\n"
                  << "  Doldurulan    : " << f << "\n"
                  << "  İptal         : " << c << "\n"
                  << "  Reddedilen    : " << r << "\n"
                  << "  Fill oranı    : " << fill_rate() * 100.0 << " %\n"
                  << "  İptal oranı   : " << cancel_rate() * 100.0 << " %\n"
                  << "  Ort. gecikme  : " << mean_latency_us() << " µs\n"
                  << "  Maks. gecikme : " << max_latency_us() << " µs\n"
                  << "  Ring buf sz   : " << ring_.size() << "\n"
                  << "─────────────────────────────────────────────────\n";
    }

private:
    Config cfg_;
    PrometheusExporter prom_;

    RingBuffer<16384> ring_;

    std::atomic<uint64_t> sent_    {0};
    std::atomic<uint64_t> filled_  {0};
    std::atomic<uint64_t> canceled_{0};
    std::atomic<uint64_t> rejected_{0};

    // Gecikme online istatistik
    std::atomic<uint64_t> lat_sum_us_ {0};
    std::atomic<uint64_t> lat_count_  {0};
    std::atomic<uint64_t> lat_max_us_ {0};

    bool csv_opened_ = false;

    // [P1-OBI-TRACK]
    ObiRing<1024>         obi_ring_;
    std::atomic<uint64_t> obi_drops_{0};
    bool                  obi_opened_ = false;
    std::string           obi_path_;
};

} // namespace kusak::telemetry
