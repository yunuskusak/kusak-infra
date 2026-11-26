// ============================================================================
// KUSAK V9 — RESEARCH FRAMEWORK
// notification.hpp — Bildirim Sistemi (Telegram / Slack / Webhook)
//
// Rapor §3: KUSAK'ta işlem yapıldığında kullanıcıya haber verecek
// mekanizma yoktu. Bu modül Telegram Bot API ve Slack Webhook
// entegrasyonu sağlar.
//
// Kullanım:
//   NotificationConfig cfg;
//   cfg.telegram.enabled = true;
//   cfg.telegram.bot_token = "TOKEN";
//   cfg.telegram.chat_id = "CHAT_ID";
//   Notifier notifier(cfg);
//   notifier.send_trade_open(price, qty, true, "BTCUSDT");
//   notifier.send_trade_close(exit_price, pnl, "tp");
//
// Önemli: Ağ istekleri non-blocking (thread pool ile) yapılır.
// Thread safety: send_*() fonksiyonları thread-safe'dir.
//
// NOT: Gerçek HTTP POST için libcurl veya sistem curl komutu kullanılır.
// Derleme: KUSAK_NOTIFY_CURL define edilerek etkin hale getirilir.
// ============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <iostream>
#include <cstring>
#include <cerrno>
// POSIX — fork/execvp/waitpid
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace kusak::notify {

// ============================================================================
// §1  Kanal Konfigürasyonları
// ============================================================================

struct TelegramConfig {
    bool        enabled   = false;
    std::string bot_token;              // "1234567890:AAF..."
    std::string chat_id;                // "@channel" veya "123456789"
    bool        verbose   = false;      // hata loglarını stdout'a yaz
};

struct SlackConfig {
    bool        enabled    = false;
    std::string webhook_url;            // "https://hooks.slack.com/services/..."
    std::string channel;                // "#kusak-alerts"
    bool        verbose    = false;
};

struct WebhookConfig {
    bool        enabled    = false;
    std::string url;                    // özel webhook URL
    std::string secret;                 // isteğe bağlı Authorization header
};

struct NotificationConfig {
    TelegramConfig  telegram;
    SlackConfig     slack;
    WebhookConfig   webhook;

    // Hangi olaylar bildirilsin?
    bool notify_trade_open    = true;   // pozisyon açıldı
    bool notify_trade_close   = true;   // pozisyon kapandı (tp/sl/timeout)
    bool notify_stop_loss     = true;   // stop-loss tetiklendi
    bool notify_drawdown_warn = true;   // max drawdown uyarısı
    bool notify_circuit_break = true;   // circuit breaker devreye girdi
    bool notify_paper_mode    = true;   // paper trading raporu
    bool notify_daily_report  = false;  // günlük özet (isteğe bağlı)
};

// ============================================================================
// §2  Mesaj formatları
// ============================================================================

struct TradeEvent {
    enum class Type {
        OPEN, CLOSE, STOP_LOSS, CIRCUIT_BREAK, DRAWDOWN_WARN,
        PAPER_SUMMARY, DAILY_REPORT, INFO
    };

    Type        type          = Type::INFO;
    double      price         = 0.0;
    double      qty           = 0.0;
    double      pnl           = 0.0;
    double      cum_pnl       = 0.0;
    double      drawdown_pct  = 0.0;
    bool        is_long       = true;
    std::string symbol;
    std::string reason;       // "tp", "sl", "timeout", "trailing"
    std::string extra;        // ek bilgi
};

// ============================================================================
// §3  Mesaj Builder
// ============================================================================

namespace detail {

inline std::string format_event(const TradeEvent& ev, const std::string& prefix = "") {
    std::ostringstream ss;
    const char* dir = ev.is_long ? "📈 LONG" : "📉 SHORT";

    switch (ev.type) {
        case TradeEvent::Type::OPEN:
            ss << prefix << "🟢 *GİRİŞ* " << dir << "\n"
               << "Sembol : " << ev.symbol << "\n"
               << "Fiyat  : " << std::fixed << std::setprecision(2) << ev.price << "\n"
               << "Miktar : " << std::setprecision(4) << ev.qty << "\n"
               << "Kümül PnL: " << std::setprecision(4) << ev.cum_pnl;
            break;

        case TradeEvent::Type::CLOSE:
            ss << prefix << "🔵 *ÇIKIŞ*\n"
               << "Sembol  : " << ev.symbol << "\n"
               << "Fiyat   : " << std::fixed << std::setprecision(2) << ev.price << "\n"
               << "PnL     : " << (ev.pnl >= 0 ? "+" : "") << std::setprecision(4) << ev.pnl << "\n"
               << "Neden   : " << ev.reason << "\n"
               << "Kümül PnL: " << ev.cum_pnl;
            break;

        case TradeEvent::Type::STOP_LOSS:
            ss << prefix << "🛑 *STOP-LOSS* tetiklendi!\n"
               << "Sembol  : " << ev.symbol << "\n"
               << "Fiyat   : " << std::fixed << std::setprecision(2) << ev.price << "\n"
               << "Kayıp   : " << std::setprecision(4) << ev.pnl << "\n"
               << "Kümül PnL: " << ev.cum_pnl;
            break;

        case TradeEvent::Type::CIRCUIT_BREAK:
            ss << prefix << "⚠️ *CIRCUIT BREAKER devreye girdi!*\n"
               << ev.reason << "\n"
               << "Max Drawdown: %" << std::fixed << std::setprecision(2)
               << ev.drawdown_pct << "\n"
               << "Bot durdu — manuel müdahale gerekli.";
            break;

        case TradeEvent::Type::DRAWDOWN_WARN:
            ss << prefix << "⚠️ *Drawdown Uyarısı*\n"
               << "Güncel Drawdown: %" << std::fixed << std::setprecision(2)
               << ev.drawdown_pct << "\n"
               << ev.extra;
            break;

        case TradeEvent::Type::PAPER_SUMMARY:
            ss << prefix << "📋 *Paper Trading Özeti*\n"
               << "Kümül PnL: " << std::fixed << std::setprecision(4) << ev.cum_pnl << "\n"
               << ev.extra;
            break;

        case TradeEvent::Type::DAILY_REPORT:
            ss << prefix << "📊 *Günlük Rapor*\n"
               << "Net PnL: " << std::fixed << std::setprecision(4) << ev.cum_pnl << "\n"
               << ev.extra;
            break;

        default:
            ss << prefix << "ℹ️ " << ev.extra;
            break;
    }
    return ss.str();
}

// URL encode (minimal — sadece boşluk ve özel karakterler)
inline std::string url_encode_minimal(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// JSON string escape — çift tırnak, ters eğik çizgi ve kontrol karakterlerini kaçışlar.
// JSON body'ye kullanıcı/bot verisini gömmeden önce her string alanına uygulanmalıdır.
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Kontrol karakteri — \uXXXX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace detail

// ============================================================================
// §4  HTTP Gönderici — sistem curl komutu ile (libcurl gerektirmez)
// ============================================================================

// ============================================================================
// §4  HTTP Gönderici — fork()+execvp() ile güvenli curl çağrısı
//
// std::system() KULLANILMIYOR: shell yorumlayıcı devreye girmediğinden
// shell enjeksiyonu (command injection) riski sıfırdır.
// Argümanlar doğrudan execvp() ile curl'e dizi olarak iletilir.
//
// Body içeriği geçici bir dosyaya yazılır; curl -d @file ile okunur.
// Böylece gövde içindeki tek/çift tırnak, özel karakterler veya
// boşluklar kabuk yorumlamasından tamamen korunur.
// ============================================================================

namespace detail {

// Geçici dosya RAII sarmalayıcı
struct TempFile {
    char path[64];
    int  fd = -1;

    TempFile() {
        std::strncpy(path, "/tmp/kusak_notify_XXXXXX", sizeof(path));
        fd = ::mkstemp(path);
    }
    ~TempFile() {
        if (fd >= 0) ::close(fd);
        ::unlink(path);
    }
    bool ok() const noexcept { return fd >= 0; }

    bool write_all(const std::string& data) const noexcept {
        const char* ptr = data.data();
        ssize_t remaining = static_cast<ssize_t>(data.size());
        while (remaining > 0) {
            ssize_t n = ::write(fd, ptr, static_cast<size_t>(remaining));
            if (n <= 0) return false;
            ptr += n;
            remaining -= n;
        }
        return true;
    }
};

inline bool send_http_post(const std::string& url,
                            const std::string& body,
                            const std::string& content_type = "application/json",
                            const std::string& auth_header = "",
                            bool verbose = false) noexcept
{
    // Body'yi geçici dosyaya yaz — kabuk yorumlamasından tamamen izole eder.
    TempFile tmp;
    if (!tmp.ok()) {
        if (verbose) std::cerr << "[Notify] geçici dosya oluşturulamadı.\n";
        return false;
    }
    if (!tmp.write_all(body)) {
        if (verbose) std::cerr << "[Notify] body dosyaya yazılamadı.\n";
        return false;
    }

    // Argüman listesini oluştur (kabuk yok, her argüman kendi dizisi)
    std::string ct_header  = "Content-Type: " + content_type;
    std::string at_header  = auth_header.empty() ? "" : "Authorization: " + auth_header;
    std::string data_arg   = std::string("@") + tmp.path;

    // argv: curl -s -X POST -H <ct> [-H <auth>] -d @file <url> NULL
    std::vector<const char*> argv;
    argv.reserve(12);
    argv.push_back("curl");
    argv.push_back("-s");
    argv.push_back("-X"); argv.push_back("POST");
    argv.push_back("-H"); argv.push_back(ct_header.c_str());
    if (!auth_header.empty()) {
        argv.push_back("-H"); argv.push_back(at_header.c_str());
    }
    argv.push_back("-d");  argv.push_back(data_arg.c_str());
    argv.push_back(url.c_str());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        if (verbose)
            std::cerr << "[Notify] fork() başarısız: " << std::strerror(errno) << "\n";
        return false;
    }

    if (pid == 0) {
        // Alt süreç: stdout/stderr'i /dev/null'a yönlendir
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        // execvp: curl'ü doğrudan çağır — shell yok, enjeksiyon riski yok.
        ::execvp("curl", const_cast<char* const*>(argv.data()));
        // execvp dönerse hata demektir
        ::_exit(127);
    }

    // Ana süreç: alt süreci bekle
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        if (verbose)
            std::cerr << "[Notify] waitpid() başarısız: " << std::strerror(errno) << "\n";
        return false;
    }

    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && verbose) {
        std::cerr << "[Notify] curl başarısız: " << url
                  << " exit=" << WEXITSTATUS(status) << "\n";
    }
    return ok;
}

inline bool send_telegram(const TelegramConfig& cfg,
                            const std::string& text) noexcept {
    if (!cfg.enabled || cfg.bot_token.empty() || cfg.chat_id.empty()) return false;

    std::string url = "https://api.telegram.org/bot" + cfg.bot_token + "/sendMessage";

    // JSON body
    std::string body = "{\"chat_id\":\"" + json_escape(cfg.chat_id) + "\","
                       "\"text\":\""    + json_escape(text)        + "\","
                       "\"parse_mode\":\"Markdown\"}";

    return send_http_post(url, body, "application/json", "", cfg.verbose);
}

inline bool send_slack(const SlackConfig& cfg,
                        const std::string& text) noexcept {
    if (!cfg.enabled || cfg.webhook_url.empty()) return false;

    // Slack'in JSON formatı
    std::string body = "{\"text\":\"" + json_escape(text) + "\"";
    if (!cfg.channel.empty())
        body += ",\"channel\":\"" + json_escape(cfg.channel) + "\"";
    body += "}";

    return send_http_post(cfg.webhook_url, body, "application/json", "", cfg.verbose);
}

inline bool send_webhook(const WebhookConfig& cfg,
                           const std::string& text) noexcept {
    if (!cfg.enabled || cfg.url.empty()) return false;

    std::string body = "{\"message\":\"" + json_escape(text) + "\"}";
    std::string auth = cfg.secret.empty() ? "" : "Bearer " + cfg.secret;

    return send_http_post(cfg.url, body, "application/json", auth);
}

} // namespace detail

