#pragma once
#ifndef KUSAK_CORE_TYPES_HPP
#define KUSAK_CORE_TYPES_HPP

// core_types.hpp — V14 Ghost Engine — Nanosecond-Deterministic / mmap-Safe
// ─────────────────────────────────────────────────────────────────────────
//
// TASARIM KARARLARI:
//   1. FileHeader.record_count → düz `uint64_t` (trivially-copyable / mmap-safe)
//      Erişim SADECE `atomic_record_count_load/store/inc` arayüzleri ile yapılır.
//      Bu arayüzler std::atomic_ref<uint64_t> kullanır → lock-free + mmap uyumlu.
//   2. TÜM FileHeader field offset'leri static_assert ile DONDURULDU.
//   3. make_tick_filename: localtime_r / strftime YOKTUR.
//      Manuel UTC aritmetik (civil_from_days) + integer→ASCII. %100 lock-free.
//   4. TradeRecord: alignas(64), 128 byte (2 cache line), SPSC-queue uyumlu.
//   5. TÜM kullanıcı kodu `namespace kusak` içindedir.

// ── Standart başlıklar ───────────────────────────────────────────────────────
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <cassert>
#include <ctime>          // clock_gettime (vDSO fast-path)
#include <atomic>
#include <string_view>
#include <span>
#include <array>
#include <type_traits>
#include <string>      // make_tick_filename_legacy_alloc wrapper için

// ════════════════════════════════════════════════════════════════════════════
// §0  COMPILER / PLATFORM MAKROLARI  (preprocessor — namespace dışında)
// ════════════════════════════════════════════════════════════════════════════

#ifndef FORCE_INLINE
#  if defined(__GNUC__) || defined(__clang__)
#    define FORCE_INLINE __attribute__((always_inline)) inline
#  elif defined(_MSC_VER)
#    define FORCE_INLINE __forceinline
#  else
#    define FORCE_INLINE inline
#  endif
#endif

#ifndef LIKELY
#  if defined(__GNUC__) || defined(__clang__)
#    define LIKELY(x)   __builtin_expect(!!(x), 1)
#    define UNLIKELY(x) __builtin_expect(!!(x), 0)
#  else
#    define LIKELY(x)   (x)
#    define UNLIKELY(x) (x)
#  endif
#endif

// Cache line boyutu — macro ile override edilebilir (ARM big cores: 128)
#ifndef CACHE_LINE_SIZE
#  define CACHE_LINE_SIZE 64
#endif

#ifndef KUSAK_PACKED
#  define KUSAK_PACKED __attribute__((packed))
#endif

#ifndef KUSAK_ALIGNED
#  define KUSAK_ALIGNED(n) __attribute__((aligned(n)))
#endif

