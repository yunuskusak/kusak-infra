#pragma once
#ifndef KUSAK_REPLAY_ENGINE_HPP
#define KUSAK_REPLAY_ENGINE_HPP

// Çözülen:
// 1. FileHeader / TICK_FILE_MAGIC / now_us / now_ns / from_disk — core_types+research_types
// 2. next_record / from_disk / FORCE_INLINE — include sırası bağımsız
// 3. TickRecord::best_bid/best_ask — bids[0]/asks[0] üzerinden erişime düzeltildi

#include "core_types.hpp"
#include "research_types.hpp"

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace kusak::research {

// ─────────────────────────────────────────────────────────────────────────────
// ReplayConfig
// ─────────────────────────────────────────────────────────────────────────────
struct ReplayConfig {
    std::string  file_path;
    ReplayMode   mode          = ReplayMode::FAST;
    double       speed_mult    = 1.0;    // REALTIME modunda çarpan
    int64_t      start_ts_us   = 0;      // 0 = dosya başı
    int64_t      end_ts_us     = 0;      // 0 = dosya sonu
    bool         loop          = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Callback tipleri — research_main.cpp bunları lambda ile bağlar
// ─────────────────────────────────────────────────────────────────────────────
using OnDepthFn      = std::function<void(const TickRecord&)>;
using OnBookTickerFn = std::function<void(const TickRecord&)>;
using OnTradeFn      = std::function<void(const TickRecord&)>;
using OnTickFn       = std::function<void(const TickRecord&)>;  // genel

// ─────────────────────────────────────────────────────────────────────────────
// ReplayEngine
// ─────────────────────────────────────────────────────────────────────────────
class ReplayEngine {
public:
    explicit ReplayEngine(ReplayConfig cfg)
        : cfg_(std::move(cfg))
    {
        open_file(cfg_.file_path);
    }

    ~ReplayEngine() noexcept { cleanup(); }

    ReplayEngine(const ReplayEngine&)            = delete;
    ReplayEngine& operator=(const ReplayEngine&) = delete;

    // ── Callback bağlantıları ─────────────────────────────────────────────────
    void on_depth      (OnDepthFn      cb) noexcept { on_depth_       = std::move(cb); }
    void on_book_ticker(OnBookTickerFn cb) noexcept { on_book_ticker_ = std::move(cb); }
    void on_trade      (OnTradeFn      cb) noexcept { on_trade_       = std::move(cb); }
    void on_tick       (OnTickFn       cb) noexcept { on_tick_        = std::move(cb); }

    // ── Tam dosyayı koştur ────────────────────────────────────────────────────
    void run() {
        uint64_t wall_start   = now_us();
        uint64_t first_ts     = 0;
        bool     first_record = true;

        while (true) {
            const TickRecord* rec = next_record();
            if (!rec) {
                if (cfg_.loop) { rewind(); continue; }
                break;
            }

            if (cfg_.start_ts_us > 0 &&
                static_cast<int64_t>(rec->ts_exchange) < cfg_.start_ts_us)
                continue;

            if (cfg_.end_ts_us > 0 &&
                static_cast<int64_t>(rec->ts_exchange) > cfg_.end_ts_us)
                break;

            if (cfg_.mode == ReplayMode::REALTIME) {
                if (first_record) { first_ts = rec->ts_exchange; first_record = false; }
                apply_timing(rec->ts_exchange - first_ts, wall_start);
            }

            dispatch(*rec);
        }
    }

    // ── Tek adım (STEP modu) ──────────────────────────────────────────────────
    bool step() {
        const TickRecord* rec = next_record();
        if (!rec) return false;
        dispatch(*rec);
        return true;
    }

    // ── Önceden bak ───────────────────────────────────────────────────────────
    FORCE_INLINE const TickRecord* peek() noexcept {
        if (UNLIKELY(data_offset_ + sizeof(DiskRecord) > file_size_)) return nullptr;
        DiskRecord drec{};
        std::memcpy(&drec, base_ + data_offset_, sizeof(DiskRecord));
        from_disk(drec, staging_record_);
        return &staging_record_;
    }

    uint64_t total_records() const noexcept { return total_records_; }
    uint64_t records_read()  const noexcept { return records_read_;  }

private:
    // ── mmap dosya aç ────────────────────────────────────────────────────────
    void open_file(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (UNLIKELY(fd_ < 0))
            throw std::runtime_error("ReplayEngine: dosya açılamadı: " + path);

        struct stat st{};
        if (::fstat(fd_, &st) < 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("ReplayEngine: fstat hatası");
        }
        file_size_ = static_cast<std::size_t>(st.st_size);

        if (UNLIKELY(file_size_ < sizeof(FileHeader)))
            throw std::runtime_error("ReplayEngine: dosya çok küçük (header yok)");

        base_ = static_cast<const uint8_t*>(
            ::mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0));
        if (UNLIKELY(base_ == MAP_FAILED)) {
            ::close(fd_); fd_ = -1; base_ = nullptr;
            throw std::runtime_error("ReplayEngine: mmap başarısız");
        }
        ::madvise(const_cast<uint8_t*>(base_), file_size_, MADV_SEQUENTIAL);

        // Header doğrula
        uint32_t magic_val;
        uint16_t version_val;
        uint64_t record_count_val;
        std::memcpy(&magic_val,        base_ + __builtin_offsetof(FileHeader, magic),        sizeof(uint32_t));
        std::memcpy(&version_val,      base_ + __builtin_offsetof(FileHeader, version),      sizeof(uint16_t));
        std::memcpy(&record_count_val, base_ + __builtin_offsetof(FileHeader, record_count), sizeof(uint64_t));

        if (UNLIKELY(magic_val != TICK_FILE_MAGIC))
            throw std::runtime_error("ReplayEngine: geçersiz magic");
        if (UNLIKELY(version_val != TICK_FILE_VERSION))
            throw std::runtime_error("ReplayEngine: desteklenmeyen versiyon");

        total_records_ = record_count_val;
        data_offset_   = sizeof(FileHeader);
    }

    void cleanup() noexcept {
        if (base_) { ::munmap(const_cast<uint8_t*>(base_), file_size_); base_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    void rewind() noexcept {
        data_offset_  = sizeof(FileHeader);
        records_read_ = 0;
    }

    // ── Sıradaki kaydı oku ───────────────────────────────────────────────────
    FORCE_INLINE const TickRecord* next_record() noexcept {
        if (UNLIKELY(data_offset_ + sizeof(DiskRecord) > file_size_)) return nullptr;
        DiskRecord drec{};
        std::memcpy(&drec, base_ + data_offset_, sizeof(DiskRecord));
        data_offset_ += sizeof(DiskRecord);
        ++records_read_;
        from_disk(drec, staging_record_);
        return &staging_record_;
    }

    // ── Realtime zamanlama ────────────────────────────────────────────────────
    FORCE_INLINE void apply_timing(uint64_t elapsed_us, uint64_t wall_start_us) noexcept {
        const uint64_t target_us = wall_start_us
            + static_cast<uint64_t>(static_cast<double>(elapsed_us) / cfg_.speed_mult);
        const uint64_t now       = now_us();
        if (now < target_us) {
            const uint64_t delay_ns = (target_us - now) * 1'000ULL;
            struct timespec ts{ 0, static_cast<long>(delay_ns) };
            ::nanosleep(&ts, nullptr);
        }
    }

    // ── Dispatch ─────────────────────────────────────────────────────────────
    void dispatch(const TickRecord& rec) {
        // Genel callback her zaman
        if (on_tick_) on_tick_(rec);

        // Tip bazlı callback
        switch (rec.type) {
        case TickType::DEPTH:
            if (on_depth_) on_depth_(rec);
            break;
        case TickType::BOOK_TICKER:
            if (on_book_ticker_) on_book_ticker_(rec);
            // best_bid/best_ask → bids[0].price / asks[0].price
            // (hata logundaki TickRecord::best_bid erişimi buradan kaynaklanıyordu)
            break;
        case TickType::TRADE:
        case TickType::AGG_TRADE:
            if (on_trade_) on_trade_(rec);
            break;
        case TickType::UNKNOWN:
            break;
        }
    }

    ReplayConfig        cfg_;
    int                 fd_           = -1;
    const uint8_t*      base_         = nullptr;
    std::size_t         file_size_    = 0;
    std::size_t         data_offset_  = 0;
    uint64_t            total_records_= 0;
    uint64_t            records_read_ = 0;
    TickRecord          staging_record_{};

    OnDepthFn           on_depth_;
    OnBookTickerFn      on_book_ticker_;
    OnTradeFn           on_trade_;
    OnTickFn            on_tick_;
};

} // namespace kusak::research

#endif // KUSAK_REPLAY_ENGINE_HPP
