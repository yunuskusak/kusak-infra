#pragma once
#ifndef KUSAK_VPIN_VELOCITY_TRACKER_HPP
#define KUSAK_VPIN_VELOCITY_TRACKER_HPP

// [ARCH-V2] Mimari dönüşüm raporuna göre yeniden yazıldı — 5 kural:
//   1) std::deque -> sabit kapasiteli (128, 2^n) RingBuffer; sarma % yerine & RING_MASK.
//   2) RingBuffer::head_/tail_/count_ -> alignas(64) (false-sharing önleme).
//   3) double/float YOK; tüm hacim/VPIN/OBI hesapları 1e6 ölçekli int64_t sabit-nokta.
//   4) Taşma riski olan çarpımlarda (VPINBucket::oi_scaled) __int128 ara hesap.
//      (EWMA terimleri a*v, (S-a)*p <= S^2 = 1e12 olduğundan int64_t yeterli/güvenli;
//      __int128 yalnızca gerçekten gerekli olan tek yere uygulanmıştır.)
//   5) Heap allocation yok — std::array tabanlı, sabit boyutlu depolama.
// Eski FIX-QUANT-1/2 notlarındaki NaN/Inf koruması ve float drift'i artık yapısal
// olarak imkânsız (tam sayı NaN üretemez).
//
// [ABI] research_types.hpp bu oturumda sağlanmadı; VPINUpdate::vpin ve
// ::dynamic_threshold'un (orijinaldeki `out.vpin = vpin_safe` atamasına bakılarak)
// legacy double alanlar olduğu varsayılmıştır. Dosyadaki TEK double kullanımı bu iki
// alanın serileştirme sınırındaki int64_t -> double dönüşümüdür; geri kalan her yer
// (config, bucket, state, ring buffer, tüm iç hesap) tamamen int64_t/__int128'dir.
//
// [BREAKING] Sessiz birim-ölçek hatasını (double [0,1] vs int64_t [0,1e6]) derleme
// hatasına çevirmek için isimler bilinçli değiştirildi — eski adla yazılmış çağrı
// yerleri (main.cpp vb.) derlenmemelidir, yanlış ölçekte sessizce çalışmamalıdır:
//   vpin() -> vpin_x1e6() [int64_t]
//   state().vpin/.adaptive_thr/.window_vol -> state().vpin_scaled/.adaptive_thr_scaled/.window_vol_scaled
//   CloseCallback: void(VPINAlarm,double) -> void(VPINAlarm,int64_t)
//   update_trade(double qty,double price,..) -> update_trade(int64_t qty_scaled,int64_t price_scaled,..)
//   VPINBucket: buy_vol/sell_vol/total()/oi() -> *_scaled / total_scaled() / oi_scaled()

#include "core_types.hpp"
#include "research_types.hpp"

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <array>
#include <algorithm>
#include <functional>
#include <utility>