// ============================================================================
// §5  Notifier Sınıfı — ana arayüz
// ============================================================================

class Notifier {
public:
    explicit Notifier(NotificationConfig cfg = {})
        : cfg_(std::move(cfg))
        , worker_running_(true)
        , worker_([this] { worker_loop(); })
    {}

    ~Notifier() {
        worker_running_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Kopyalama yasak (mutex & thread)
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    // ── Olay göndericiler ─────────────────────────────────────────────────

    void send_trade_open(double price, double qty, bool is_long,
                          const std::string& symbol, double cum_pnl = 0.0) {
        if (!cfg_.notify_trade_open) return;
        TradeEvent ev;
        ev.type    = TradeEvent::Type::OPEN;
        ev.price   = price;
        ev.qty     = qty;
        ev.is_long = is_long;
        ev.symbol  = symbol;
        ev.cum_pnl = cum_pnl;
        enqueue(ev);
    }

    void send_trade_close(double exit_price, double pnl, const std::string& reason,
                           const std::string& symbol, double cum_pnl = 0.0) {
        if (!cfg_.notify_trade_close) return;
        TradeEvent ev;
        ev.type    = TradeEvent::Type::CLOSE;
        ev.price   = exit_price;
        ev.pnl     = pnl;
        ev.reason  = reason;
        ev.symbol  = symbol;
        ev.cum_pnl = cum_pnl;
        // stop-loss ise ayrı olay tipi
        if ((reason == "sl" || reason == "stop") && cfg_.notify_stop_loss)
            ev.type = TradeEvent::Type::STOP_LOSS;
        enqueue(ev);
    }

    void send_circuit_breaker(const std::string& reason, double drawdown_pct) {
        if (!cfg_.notify_circuit_break) return;
        TradeEvent ev;
        ev.type         = TradeEvent::Type::CIRCUIT_BREAK;
        ev.reason       = reason;
        ev.drawdown_pct = drawdown_pct;
        enqueue(ev);
    }

    void send_drawdown_warning(double drawdown_pct, const std::string& extra = "") {
        if (!cfg_.notify_drawdown_warn) return;
        TradeEvent ev;
        ev.type         = TradeEvent::Type::DRAWDOWN_WARN;
        ev.drawdown_pct = drawdown_pct;
        ev.extra        = extra;
        enqueue(ev);
    }

    void send_paper_summary(double cum_pnl, const std::string& extra = "") {
        if (!cfg_.notify_paper_mode) return;
        TradeEvent ev;
        ev.type    = TradeEvent::Type::PAPER_SUMMARY;
        ev.cum_pnl = cum_pnl;
        ev.extra   = extra;
        enqueue(ev);
    }

    void send_info(const std::string& msg) {
        TradeEvent ev;
        ev.type  = TradeEvent::Type::INFO;
        ev.extra = msg;
        enqueue(ev);
    }

    // Kaç bildirim gönderildi?
    uint64_t sent_count() const noexcept {
        return sent_count_.load(std::memory_order_relaxed);
    }

    // Aktif mi?
    bool any_enabled() const noexcept {
        return cfg_.telegram.enabled || cfg_.slack.enabled || cfg_.webhook.enabled;
    }

    const NotificationConfig& config() const noexcept { return cfg_; }

private:
    void enqueue(const TradeEvent& ev) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(ev);
        }
        cv_.notify_one();
    }

    void worker_loop() {
        while (worker_running_.load(std::memory_order_acquire)) {
            TradeEvent ev;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] {
                    return !queue_.empty() ||
                           !worker_running_.load(std::memory_order_relaxed);
                });
                if (queue_.empty()) continue;
                ev = queue_.front();
                queue_.pop();
            }
            dispatch(ev);
        }
        // Kalan olayları gönder (temiz kapanış)
        std::lock_guard<std::mutex> lk(mtx_);
        while (!queue_.empty()) {
            dispatch(queue_.front());
            queue_.pop();
        }
    }

    void dispatch(const TradeEvent& ev) {
        std::string text = detail::format_event(ev);

        bool ok = false;
        if (cfg_.telegram.enabled)
            ok |= detail::send_telegram(cfg_.telegram, text);
        if (cfg_.slack.enabled)
            ok |= detail::send_slack(cfg_.slack, text);
        if (cfg_.webhook.enabled)
            ok |= detail::send_webhook(cfg_.webhook, text);

        if (ok || any_enabled())
            sent_count_.fetch_add(1, std::memory_order_relaxed);
    }

    NotificationConfig          cfg_;
    std::mutex                  mtx_;
    std::queue<TradeEvent>      queue_;
    std::condition_variable     cv_;
    std::atomic<bool>           worker_running_;
    std::atomic<uint64_t>       sent_count_{0};
    std::thread                 worker_;
};

