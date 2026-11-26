#pragma once
// [FIX-P0-02] POLL sorgu dizesi uretici (header-only, heap yok).
//   order_id != 0           -> symbol=<sym>&orderId=<N>
//   aksi, gecerli client_oid -> symbol=<sym>&origClientOrderId=<coid>
//   baska her durum / tasma  -> false (fail-closed; ag istegi yapilmaz)
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "engine/types.hpp"


[[nodiscard]] inline bool make_poll_query(const OrderTask& t, char* out, size_t out_size) noexcept {
    if (!out || out_size < 2) return false;
    const char*  sym     = t.symbol.c_str();
    const size_t sym_len = std::strlen(sym);
    if (sym_len == 0) return false;

    const char*  key;
    size_t       key_len;
    char         numbuf[24];
    const char*  val;
    size_t       val_len;

    if (t.order_id != 0) {
        key = "&orderId="; key_len = 9;
        auto [ep, ec] = std::to_chars(numbuf, numbuf + sizeof(numbuf), t.order_id);
        if (ec != std::errc{}) return false;
        val = numbuf; val_len = static_cast<size_t>(ep - numbuf);
    } else {
        if (t.client_oid.empty()) return false;
        val     = t.client_oid.c_str();
        val_len = std::strlen(val);
        if (val_len == 0) return false;
        for (size_t i = 0; i < val_len; ++i) {
            const char c = val[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok) return false;
        }
        key = "&origClientOrderId="; key_len = 19;
    }

    const size_t total = 7 + sym_len + key_len + val_len;   // "symbol=" + sym + key + val
    if (total + 1 > out_size) return false;
    char* p = out;
    std::memcpy(p, "symbol=", 7); p += 7;
    std::memcpy(p, sym, sym_len); p += sym_len;
    std::memcpy(p, key, key_len); p += key_len;
    std::memcpy(p, val, val_len); p += val_len;
    *p = '\0';
    return true;
}