namespace kusak::research {

inline constexpr int64_t VPIN_SCALE = 1'000'000; // [KURAL-3] 1e6 sabit-nokta ölçek faktörü

// ─────────────────────────────────────────────────────────────────────────────
// VPINConfig — tüm oranlar/eşikler 1e6 ölçekli int64_t (double/float yok)
// ─────────────────────────────────────────────────────────────────────────────
struct VPINConfig {
    int     n_buckets_window                = 50;                 // rolling window bucket sayısı (fiziksel üst sınır: RingBuffer::RING_CAPACITY)
    // [PHASE5] Bucket hedef hacmi (taban birim x 1e6) iki modda calisir:
    //   > 0  : SABIT hedef (ornek: 300*VPIN_SCALE = 300 birim). Eski davranis, bit-bit ayni.
    //   == 0 : OTOMATIK -- ilk `calib_trades` islemin ORTALAMA boyutu x T. T = bucket_size_multiplier_scaled/1e6
    //          "bucket basina hedef islem sayisi"dir (alt sinir MIN_AUTO_T_SCALED/1e6 = 10).
    //          Hedef piyasa akisina oranlanir; sabit 1000 birim gibi enstruman-bagimsiz sihirli sayi yoktur.
    //          Kalibrasyon islemleri bucket'lanmaz (VPIN, kalibrasyon + min_buckets_valid bucket sonra gecerli olur).
    // NOT: Burada varsayilan (1000 birim, sabit) eski arac/testler icin KORUNDU; motor (StrategyContext)
    //      config'ten acikca ezer (bkz. vpin_bucket_volume / vpin_bucket_mult / vpin_calib_trades).
    int64_t bucket_volume_scaled            = 1000LL * VPIN_SCALE; // her bucket'ın hedef hacmi (0 = otomatik)
    int64_t bucket_size_multiplier_scaled   = 1LL    * VPIN_SCALE; // OTOMATIK modda T (hedef islem/bucket) x 1e6
    int64_t warn_threshold_scaled           = 600'000;             // 0.60 — VPINAlarm::WARNING eşiği
    int64_t crit_threshold_scaled           = 800'000;             // 0.80 — VPINAlarm::CRITICAL eşiği
    int64_t ewma_alpha_scaled               = 50'000;              // 0.05 — adaptive threshold EWMA
    int     min_buckets_valid               = 10;                  // geçerli VPIN için min dolu bucket
    bool    use_dynamic_threshold           = true;                // dynamic threshold toggle
    size_t  dynamic_window                  = 20;                  // dynamic threshold window
    int64_t dynamic_k_scaled                = 2LL * VPIN_SCALE;    // 2.0 — dynamic threshold k-factor
    int64_t velocity_surge_threshold_scaled = 3LL * VPIN_SCALE;    // 3.0 — velocity surge threshold
    int     calib_trades                    = 500;                 // [PHASE5] otomatik mod: ortalama islem boyutu orneklemi
};

// ─────────────────────────────────────────────────────────────────────────────
// VPINBucket — tek hacim dilimi (int64_t, 1e6 ölçekli sabit nokta)
// ─────────────────────────────────────────────────────────────────────────────
struct VPINBucket {
    int64_t buy_vol_scaled  = 0;
    int64_t sell_vol_scaled = 0;

    int64_t total_scaled() const noexcept { return buy_vol_scaled + sell_vol_scaled; }