// ════════════════════════════════════════════════════════════════════════════
namespace kusak {
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
// §1  ZAMAN FONKSİYONLARI  (lock-free, async-signal-safe, vDSO fast-path)
// ════════════════════════════════════════════════════════════════════════════
// Not: Linux'ta clock_gettime CLOCK_REALTIME ve CLOCK_MONOTONIC_RAW
// vDSO üzerinden çalışır — gerçek syscall'a düşmez (~15-25 ns).

FORCE_INLINE uint64_t now_ns() noexcept {
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec)  * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

FORCE_INLINE uint64_t now_us() noexcept {
    return now_ns() / 1'000ULL;
}

FORCE_INLINE uint64_t now_ms() noexcept {
    return now_ns() / 1'000'000ULL;
}

// Monotonic varyant — gecikme ölçümü için (CLOCK_MONOTONIC_RAW tercihli)
FORCE_INLINE uint64_t mono_ns() noexcept {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec)  * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

// ════════════════════════════════════════════════════════════════════════════
// §2  TICK DOSYA FORMAT SABİTLERİ
// ════════════════════════════════════════════════════════════════════════════

static constexpr uint32_t TICK_FILE_MAGIC   = 0x4B53414Bu; // "KSAK"
static constexpr uint16_t TICK_FILE_VERSION = 0x0002u;
static constexpr int      MAX_DEPTH         = 20;

// ════════════════════════════════════════════════════════════════════════════
// §2.1  LOCK-FREE UTC FORMATLAMA YARDIMCILARI
// ════════════════════════════════════════════════════════════════════════════

// 2-basamaklı integer → char[2] (branchless, lock-free, zero-padded)
FORCE_INLINE void fmt_2d(char* p, uint32_t v) noexcept {
    p[0] = static_cast<char>('0' + v / 10u);
    p[1] = static_cast<char>('0' + v % 10u);
}

// 4-basamaklı integer → char[4] (branchless, lock-free, zero-padded)
FORCE_INLINE void fmt_4d(char* p, uint32_t v) noexcept {
    p[0] = static_cast<char>('0' + v / 1000u);
    p[1] = static_cast<char>('0' + (v / 100u) % 10u);
    p[2] = static_cast<char>('0' + (v / 10u)  % 10u);
    p[3] = static_cast<char>('0' + v % 10u);
}

// Howard Hinnant civil_from_days algoritması
// Input:  Unix epoch'tan (1970-01-01) bu yana gün sayısı
// Output: year, month [1-12], day [1-31]
// Özellik: Sıfır branch, sıfır lookup, sıfır syscall, tamamen aritmetik.
FORCE_INLINE void civil_from_days(int64_t z,
                                   int32_t& year,
                                   uint32_t& month,
                                   uint32_t& day) noexcept {
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int32_t  y   = static_cast<int32_t>(static_cast<int64_t>(yoe) + era * 400);
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp  = (5u * doy + 2u) / 153u;
    day   = doy - (153u * mp + 2u) / 5u + 1u;
    month = mp < 10u ? mp + 3u : mp - 9u;
    year  = y + (month <= 2u ? 1 : 0);
}

// Inline küçük string kopyalama (<32 byte): memcpy call overhead'i yok
// Büyük veriler (>=32 byte) için memcpy kullanılmalıdır.
FORCE_INLINE char* inline_copy(char* __restrict__ dst,
                                const char* __restrict__ src,
                                std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = src[i];
    return dst + n;
}

// ════════════════════════════════════════════════════════════════════════════
// §3  FileHeader — mmap başlığı, 256-byte fixed, trivially-copyable
// ════════════════════════════════════════════════════════════════════════════
//
// V14 Nanosecond-Deterministic Cache-Line Mimarisi:
//
//   ┌─────────────────────────────────────────────────────────┐
//   │  BÖLGE A — Read-Only / Immutable (CL0: offset 0–63)   │
//   │  magic, version, reserved0, ts_created, symbol, pad    │
//   │  ⚠ symbol: init sonrası IMMUTABLE — runtime yazımı UB! │
//   ├─────────────────────────────────────────────────────────┤
//   │  BÖLGE B — Hot Atomic Write  (CL1: offset 64–127)     │
//   │  record_count [uint64_t → atomic_ref erişim] — İZOLE   │
//   ├─────────────────────────────────────────────────────────┤
//   │  BÖLGE C — Reserved           (CL2-3: offset 128–255) │
//   └─────────────────────────────────────────────────────────┘
//
// mmap Uyumluluk: struct trivially-copyable kalır.
//   record_count'a SADECE atomic_record_count_* fonksiyonları ile erişilir.
// Stride = 256 byte = 4 × cache line → FileHeader[] dizisinde false sharing = 0.

struct alignas(64) FileHeader {
    // ── BÖLGE A: Read-Only / Immutable (offset 0–63) ────────────────────
    uint32_t magic;                            //  0: TICK_FILE_MAGIC
    uint16_t version;                          //  4: TICK_FILE_VERSION
    uint16_t reserved0;                        //  6: alignment padding
    uint64_t ts_created;                       //  8: Unix µs — dosya oluşturma zamanı

    // ⚠ IMMUTABLE: symbol alanı dosya oluşturulurken bir kez yazılır.
    //   Runtime sonrası değiştirilmesi yasaktır — Cache Line 0'ı bozar.
    char     symbol[32];                       // 16: enstrüman (null-terminated)

