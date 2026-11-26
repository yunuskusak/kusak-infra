#pragma once
#ifndef KUSAK_ENGINE_MARKET_EVENT_HPP
#define KUSAK_ENGINE_MARKET_EVENT_HPP

// engine/market_event.hpp — V13 Ghost Engine — MarketEvent + PositionSide
// ─────────────────────────────────────────────────────────────────────────────
// main.cpp satır 150–166'dan taşındı. Orijinal kod değiştirilmeden kopyalandı.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include "ws_client.hpp"   // kusak::ws::AggTrade, kusak::ws::BookTicker

// ─────────────────────────────────────────────────────────────────────────────
// [FIX-SPSC-1] MarketEvent — alignas(64) + explicit padding (V12'den değişmedi)
// [V13-FIX] memset(this,0,...) kaldırıldı — non-trivial type UB/segfault.
//           Yerine member initializer list ile zero-init kullanıldı.
// ─────────────────────────────────────────────────────────────────────────────
enum class PositionSide : uint8_t { NONE, LONG, SHORT };

struct alignas(64) MarketEvent {
    enum class Tag : uint8_t { AGG_TRADE, BOOK_TICKER } tag = Tag::AGG_TRADE;
    union {
        kusak::ws::AggTrade   trade;
        kusak::ws::BookTicker ticker;
    };
    uint8_t _pad[16] = {};

    MarketEvent() noexcept : tag{Tag::AGG_TRADE}, trade{}, _pad{} {}
};
static_assert(sizeof(MarketEvent) == 64,
    "[FIX-SPSC-1] MarketEvent 64 byte olmali");
static_assert(alignof(MarketEvent) == 64,
    "[FIX-SPSC-1] MarketEvent 64-byte hizali olmali");

#endif // KUSAK_ENGINE_MARKET_EVENT_HPP