// ============================================================================
// §6  .env'den NotificationConfig yükle
//     API_KEY, API_SECRET ile aynı .env dosyasına:
//       TELEGRAM_BOT_TOKEN=...
//       TELEGRAM_CHAT_ID=...
//       SLACK_WEBHOOK=...
// ============================================================================

inline NotificationConfig load_notification_config_from_env(
    const std::string& env_path) noexcept
{
    NotificationConfig cfg;

    std::FILE* f = std::fopen(env_path.c_str(), "r");
    if (!f) return cfg;

    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        // Trim newline
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        // Remove quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        if (key == "TELEGRAM_BOT_TOKEN") {
            cfg.telegram.bot_token = val;
            cfg.telegram.enabled   = !val.empty();
        } else if (key == "TELEGRAM_CHAT_ID") {
            cfg.telegram.chat_id = val;
        } else if (key == "SLACK_WEBHOOK") {
            cfg.slack.webhook_url = val;
            cfg.slack.enabled     = !val.empty();
        } else if (key == "SLACK_CHANNEL") {
            cfg.slack.channel = val;
        } else if (key == "NOTIFY_WEBHOOK") {
            cfg.webhook.url    = val;
            cfg.webhook.enabled= !val.empty();
        }
    }
    std::fclose(f);
    return cfg;
}

} // namespace kusak::notify