    uint8_t  _pad_a[16];                       // 48: deterministik dolgu → CL sınırı
    // ── 4+2+2+8+32+16 = 64 byte ✓ ──────────────────────────────────────

    // ── BÖLGE B: Hot / Write-Heavy (offset 64–127) ──────────────────────
    // ⚠ Bu alana doğrudan erişme! atomic_record_count_* fonksiyonlarını kullan.
    alignas(64) uint64_t record_count;         // 64: atomic_ref ile lock-free erişim
    uint8_t  _pad_b[56];                       // 72: CL dolgusu → 64 byte izolasyon
    // ── 8+56 = 64 byte ✓ ───────────────────────────────────────────────

    // ── BÖLGE C: Reserved / Gelecek Genişleme (offset 128–255) ──────────
    uint8_t  _reserved[128];                   // 128: 2 cache line, gelecek kullanım
    // ── 128 byte ✓ ─────────────────────────────────────────────────────
};

// ── Derleme Zamanı Donanım Garantileri — FROZEN LAYOUT ────────────────────
// Bu blok altındaki hiçbir offset değiştirilemez.

// Boyut, hizalama, stride
static_assert(sizeof(FileHeader) == 256,
              "FileHeader MUST be exactly 256 bytes for mmap compatibility");
static_assert(alignof(FileHeader) == 64,
              "FileHeader MUST be 64-byte aligned for cache-line boundaries");
static_assert(sizeof(FileHeader) % 64 == 0,
              "FileHeader stride MUST be a multiple of cache line size");

// Trivially-copyable + Standard Layout → mmap + offsetof güvenli
static_assert(std::is_trivially_copyable_v<FileHeader>,
              "FileHeader MUST be trivially copyable for mmap compatibility");
static_assert(std::is_standard_layout_v<FileHeader>,
              "FileHeader MUST be standard layout for offsetof correctness");

// Bölge A field offset'leri — FROZEN
static_assert(offsetof(FileHeader, magic)      == 0,  "magic FROZEN at offset 0");
static_assert(offsetof(FileHeader, version)    == 4,  "version FROZEN at offset 4");
static_assert(offsetof(FileHeader, reserved0)  == 6,  "reserved0 FROZEN at offset 6");
static_assert(offsetof(FileHeader, ts_created) == 8,  "ts_created FROZEN at offset 8");
static_assert(offsetof(FileHeader, symbol)     == 16, "symbol FROZEN at offset 16");
static_assert(offsetof(FileHeader, _pad_a)     == 48, "_pad_a FROZEN at offset 48");

// Bölge B — cache-line izolasyon garantisi — FROZEN
static_assert(offsetof(FileHeader, record_count) == 64,
              "record_count MUST start at offset 64 (dedicated cache line)");
static_assert(offsetof(FileHeader, record_count) % 64 == 0,
              "record_count MUST be cache-line aligned (mod 64 == 0)");
static_assert(offsetof(FileHeader, _pad_b) == 72, "_pad_b FROZEN at offset 72");

// Bölge C — FROZEN
static_assert(offsetof(FileHeader, _reserved) == 128, "_reserved FROZEN at offset 128");

// atomic_ref hizalama uyumu
static_assert(alignof(uint64_t) >= std::atomic_ref<uint64_t>::required_alignment,
              "uint64_t alignment must satisfy atomic_ref requirements");
static_assert(offsetof(FileHeader, record_count) %
              std::atomic_ref<uint64_t>::required_alignment == 0,
              "record_count offset must be aligned for atomic_ref access");

// atomic_ref lock-free garantisi
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free,
              "std::atomic_ref<uint64_t> MUST be always lock-free");

// ── FileHeader Atomic Erişim Arayüzü ─────────────────────────────────────
// record_count'a tüm erişimler BU fonksiyonlar üzerinden yapılır.
// mmap uyumlu: struct trivially-copyable kalır, atomic_ref geçici sarmalayıcıdır.

FORCE_INLINE uint64_t atomic_record_count_load(FileHeader& h,
        std::memory_order order = std::memory_order_acquire) noexcept {
    return std::atomic_ref<uint64_t>(h.record_count).load(order);
}

