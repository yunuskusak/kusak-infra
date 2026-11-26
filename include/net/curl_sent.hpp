#pragma once
// [FIX-P37-C19A] "Istegin tek bayti bile gonderilmedi" kaniti (libcurl).
//
// Sorun (T0 testnet gozlemi, 2026-11-24): baglanti asamasinda dusen bir emir POST'u (http=-1) belirsiz (UNKNOWN)
// sayilip kill switch'i acti; allOrders emrin borsaya hic ulasmadigini gosterdi.
// Kural (fail-closed): false YALNIZ curl hicbir istek baytinin gonderilmedigini raporladiginda doner:
//   PRETRANSFER_TIME == 0  (aktarim asamasina hic gelinmedi: DNS/TCP/TLS tamamlanmadi) VE
//   REQUEST_SIZE     == 0  (gonderilen istek bayti yok).
// getinfo hatasi veya herhangi bir pozitif deger => true (gonderilmis olabilir -> belirsiz).
// Kanit: tools/curl_sent_probe_test.cpp (libcurl 8.5.0, 127.0.0.1): ret -> 0/0; timeout/yanitsiz kapanis/200 ->
// pretransfer>0 ve request_size = sunucunun okudugu bayt. [DOGRULANMADI] baglanti zaman asimi ve TLS el sikisma hatasi.
#include <curl/curl.h>

namespace kusak::net {

inline bool curl_request_maybe_sent(CURL* h) noexcept {
    if (!h) return true;
    curl_off_t pretransfer_us = 0;
    long       request_size   = 0;
    if (curl_easy_getinfo(h, CURLINFO_PRETRANSFER_TIME_T, &pretransfer_us) != CURLE_OK) return true;
    if (curl_easy_getinfo(h, CURLINFO_REQUEST_SIZE, &request_size) != CURLE_OK)         return true;
    return !(pretransfer_us == 0 && request_size == 0);
}

} // namespace kusak::net
