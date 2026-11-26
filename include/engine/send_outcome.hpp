#pragma once
// [FIX-P36-C15] Yeni emir (SEND/EXIT) sonucunun siniflamasi -- saf fonksiyon, ag/durum yok, birim testlenebilir.
//
// Sorun (C15, KZ-P0-02 kapsami): new_order her hata yolunda error.code != 0 biraktigi icin order thread'in
// "code==0 -> NET_UNKNOWN" dali hic calismiyordu; transport hatasi / zaman asimi / 5xx "gonderilmedi" (REJECTED)
// sayiliyordu. Borsada canli olabilecek emir yok sayiliyordu (CLAUDE.md §3.4 ihlali).
//
// Kural (fail-closed): yalniz istegin ag'a CIKMADIGI (LOCAL_REJECT) veya borsanin emri ISLEMEDIGINI acikca
// bildirdigi (EXCHANGE_REJECT) durumlar kesin rettir. Geri kalan her durum AMBIGUOUS'tur:
// emir borsada olabilir -> UNKNOWN + kill switch + origClientOrderId ile POLL.
#include <cstdint>

namespace kusak {

enum class SendOutcome : uint8_t {
    ACCEPTED        = 0,   // 2xx + orderId + status ayristi
    LOCAL_REJECT    = 1,   // yerel filtre / istemci yok: istek ag'a CIKMADI
    EXCHANGE_REJECT = 2,   // borsa Binance kodlu 4xx ile reddetti (429/-1003 dahil): emir islenmedi
    AMBIGUOUS       = 3,   // emir borsada olabilir (varsayilan; fail-closed)
    NOT_SENT        = 4,   // [FIX-P37-C19A] transport hatasi + curl kaniti: istegin tek bayti bile gonderilmedi
};

enum class SendStage : uint8_t {
    LOCAL          = 0,    // http cagrisi yapilmadi
    HTTP_RESPONSE  = 1,    // http_func dondu (status_code gecerli veya <= 0 transport)
    PARSE_FAIL_2XX = 2,    // 2xx geldi ama orderId/status ayristirilamadi
    EXCEPTION      = 3,    // http cagrisi sirasinda/sonrasinda istisna
};

inline const char* send_outcome_name(SendOutcome o) noexcept {
    switch (o) {
        case SendOutcome::ACCEPTED:        return "ACCEPTED";
        case SendOutcome::LOCAL_REJECT:    return "LOCAL_REJECT";
        case SendOutcome::EXCHANGE_REJECT: return "EXCHANGE_REJECT";
        case SendOutcome::AMBIGUOUS:       return "AMBIGUOUS";
        case SendOutcome::NOT_SENT:        return "NOT_SENT";
    }
    return "AMBIGUOUS";
}

// Binance "execution status unknown" kodlari. [DOGRULANMADI] kod anlamlari resmi dokumanla test edilmedi:
//  -1006 UNEXPECTED_RESP, -1007 TIMEOUT (backend'den yanit gelmedi; durum bilinmiyor).
constexpr bool is_status_unknown_code(int code) noexcept {
    return code == -1006 || code == -1007;
}

// http_status: HttpResponse::status_code (transport hatasinda <= 0).
// parsed_code: govdeden ayristirilan Binance kodu; ayristirilamadiysa 0.
// request_sent: [FIX-P37-C19A] HttpResponse::request_sent; YALNIZ http_status <= 0 iken dikkate alinir
//               (HTTP yaniti geldiyse istek gitmistir). Varsayilan true = kanit yok = belirsiz.
constexpr SendOutcome classify_send_outcome(SendStage stage, long http_status, int parsed_code,
                                            bool request_sent = true) noexcept {
    switch (stage) {
        case SendStage::LOCAL:          return SendOutcome::LOCAL_REJECT;
        case SendStage::PARSE_FAIL_2XX: return SendOutcome::AMBIGUOUS;
        case SendStage::EXCEPTION:      return SendOutcome::AMBIGUOUS;
        case SendStage::HTTP_RESPONSE:  break;
    }
    if (http_status <= 0)                    // transport / timeout
        return request_sent ? SendOutcome::AMBIGUOUS : SendOutcome::NOT_SENT;   // [FIX-P37-C19A] yalniz kanitla
    if (http_status >= 200 && http_status < 300) return SendOutcome::ACCEPTED;
    if (http_status >= 500)                  return SendOutcome::AMBIGUOUS;   // 5xx: durum bilinmiyor
    if (parsed_code == 0)                    return SendOutcome::AMBIGUOUS;   // kodsuz 4xx (WAF/HTML/proxy)
    if (is_status_unknown_code(parsed_code)) return SendOutcome::AMBIGUOUS;
    return SendOutcome::EXCHANGE_REJECT;                                     // Binance kodlu 4xx
}

// Derleme zamani tablo kontrolu (davranis testi tools/phase36_test.cpp'de).
static_assert(classify_send_outcome(SendStage::LOCAL, 0, 0)           == SendOutcome::LOCAL_REJECT);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, -1, 0)  == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 503, -1001) == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 403, 0) == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 400, -1007) == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 400, -2010) == SendOutcome::EXCHANGE_REJECT);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 429, -1003) == SendOutcome::EXCHANGE_REJECT);
static_assert(classify_send_outcome(SendStage::PARSE_FAIL_2XX, 200, 0) == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::EXCEPTION, 0, 0)       == SendOutcome::AMBIGUOUS);
// [FIX-P37-C19A]
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, -1, 0, false)  == SendOutcome::NOT_SENT);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, -1, 0, true)   == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::HTTP_RESPONSE, 503, 0, false) == SendOutcome::AMBIGUOUS);
static_assert(classify_send_outcome(SendStage::EXCEPTION, 0, 0, false)       == SendOutcome::AMBIGUOUS);

} // namespace kusak