FORCE_INLINE void atomic_record_count_store(FileHeader& h, uint64_t val,
        std::memory_order order = std::memory_order_release) noexcept {
    std::atomic_ref<uint64_t>(h.record_count).store(val, order);
}

FORCE_INLINE uint64_t atomic_record_count_inc(FileHeader& h,
        std::memory_order order = std::memory_order_acq_rel) noexcept {
    return std::atomic_ref<uint64_t>(h.record_count).fetch_add(1u, order);
}

// ════════════════════════════════════════════════════════════════════════════
// §4  make_tick_filename — Zero-Allocation, Lock-Free, Deterministic
// ════════════════════════════════════════════════════════════════════════════
//
// localtime_r: YOK (mutex + timezone lookup → non-deterministic)
// strftime:    YOK (locale + branch-heavy → pipeline break)
// Yöntem:      Manuel UTC aritmetik + integer→ASCII, %100 lock-free.
//
// API:
//   Mode A (Safe):     span<char> buffer — runtime bounds check
//   Mode B (Hot-Path): array<char, N> — compile-time capacity garanti
//   Eski API:          2-arg uyumluluk wrapper → std::string (rotate_file uyumu)
//
// Format: {base_dir}/{symbol}_{YYYYMMDD_HHMMSS}.tick
// Timestamp: TAM 15 karakter. Sağlanamazsa fonksiyon 0 döner (fail).

static constexpr std::size_t TICK_FILENAME_BUF_SIZE = 128;
static constexpr std::size_t TIMESTAMP_EXACT_LEN    = 15; // YYYYMMDD_HHMMSS

// ── Mode A: Safe — runtime buffer kontrolü ────────────────────────────────
[[nodiscard]]
FORCE_INLINE std::size_t make_tick_filename(std::string_view symbol,
                                             std::string_view base_dir,
                                             std::span<char>  buffer) noexcept {
    // Buffer kapasite kontrolü
    // base_dir + '/' + symbol + '_' + 15(timestamp) + 5(.tick) + 1(\0)
    const std::size_t required = base_dir.size() + 1u
                               + symbol.size()  + 1u
                               + TIMESTAMP_EXACT_LEN + 5u + 1u;

    if (UNLIKELY(buffer.size() < required)) {
        if (!buffer.empty()) buffer[0] = '\0';
        return 0;
    }

    // ── UTC zaman hesaplama — %100 lock-free, sıfır mutex ──
    const uint64_t us        = now_us();
    const int64_t  total_sec = static_cast<int64_t>(us / 1'000'000ULL);
    const int64_t  days      = total_sec / 86400;
    const uint32_t day_sec   = static_cast<uint32_t>(total_sec - days * 86400);

    int32_t  year;
    uint32_t month, day;
    civil_from_days(days, year, month, day);

    const uint32_t hour = day_sec / 3600u;
    const uint32_t min  = (day_sec % 3600u) / 60u;
    const uint32_t sec  = day_sec % 60u;

    // ── Lineer yazım: tek buffer, sıralı, branchless ──
    char* ptr = buffer.data();

    // base_dir kopyala (değişken uzunluk)
    if (base_dir.size() < 32u) {
        ptr = inline_copy(ptr, base_dir.data(), base_dir.size());
    } else {
        std::memcpy(ptr, base_dir.data(), base_dir.size());
        ptr += base_dir.size();
    }

    // '/'
    *ptr++ = '/';

    // symbol kopyala (tipik <16 byte, inline copy optimal)
    ptr = inline_copy(ptr, symbol.data(), symbol.size());

    // '_'
    *ptr++ = '_';

    // YYYYMMDD_HHMMSS → tam 15 karakter, integer→ASCII
    char* const ts_start = ptr;
    fmt_4d(ptr, static_cast<uint32_t>(year)); ptr += 4;
    fmt_2d(ptr, month);                       ptr += 2;
    fmt_2d(ptr, day);                         ptr += 2;
    *ptr++ = '_';
    fmt_2d(ptr, hour);                        ptr += 2;
    fmt_2d(ptr, min);                         ptr += 2;
    fmt_2d(ptr, sec);                         ptr += 2;

    // Timestamp length doğrulama: KESİNLİKLE 15 olmalı
    const std::size_t ts_len = static_cast<std::size_t>(ptr - ts_start);
    if (UNLIKELY(ts_len != TIMESTAMP_EXACT_LEN)) {
        buffer[0] = '\0';
        return 0;
    }

    // ".tick" → doğrudan karakter ataması (memcpy overhead yok)
    *ptr++ = '.';
    *ptr++ = 't';
    *ptr++ = 'i';
    *ptr++ = 'c';
    *ptr++ = 'k';

    // null terminator
    *ptr = '\0';

    return static_cast<std::size_t>(ptr - buffer.data());
}

