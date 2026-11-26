#pragma once
// ============================================================================
// KUSAK V13 — JSON/parse yardimci fonksiyonlari
//
// [FIX-SEPARATION-1] Bu fonksiyonlar (JSON string/number cikarma, sayisal
// string parse) stratejinin KARAR mantigiyla (OBI/VPIN/Kelly) hicbir ilgisi
// olmayan, saf REST/JSON ayristirma yardimcilaridir. Onceden main.cpp'nin
// tepesine ve strategy_core.hpp'nin icine DAGINIK sekilde serpistirilmisti --
// artik TEK, izole bir dosyada. main.cpp bunu ihtiyaci oldugu icin #include
// eder; strategy_core.hpp (saf karar mantigi) BUNA HIC IHTIYAC DUYMAZ ve
// dahil etmez.
// ============================================================================
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

inline std::optional<std::string_view> json_find_string_view(
    std::string_view body, std::string_view key) noexcept {
    const size_t kp = body.find(key);
    if (kp == std::string_view::npos) return std::nullopt;
    const size_t c = body.find(':', kp + key.size());
    if (c == std::string_view::npos) return std::nullopt;
    const size_t q1 = body.find('"', c + 1);
    if (q1 == std::string_view::npos) return std::nullopt;
    const size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string_view::npos || q2 <= q1 + 1) return std::nullopt;
    return body.substr(q1 + 1, q2 - q1 - 1);
}

inline bool json_find_number_view(std::string_view body,
                                  std::string_view key,
                                  std::string_view& out) noexcept {
    const size_t kp = body.find(key);
    if (kp == std::string_view::npos) return false;
    const size_t c = body.find(':', kp + key.size());
    if (c == std::string_view::npos) return false;
    size_t b = c + 1;
    while (b < body.size() && (body[b] == ' ' || body[b] == '"')) ++b;
    size_t e = b;
    while (e < body.size()) {
        const char ch = body[e];
        const bool ok = (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' ||
                        ch == 'e' || ch == 'E';
        if (!ok) break;
        ++e;
    }
    if (e <= b) return false;
    out = body.substr(b, e - b);
    return true;
}

inline bool parse_u64_sv(std::string_view sv, uint64_t& out) noexcept {
    const char* b = sv.data();
    const char* e = b + sv.size();
    const auto r = std::from_chars(b, e, out);
    return r.ec == std::errc{} && r.ptr == e;
}

inline bool parse_double_sv(std::string_view sv, double& out) noexcept {
    const auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return r.ec == std::errc{} && r.ptr == sv.data() + sv.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// json_extract_string — yalnızca başlangıçta, hot path'te değil
// ─────────────────────────────────────────────────────────────────────────────
inline std::string json_extract_string(const std::string& json,
                                        const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    auto colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return {};
    auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}