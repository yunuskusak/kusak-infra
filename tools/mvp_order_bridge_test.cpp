// ============================================================================
// tools/mvp_order_bridge_test.cpp
// Tamamen bağımsız MVP köprü testi — main.cpp ve diğer KUSAK-INFRA
// modüllerinden (ConfigManager, ExchangeInfoManager, RestOrderClient) HİÇBİR
// bağımlılığı yoktur. Sadece binance_signer.hpp + ham libcurl kullanır.
//
// Akış: SignedRequestBuilder (saat senk. + imza) -> ham libcurl POST
//       -> Binance Spot TESTNET /api/v3/order -> JSON yanıtı stdout.
// ============================================================================

#include <cstdlib>
#include <iostream>
#include <string>
#include <curl/curl.h>
#include "binance_signer.hpp"

namespace {

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const std::size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}

} // namespace

int main() {
    using namespace kusak::sign;

    // === TESTNET kimlik bilgileri — kendi anahtarlarınla değiştir ===
    // https://testnet.binance.vision üzerinden üretilen SPOT TESTNET anahtarları.
    // [PHASE0-SECRETS] Anahtarlar kaynak koda gomulmez; ortam degiskeninden okunur (src/main.cpp ile ayni adlar).
    const char* env_key    = std::getenv("API_KEY");
    const char* env_secret = std::getenv("API_SECRET");
    if (!env_key || !env_secret || !*env_key || !*env_secret) {
        std::cerr << "HATA: API_KEY/API_SECRET ortam degiskeni yok; ag istegi yapilmadi" << std::endl;
        return 2;
    }
    std::string API_KEY    = env_key;
    std::string API_SECRET = env_secret;

    const std::string BASE_URL = "https://testnet.binance.vision";

    // ── 1) İmzalayıcıyı kur — init() /api/v3/time ile gerçek saat senkronu yapar ──
    SignedRequestBuilder signer;
    try {
        signer.init(API_SECRET, BASE_URL, /*require_sync_success=*/true);
    } catch (const std::exception& e) {
        std::cerr << "Saat senkronizasyonu basarisiz: " << e.what() << "\n";
        return 1;
    }

    // ── 2) Parametreleri kur ve imzala ──────────────────────────────────────
    // symbol=BTCUSDT&side=BUY&type=MARKET&quantity=0.01
    std::string params;
    params += RequestSigner::build_param("symbol",   "BTCUSDT");
    params += RequestSigner::build_param("side",     "BUY");
    params += RequestSigner::build_param("type",     "MARKET");
    params += RequestSigner::build_param("quantity", "0.01");

    const std::string signed_params = signer.sign_query(params);

    // ── 3) Ham libcurl ile POST /api/v3/order ───────────────────────────────
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "curl_easy_init basarisiz\n";
        curl_global_cleanup();
        return 1;
    }

    const std::string url = BASE_URL + "/api/v3/order";
    const std::string api_key_header = "X-MBX-APIKEY: " + API_KEY;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, api_key_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    std::string response_body;
    long http_code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, signed_params.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(signed_params.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        std::cerr << "curl_easy_perform basarisiz: " << curl_easy_strerror(res) << "\n";
        return 1;
    }

    // ── 4) Borsa yanıtını (JSON, 200 OK veya hata gövdesi) stdout'a bas ────
    std::cout << "HTTP " << http_code << "\n" << response_body << std::endl;

    return (http_code == 200) ? 0 : 1;
}