// ── Mode B: Hot-Path — compile-time buffer boyut garantisi ────────────────
template<std::size_t N>
[[nodiscard]]
FORCE_INLINE std::size_t make_tick_filename(std::string_view symbol,
                                             std::string_view base_dir,
                                             std::array<char, N>& buffer) noexcept {
    static_assert(N >= TICK_FILENAME_BUF_SIZE,
                  "Buffer too small: minimum TICK_FILENAME_BUF_SIZE (128) required");
    return make_tick_filename(symbol, base_dir,
                              std::span<char>(buffer.data(), buffer.size()));
}

// ── Eski API Uyumluluk Wrapper ────────────────────────────────────────────
// market_data_recorder.hpp rotate_file() geriye uyumluluk için.
// Bu fonksiyon heap allocation yapar — SADECE cold-path (dosya rotasyonu) için!
// NOT: <string> dosya başında include edildi (namespace dışında olmalı)
[[nodiscard]]
inline std::string make_tick_filename_legacy_alloc(const std::string& symbol,
                                      const std::string& base_dir) noexcept {
    std::array<char, TICK_FILENAME_BUF_SIZE> buf{};
    const std::size_t len = make_tick_filename(
        std::string_view(symbol), std::string_view(base_dir),
        std::span<char>(buf.data(), buf.size()));
    return (len > 0) ? std::string(buf.data(), len) : std::string{};
}

// ════════════════════════════════════════════════════════════════════════════
// §5  TEMEL TİCARET KAYIT YAPILARI
// ════════════════════════════════════════════════════════════════════════════

// TickType — tüm bileşenler aynı enum kullanır
enum class TickType : uint8_t {
    UNKNOWN     = 0,
    DEPTH       = 1,
    BOOK_TICKER = 2,
    TRADE       = 3,
    AGG_TRADE   = 4,

    // [V13-COMPAT] Eski research kodu DEPTH_UPDATE adini kullaniyordu; V13'te DEPTH.
    // Ayni sayisal deger -> diskteki .tick dosya formati degismez.
    DEPTH_UPDATE = DEPTH,
};

// TradeRecord — SPSC kuyruk optimizasyonu için alignas(64), 128 byte sabit.
// Layout:
//   ts_us(8) + price(8) + qty(8) + pnl(8) + commission(8) + side(4) +
//   reason(64) = 108 byte → +20 byte padding → 128 byte (2 cache line)
struct alignas(64) TradeRecord {
    uint64_t ts_us      = 0;
    double   price      = 0.0;
    double   qty        = 0.0;
    double   pnl        = 0.0;
    double   commission = 0.0;
    int      side       = 0;   // +1 long  / -1 short
    char     reason[64] = {};  // std::string yerine fixed-size: lock-free güvenli
    char     _pad[20]   = {};  // deterministik dolgu → 128 byte sınırı
};

static_assert(sizeof(TradeRecord) == 128,
              "TradeRecord MUST be exactly 128 bytes (2 cache lines) for SPSC");
static_assert(alignof(TradeRecord) == 64,
              "TradeRecord MUST be 64-byte aligned");
static_assert(std::is_trivially_copyable_v<TradeRecord>,
              "TradeRecord MUST be trivially copyable for SPSC queue memcpy");

// ════════════════════════════════════════════════════════════════════════════
} // namespace kusak
// ════════════════════════════════════════════════════════════════════════════

#endif // KUSAK_CORE_TYPES_HPP