    // |buy-sell| / total, 1e6 ölçekli sonuç.
    // [KURAL-4]: diff (ham, 1e6-ölçekli hacim farkı) büyük nominal değerlerde
    // diff*VPIN_SCALE çarpımını int64_t sınırının (~9.22e18) üstüne taşırabilir
    // (örn. diff~1e15 için ~1e21) -> __int128 ara hesapla taşma engellenir.
    int64_t oi_scaled() const noexcept {
        const int64_t t = total_scaled();
        if (t <= 0) return 0;
        const int64_t diff = (buy_vol_scaled >= sell_vol_scaled)
                            ? (buy_vol_scaled - sell_vol_scaled)
                            : (sell_vol_scaled - buy_vol_scaled);
        const __int128 num = static_cast<__int128>(diff) * VPIN_SCALE;
        return static_cast<int64_t>(num / t);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RingBuffer — sabit kapasiteli (128, 2'nin kuvveti), heap-free, bit-mask sarmalı
// halka tampon. [KURAL-1]: std::deque yerine std::array; % yerine & RING_MASK.
// [KURAL-2]: head_/tail_/count_ ayrı önbellek satırlarında (alignas(64)) —
// VPINVelocityTracker tek-thread'li olsa da spesifikasyon gereği uygulanmıştır.
// ─────────────────────────────────────────────────────────────────────────────
class RingBuffer {
public:
    static constexpr size_t RING_CAPACITY = 128;
    static constexpr size_t RING_MASK     = RING_CAPACITY - 1;
    static_assert((RING_CAPACITY & (RING_CAPACITY - 1)) == 0,
                  "RING_CAPACITY 2'nin kuvveti olmalidir");

    void push_back(const VPINBucket& b) noexcept {
        buf_[head_] = b;
        head_ = (head_ + 1) & RING_MASK;
        if (count_ < RING_CAPACITY) {
            ++count_;
        } else {
            tail_ = (tail_ + 1) & RING_MASK; // dolulukta en eski öğe üzerine yazılır
        }
    }

    void pop_front() noexcept {
        if (count_ == 0) return;
        tail_ = (tail_ + 1) & RING_MASK;
        --count_;
    }

    const VPINBucket& operator[](size_t i) const noexcept { return buf_[(tail_ + i) & RING_MASK]; }

    size_t size()  const noexcept { return count_; }
    bool   empty() const noexcept { return count_ == 0; }
    void   clear() noexcept { head_ = 0; tail_ = 0; count_ = 0; }

    // range-for desteği (recompute_vpin / window_vol_scaled döngüsü için)
    struct const_iterator {
        const RingBuffer* rb;
        size_t idx;
        const VPINBucket& operator*() const noexcept { return (*rb)[idx]; }
        const_iterator& operator++() noexcept { ++idx; return *this; }
        bool operator!=(const const_iterator& o) const noexcept { return idx != o.idx; }
    };
    const_iterator begin() const noexcept { return const_iterator{this, 0}; }
    const_iterator end()   const noexcept { return const_iterator{this, count_}; }

private:
    std::array<VPINBucket, RING_CAPACITY> buf_{};
    alignas(64) size_t head_  = 0; // [KURAL-2]
    alignas(64) size_t tail_  = 0; // [KURAL-2]
    alignas(64) size_t count_ = 0; // [KURAL-2]
};

// ─────────────────────────────────────────────────────────────────────────────
// VPINState — tracker anlık durumu (int64_t, 1e6 ölçekli)
// ─────────────────────────────────────────────────────────────────────────────
struct VPINState {
    int64_t  vpin_scaled         = 0;       // VPIN * 1e6, [0, 1e6]
    int64_t  adaptive_thr_scaled = 600'000; // EWMA ile güncellenir (0.60 başlangıç)
    uint64_t bucket_count        = 0;       // tamamlanan bucket toplamı
    int64_t  window_vol_scaled   = 0;       // penceredeki toplam hacim (1e6 ölçekli)
    bool     valid                = false;   // min_buckets_valid geçildi mi
};

// ─────────────────────────────────────────────────────────────────────────────
// VPINVelocityTracker
//
// Volume-Synchronized Probability of Informed Trading.
// Tamamen int64_t / __int128 sabit-nokta aritmetik; heap allocation yok.
// lock-free, single-threaded call path (birden fazla thread için dışarıdan mutex).
// ─────────────────────────────────────────────────────────────────────────────
class VPINVelocityTracker {
public:
    explicit VPINVelocityTracker(VPINConfig cfg = VPINConfig{}) noexcept
        : cfg_(cfg) {
        // RingBuffer fiziksel kapasitesi (128) sabit; pencere onu aşamaz.
        if (cfg_.n_buckets_window > static_cast<int>(RingBuffer::RING_CAPACITY))
            cfg_.n_buckets_window = static_cast<int>(RingBuffer::RING_CAPACITY);
        if (cfg_.n_buckets_window < 1)
            cfg_.n_buckets_window = 1;
    }

    // ── Ana güncelleme: her trade tick'te çağrılır ────────────────────────────
    // qty_scaled: işlem hacmi, 1e6 ölçekli sabit-nokta (feed-handler'da önceden ölçeklenir)
    // price_scaled: 1e6 ölçekli sabit-nokta fiyat (bu trackerda kullanılmaz; arayüz
    //               uyumluluğu için tutulur)
    // is_buy: aggressor buy mu (tick-test veya exchange flag)
    // ts_us: exchange timestamp (µs)
    VPINUpdate update_trade(int64_t qty_scaled, int64_t price_scaled,
                            bool is_buy, uint64_t ts_us) noexcept {
        (void)price_scaled;
        (void)ts_us;

        // qty <= 0 erken dönüş — current_ zehirlenmez. int64_t olduğundan NaN/Inf
        // yapısal olarak imkânsız; tek kontrol işaret kontrolüdür.
        if (qty_scaled <= 0) [[unlikely]] { return build_update(); }

        // [PHASE5] Bucket hedefi: SABIT (bucket_volume_scaled > 0) ya da ilk calib_trades isleminden OTOMATIK.
        // Kalibrasyon bitene dek islemler bucket'a EKLENMEZ (hedef bilinmeden bucket kapatilamaz).
        if (!calibrated_) [[unlikely]] {
            if (cfg_.bucket_volume_scaled > 0) {
                eff_bucket_scaled_ = cfg_.bucket_volume_scaled;
                calibrated_        = true;
            } else {
                calib_sum_ += qty_scaled;
                ++calib_n_;
                if (calib_n_ < static_cast<uint32_t>(std::max(1, cfg_.calib_trades))) { return build_update(); }
                // hedef = (ortalama islem boyutu) x T ;  __int128: sum * T_scaled tasmasin
                const int64_t  t_scaled = std::max(cfg_.bucket_size_multiplier_scaled, MIN_AUTO_T_SCALED);
                const __int128 num      = static_cast<__int128>(calib_sum_) * t_scaled;
                const __int128 den      = static_cast<__int128>(calib_n_) * VPIN_SCALE;
                eff_bucket_scaled_ = std::max<int64_t>(1, static_cast<int64_t>(num / den));
                calibrated_        = true;
                return build_update();   // kalibrasyonu bitiren islem de bucket'lanmaz
            }
        }
        const int64_t bvol_scaled = eff_bucket_scaled_;   // calibrated_ burada daima true => bvol >= 1

        window_count_ = static_cast<uint32_t>(buckets_.size());

        // [SPILLOVER] Hacim korunumu: islem kovanin kalan kapasitesini asarsa kova TAM bvol'de kapanir,
        // arta kalan hacim (ayni yon) sonraki kova(lar)a eksiksiz aktarilir.
        int64_t rem = qty_scaled;

        // Dev islem korumasi: pencereyi (n_buckets_window) tamamen yenileyecek kadar tam kova varsa,
        // en eskileri tek tek uretmek yerine atla (hepsi tek yonlu, ayni icerikli; halka zaten ezilirdi).
        // bucket_count atlanan kovalari da sayar; hacim korunur. Dongu <= n_buckets_window+2 turla sinirli.
        {
            const int64_t room0 = bvol_scaled - current_.total_scaled();
            if (rem > room0) {
                const int64_t full = (rem - room0) / bvol_scaled;
                const int64_t keep = static_cast<int64_t>(cfg_.n_buckets_window);
                if (full > keep) {
                    const int64_t skip = full - keep;
                    state_.bucket_count += static_cast<uint64_t>(skip);
                    rem -= skip * bvol_scaled;               // skip*bvol <= rem: tasma yok
                }
            }
        }

        while (rem > 0) {
            const int64_t room = bvol_scaled - current_.total_scaled();
            if (room <= 0) [[unlikely]] { close_bucket(); continue; }   // savunma: dolu kova
            const int64_t take = std::min(rem, room);
            if (is_buy) current_.buy_vol_scaled  += take;
            else        current_.sell_vol_scaled += take;
            rem -= take;
            if (current_.total_scaled() >= bvol_scaled) close_bucket();
            // aksi halde rem == 0: kova acik kalir, dongu biter
        }

        return build_update();
    }

    // ── [V13-COMPAT] Research katmani adaptoru (eski VPINCalculator yerine) ───
    // double qty/price alir, 1e6 ölçeğe çevirip update_trade()'e iletir.
    // is_buy = AGRESOR alici mi (Binance aggTrade 'm' bayragi tersi).
    VPINUpdate update_trade_f(double qty, double price, bool is_buy, uint64_t ts_us) noexcept {
        constexpr double kMax = 9.0e12;   // kMax * 1e6 < INT64_MAX
        if (!(qty > 0.0) || !std::isfinite(qty)) [[unlikely]] return build_update();
        const double q = std::min(qty, kMax);
        const double p = (std::isfinite(price) && price > 0.0) ? std::min(price, kMax) : 0.0;
        return update_trade(static_cast<int64_t>(std::llround(q * static_cast<double>(VPIN_SCALE))),
                            static_cast<int64_t>(std::llround(p * static_cast<double>(VPIN_SCALE))),
                            is_buy, ts_us);
    }
    // VPIN [0,1] — SADECE research/raporlama; motor yolu vpin_x1e6() kullanmali.
    double vpin_f() const noexcept {
        return static_cast<double>(state_.vpin_scaled) / static_cast<double>(VPIN_SCALE);
    }

    // ── Public state erişimi ──────────────────────────────────────────────────
    const VPINState& state() const noexcept { return state_; }
    // [PHASE5] bucket hedefi gozlemi
    static constexpr int64_t MIN_AUTO_T_SCALED = 10 * VPIN_SCALE;   // otomatik modda T alt siniri (islem/bucket)
    bool     calibrated() const noexcept                       { return calibrated_; }
    int64_t  effective_bucket_volume_scaled() const noexcept   { return eff_bucket_scaled_; }
    uint32_t calib_progress() const noexcept                   { return calib_n_; }
    int64_t  calib_mean_qty_scaled() const noexcept            { return calib_n_ ? calib_sum_ / static_cast<int64_t>(calib_n_) : 0; }

    void reset() noexcept {
        buckets_.clear();
        current_      = VPINBucket{};
        state_        = VPINState{};
        window_vol_   = 0;
        window_count_ = 0;
        calibrated_ = false; calib_n_ = 0; calib_sum_ = 0; eff_bucket_scaled_ = 0;   // [PHASE5] yeniden kalibre
    }

    // ── main.cpp'nin çağırdığı legacy accessor'lar ───────────────────────────
    // [BREAKING] vpin() -> vpin_x1e6(): dönüş tipi int64_t (1e6 ölçekli). Eski
    // çağrı yerleri derleme hatası vermeli; double sanıp yanlış ölçekte sessizce
    // çalışmamalıdır.
    int64_t   vpin_x1e6() const noexcept { return state_.vpin_scaled; }
    VPINAlarm alarm()     const noexcept { return last_alarm_; }

    // ── Callback desteği (main.cpp set_close_callback kullanıyor) ────────────
    // [BREAKING] double -> int64_t (1e6 ölçekli vpin değeri)
    using CloseCallback = std::function<void(VPINAlarm, int64_t)>;
    void set_close_callback(CloseCallback cb) noexcept {
        close_cb_ = std::move(cb);
    }

    // ── Diagnostics ──────────────────────────────────────────────────────────
    void print_status() const noexcept { /* telemetry sink tarafından doldurulur */ }

private:
    // [SPILLOVER] Dolan kovayi kapatir: push -> pencere kirp -> window_vol_ -> recompute_vpin (her kapanista).
    void close_bucket() noexcept {
        buckets_.push_back(current_);
        current_ = VPINBucket{};
        ++state_.bucket_count;

        while (static_cast<int>(buckets_.size()) > cfg_.n_buckets_window) buckets_.pop_front();

        window_vol_ = 0;   // RING_CAPACITY * tek-kova-hacmi int64_t sinirinin cok altinda
        for (const auto& b : buckets_) window_vol_ += b.total_scaled();
        state_.window_vol_scaled = window_vol_;   // state() uzerinden gozlenebilsin (onceden hic yazilmiyordu)

        window_count_ = static_cast<uint32_t>(buckets_.size());
        recompute_vpin();
    }

    void recompute_vpin() noexcept {
        const int n = static_cast<int>(buckets_.size());
        state_.valid = (n >= cfg_.min_buckets_valid);

        if (!state_.valid) {
            state_.vpin_scaled = 0;
            return; // adaptive_thr_scaled int64_t — NaN riski yapısal olarak yok
        }

        int64_t sum_oi_scaled = 0; // <= RING_CAPACITY * VPIN_SCALE (~1.28e8) — taşma yok
        for (const auto& b : buckets_) sum_oi_scaled += b.oi_scaled();

        state_.vpin_scaled = std::clamp<int64_t>(sum_oi_scaled / n, 0, VPIN_SCALE);

        const int64_t alpha    = std::clamp<int64_t>(cfg_.ewma_alpha_scaled, 0, VPIN_SCALE);
        const int64_t prev_thr = std::clamp<int64_t>(state_.adaptive_thr_scaled, 0, VPIN_SCALE);

        // alpha, vpin_scaled, prev_thr hepsi [0, VPIN_SCALE] ile sınırlı (yukarıdaki
        // clamp'lar). a*v + (S-a)*p <= S*S = 1e12, int64_t sınırının (~9.22e18) çok
        // altında -> burada __int128 gerekmez ([KURAL-4] yalnızca ham hacim
        // oranlarında geçerli, bkz. VPINBucket::oi_scaled()).
        const int64_t term1 = alpha * state_.vpin_scaled;
        const int64_t term2 = (VPIN_SCALE - alpha) * prev_thr;
        state_.adaptive_thr_scaled = std::clamp<int64_t>((term1 + term2) / VPIN_SCALE, 0, VPIN_SCALE);
    }

    // ── VPINUpdate inşası — tüm fixed-point field'lar burada üretilir ────────
    VPINUpdate build_update() noexcept {
        VPINUpdate out{};

        const int64_t vpin_scaled = std::clamp<int64_t>(state_.vpin_scaled, 0, VPIN_SCALE);
        const int64_t thr_scaled  = std::clamp<int64_t>(state_.adaptive_thr_scaled, 0, VPIN_SCALE);

        // [ABI] VPINUpdate::vpin / dynamic_threshold — research_types.hpp bu oturumda
        // sağlanmadı; legacy double alanlar olduğu varsayılır (bkz. dosya başı not).
        // Dosyadaki TEK double işlemi budur.
        out.vpin              = static_cast<double>(vpin_scaled) / static_cast<double>(VPIN_SCALE);
        out.dynamic_threshold = static_cast<double>(thr_scaled)  / static_cast<double>(VPIN_SCALE);

        // vpin_scaled/thr_scaled zaten [0, 1e6] ile sınırlı -> doğrudan int32_t'ye
        // sığar (INT32_MAX ~2.1e9 >> VPIN_SCALE); ekstra clamp gereksiz.
        out.vpin_x1e6              = static_cast<int32_t>(vpin_scaled);
        out.dynamic_threshold_x1e6 = static_cast<int32_t>(thr_scaled);

        // Alarm sıralaması: NONE < WARNING < HIGH < CRITICAL
        VPINAlarm alarm = VPINAlarm::NONE;
        if (state_.valid) {
            const int64_t high_threshold_scaled = (cfg_.crit_threshold_scaled + cfg_.warn_threshold_scaled) / 2;
            if (vpin_scaled >= cfg_.crit_threshold_scaled) {
                alarm = VPINAlarm::CRITICAL;
            } else if (vpin_scaled >= high_threshold_scaled) {
                alarm = VPINAlarm::HIGH;
            } else if (vpin_scaled >= cfg_.warn_threshold_scaled) {
                alarm = VPINAlarm::WARNING;
            }
        }
        out.alarm = alarm;
        out.valid = state_.valid; // int64_t — isfinite kontrolüne gerek yok

        last_alarm_ = alarm;

        if (alarm >= VPINAlarm::CRITICAL && close_cb_) {
            close_cb_(alarm, vpin_scaled);
        }

        return out;
    }

    VPINConfig    cfg_;
    RingBuffer    buckets_;
    VPINBucket    current_;
    VPINState     state_;
    int64_t       window_vol_   = 0;
    uint32_t      window_count_ = 0;
    VPINAlarm     last_alarm_   = VPINAlarm::NONE;
    CloseCallback close_cb_;
    // [PHASE5] bucket hedefi / kalibrasyon durumu
    bool          calibrated_         = false;
    uint32_t      calib_n_            = 0;
    int64_t       calib_sum_          = 0;
    int64_t       eff_bucket_scaled_  = 0;
};

// [V13-COMPAT] Research araclari icin enstruman-bagimsiz VPIN konfigurasyonu:
// OTOMATIK bucket (ilk calib_trades isleminin ortalamasi x T), sabit "1000 birim" yok.
inline VPINConfig make_research_vpin_config(int n_buckets = 50,
                                            double crit_threshold = 0.8) noexcept {
    VPINConfig c{};
    c.n_buckets_window              = std::clamp(n_buckets, 1, static_cast<int>(RingBuffer::RING_CAPACITY));
    c.bucket_volume_scaled          = 0;                                   // otomatik mod
    c.bucket_size_multiplier_scaled = VPINVelocityTracker::MIN_AUTO_T_SCALED; // T = 10 islem/bucket
    c.calib_trades                  = 10;
    c.crit_threshold_scaled         = static_cast<int64_t>(std::llround(
                                          std::clamp(crit_threshold, 0.0, 1.0) * static_cast<double>(VPIN_SCALE)));
    c.warn_threshold_scaled         = (c.crit_threshold_scaled * 3) / 4;
    return c;
}

} // namespace kusak::research

#endif // KUSAK_VPIN_VELOCITY_TRACKER_HPP
