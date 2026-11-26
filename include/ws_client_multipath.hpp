// ws_client_multipath.hpp — KUSAK V17 Epoll Reactor / Lock-Free / Zero-Allocation
// ──────────────────────────────────────────────────────────────────────────────
// AUDIT PATCH LOG (V16 → V17):
//   [1.1] discard_ssl_bytes():  CPU spin on SSL_WANT_* replaced with poll(100ms)
//   [1.2] epoll_wait 1ms timeout: replaced with 0 timeout + eventfd wake
//   [1.3] do_ws_handshake():   byte-by-byte SSL_read replaced with buffered bulk read
//   [1.4] make_binance_endpoints: strncpy → snprintf (null-termination guarantee)
//   [1.5] handle_disconnect():  epoch set to UINT64_MAX on deregister (zombie guard)
//   [2.1] reconnect_worker_loop: poll() → ppoll() with stop_ check, clean shutdown
//   [2.2] pending_fd_ / conn_state_ ordering: pending_fd_ stored BEFORE conn_state_
//   [2.3] active_idx_ failover: scan all conns for first CONNECTED before switching
//   [3.0] OnBookTicker/OnAggTrade: std::function → raw fn ptr + void* ctx
//   [4.1] make_binance_endpoints: std::string concat → snprintf into char[]
//   [4.2] do_ws_handshake(): std::string req/key/expected → stack char arrays
//   [4.3] b64() / ws_accept(): return std::string → write into caller-supplied char[]
//   [5.1] Per-index atomics: all wrapped in alignas(64) padded structs (false-share fix)
//   [5.2] WsConnection::generation_id_: isolated to its own cache line
// ──────────────────────────────────────────────────────────────────────────────
#pragma once
#include <atomic>
#include <array>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>  // [1.2] eventfd for zero-latency epoll wake
#include <poll.h>
#include <signal.h>       // [2.1] ppoll needs sigset_t
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include "ws_client.hpp"  // canonical AggTrade / BookTicker definitions

namespace kusak::ws {

// ── [3.0] Zero-overhead callbacks: raw function pointers + void* ctx ──────────
// Replaces std::function<> — eliminates vtable dispatch, enables inlining,
// guarantees zero heap allocation on every invocation.
// ─────────────────────────────────────────────────────────────────────────────
using OnBookTicker = void (*)(const BookTicker&, void* ctx) noexcept;
using OnAggTrade   = void (*)(const AggTrade&,   void* ctx) noexcept;

struct WsEndpoint { char host[64]; char port[8]; char path[256]; };

// Connection states for lock-free state machine
enum class ConnState : int { DISCONNECTED=0, CONNECTING=1, CONNECTED=2 };

// ── [5.1] Cache-line padded atomic wrappers — eliminates false sharing ─────────
// Each per-connection atomic lives on its own 64-byte cache line so writes
// from one core (e.g. reconnect worker for conn 0) cannot invalidate the
// L1 line of another core monitoring conn 1.
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(64) PaddedAtomicBool {
    std::atomic<bool> val{false};
    char _pad[64 - sizeof(std::atomic<bool>)];
};
struct alignas(64) PaddedAtomicInt {
    std::atomic<int> val{-1};
    char _pad[64 - sizeof(std::atomic<int>)];
};
struct alignas(64) PaddedAtomicConnState {
    std::atomic<int> val{static_cast<int>(ConnState::DISCONNECTED)};
    char _pad[64 - sizeof(std::atomic<int>)];
};
struct alignas(64) PaddedAtomicU64 {
    std::atomic<uint64_t> val{0};
    char _pad[64 - sizeof(std::atomic<uint64_t>)];
};
struct alignas(64) PaddedAtomicSession {
    std::atomic<SSL_SESSION*> val{nullptr};
    char _pad[64 - sizeof(std::atomic<SSL_SESSION*>)];
};
// Non-atomic epoch — read/written only on the reactor thread; padded for safety.
struct alignas(64) PaddedEpoch {
    uint64_t val{0};
    char _pad[64 - sizeof(uint64_t)];
};

// ── SPSC Ring Buffer — cache-line isolated, lock-free ─────────────────────────
struct alignas(64) RingSlot {
    uint16_t len;
    char payload[4094]; // max WS frame
};

template<uint32_t N=4096>
class SpscFrameRing {
    static_assert((N&(N-1))==0,"N must be power of 2");
    static constexpr uint32_t MASK=N-1;
    alignas(64) std::atomic<uint32_t> head_{0};
    alignas(64) std::atomic<uint32_t> tail_{0};
    alignas(64) std::atomic<uint64_t> drops_{0};
    alignas(64) RingSlot buf_[N];
public:
    RingSlot* try_claim() noexcept {
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        return &buf_[h];
    }
    void publish() noexcept {
        uint32_t h = head_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        head_.store((h + 1) & MASK, std::memory_order_relaxed);
    }
    const RingSlot* peek() const noexcept {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            return nullptr;
        }
        return &buf_[t];
    }
    void consume() noexcept {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        tail_.store((t + 1) & MASK, std::memory_order_release);
    }
    uint64_t dropped() const noexcept { return drops_.load(std::memory_order_relaxed); }
};

// ── HFT: 64-bit vectorized WebSocket unmask ───────────────────────────────────
inline void unmask_payload_64(char* data, int len, const uint8_t* mask_key) noexcept {
    uint64_t mask64;
    std::memcpy(&mask64, mask_key, 4);
    mask64 = mask64 | (mask64 << 32);
    int i = 0;
    const int block_end = len & ~7;
    for (; i < block_end; i += 8) {
        uint64_t chunk;
        std::memcpy(&chunk, data + i, 8);
        chunk ^= mask64;
        std::memcpy(data + i, &chunk, 8);
    }
    for (; i < len; ++i) {
        data[i] ^= mask_key[i & 3];
    }
}

// ── WsConnection — RAII SSL with non-blocking handshake ───────────────────────
class WsConnection {
    static constexpr int POLL_TIMEOUT_MS     = 3000; // hard 3s ceiling for I/O phases
    static constexpr int DISCARD_POLL_MS     = 100;  // [1.1] bounded poll in discard loop
    static constexpr int HANDSHAKE_BUF_SZ   = 4096;
    static constexpr int WS_KEY_B64_SZ      = 25;   // 24 chars + NUL
    static constexpr int WS_ACCEPT_B64_SZ   = 29;   // 28 chars + NUL
    static constexpr int WS_REQ_BUF_SZ      = 1024; // enough for GET request

public:
    WsConnection()=default;
    ~WsConnection() { close(); }
    WsConnection(const WsConnection&)=delete;
    WsConnection& operator=(const WsConnection&)=delete;
    WsConnection(WsConnection&&)=delete;
    WsConnection& operator=(WsConnection&&)=delete;

    // [2.1] open() now accepts a stop flag pointer so ppoll loops can bail early.
    bool open(const WsEndpoint& ep, SSL_CTX* ctx,
              PaddedAtomicSession& session_atom,
              PaddedAtomicU64& session_fails,
              const std::atomic<bool>* stop_flag = nullptr) noexcept {
        close();
        generation_id_.fetch_add(1, std::memory_order_release);

        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        // fprintf(stderr, "[KUSAK_DIAG] DNS: resolving %s:%s\n", ep.host, ep.port);
        if (::getaddrinfo(ep.host, ep.port, &hints, &res) != 0) {
            // fprintf(stderr, "[KUSAK_DIAG] DNS: FAILED for %s:%s\n", ep.host, ep.port);
            return false;
        }
        // fprintf(stderr, "[KUSAK_DIAG] DNS: OK for %s:%s\n", ep.host, ep.port);

        fd_ = ::socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) { ::freeaddrinfo(res); return false; }

        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        {
            int flags = ::fcntl(fd_, F_GETFL, 0);
            if (flags >= 0) ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }

        int cr = ::connect(fd_, res->ai_addr, res->ai_addrlen);
        if (cr != 0 && errno != EINPROGRESS) {
            ::freeaddrinfo(res); close(); return false;
        }
        ::freeaddrinfo(res);

        if (cr != 0) {
            // [2.1] Use ppoll so we can abort if stop_ fires during connect wait
            if (!poll_with_stop(fd_, POLLOUT, POLL_TIMEOUT_MS, stop_flag)) {
                // fprintf(stderr, "[KUSAK_DIAG] CONNECT: poll timeout/stop for %s\n", ep.host);
                close(); return false;
            }
            int serr = 0; socklen_t slen = sizeof(serr);
            ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &serr, &slen);
            if (serr != 0) {
                // fprintf(stderr, "[KUSAK_DIAG] CONNECT: SO_ERROR=%d for %s\n", serr, ep.host);
                close(); return false;
            }
        }
        // fprintf(stderr, "[KUSAK_DIAG] CONNECT: TCP connected to %s\n", ep.host);

        if (!ctx) { close(); return false; }
        ssl_ = SSL_new(ctx);
        if (!ssl_) { close(); return false; }
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, ep.host);

        SSL_SESSION* cached = session_atom.val.load(std::memory_order_acquire);
        bool session_offered = (cached != nullptr);
        if (cached) SSL_set_session(ssl_, cached);

        // fprintf(stderr, "[KUSAK_DIAG] SSL: attempting async SSL_connect to %s\n", ep.host);
        while (true) {
            // [2.1] abort if stop_ fires inside SSL handshake
            if (stop_flag && stop_flag->load(std::memory_order_acquire)) {
                close(); return false;
            }
            int sr = SSL_connect(ssl_);
            if (sr == 1) break;
            int serr2 = SSL_get_error(ssl_, sr);
            if (serr2 == SSL_ERROR_WANT_READ || serr2 == SSL_ERROR_WANT_WRITE) {
                short poll_ev = (serr2 == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                if (!poll_with_stop(fd_, poll_ev, POLL_TIMEOUT_MS, stop_flag)) {
                    // fprintf(stderr, "[KUSAK_DIAG] SSL: poll timeout/stop during SSL_connect to %s\n", ep.host);
                    close(); return false;
                }
                continue;
            }
            // fprintf(stderr, "[KUSAK_DIAG] SSL: SSL_connect FAILED (err=%d) to %s\n", serr2, ep.host);
            close(); return false;
        }
        // fprintf(stderr, "[KUSAK_DIAG] SSL: connected OK to %s\n", ep.host);

        if (session_offered && !SSL_session_reused(ssl_)) {
            session_fails.val.fetch_add(1, std::memory_order_relaxed);
            SSL_SESSION* old = session_atom.val.exchange(nullptr, std::memory_order_acq_rel);
            if (old) SSL_SESSION_free(old);
        }

        if (!do_ws_handshake(ep.host, ep.path, stop_flag)) {
            close(); return false;
        }

        SSL_SESSION* s = SSL_get1_session(ssl_);
        if (s) {
            SSL_SESSION* old = session_atom.val.exchange(s, std::memory_order_release);
            if (old) SSL_SESSION_free(old);
        }
        // fprintf(stderr, "[KUSAK_DIAG] WS: handshake complete, connection OPEN fd=%d\n", fd_);
        connected_ = true;
        return true;
    }

    bool set_nonblock() noexcept {
        if (fd_ < 0) return false;
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0) return false;
        return ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void close() noexcept {
        connected_ = false;
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        reset_reassembly();
    }

    int ssl_read_append() noexcept {
        if (!connected_ || write_off_ >= sizeof(frag_buf_)) return -1;
        int cap = static_cast<int>(sizeof(frag_buf_) - write_off_);
        int r = SSL_read(ssl_, frag_buf_.data() + write_off_, cap);
        if (r > 0) {
            // fprintf(stderr, "[KUSAK_DIAG] SSL_read: %d bytes read\n", r);
            write_off_ += static_cast<size_t>(r);
            return r;
        }
        int err = SSL_get_error(ssl_, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            // fprintf(stderr, "[KUSAK_DIAG] SSL_read: WANT_READ/WRITE (err=%d)\n", err);
            return 0;
        }
        // fprintf(stderr, "[KUSAK_DIAG] SSL_read: ERROR err=%d, closing\n", err);
        connected_ = false;
        return -1;
    }

    // [1.1] FIXED: discard_ssl_bytes() — poll(100ms) on SSL_WANT_* instead of spin.
    // Previously: unconditional continue → CPU spin on NB socket until data arrives.
    // Now:        bounded poll(DISCARD_POLL_MS) per WANT_* → yields CPU, prevents hang.
    bool discard_ssl_bytes(size_t bytes_to_skip) noexcept {
        char trash[4096];
        while (bytes_to_skip > 0) {
            int chunk = static_cast<int>(
                bytes_to_skip < sizeof(trash) ? bytes_to_skip : sizeof(trash));
            int r = SSL_read(ssl_, trash, chunk);
            if (r > 0) {
                bytes_to_skip -= static_cast<size_t>(r);
                continue;
            }
            int err = SSL_get_error(ssl_, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // [1.1] poll instead of spinning — bounded 100ms timeout
                short poll_ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                struct pollfd pfd{.fd = fd_, .events = poll_ev, .revents = 0};
                int pr = ::poll(&pfd, 1, DISCARD_POLL_MS);
                if (pr < 0 && errno == EINTR) continue; // signal — retry
                if (pr <= 0) {
                    // Timeout: network is too slow or malicious; abort discard.
                    // fprintf(stderr, "[KUSAK_DIAG] DISCARD: poll timeout, aborting discard\n");
                    connected_ = false;
                    return false;
                }
                continue; // ready — retry SSL_read
            }
            connected_ = false;
            return false; // real SSL error
        }
        return true;
    }

    void reset_reassembly() noexcept { write_off_ = 0; parse_cur_ = 0; }

    const char* parse_ptr() const noexcept { return frag_buf_.data() + parse_cur_; }
    size_t available() const noexcept { return write_off_ - parse_cur_; }
    void advance_cursor(size_t n) noexcept { parse_cur_ += n; }
    static constexpr size_t buf_capacity() noexcept { return 65536; }

    void compact() noexcept {
        if (parse_cur_ == 0) return;
        const size_t rem = write_off_ - parse_cur_;
        if (rem > 0) std::memmove(frag_buf_.data(), frag_buf_.data() + parse_cur_, rem);
        write_off_ = rem;
        parse_cur_ = 0;
    }

    bool send_text(std::string_view msg) noexcept {
        if (!connected_) return false;
        const size_t n = msg.size();
        uint8_t hdr[10]; int hlen = 0;
        hdr[hlen++] = 0x81;
        if (n < 126) {
            hdr[hlen++] = uint8_t(0x80 | n);
        } else {
            hdr[hlen++] = 0x80 | 126;
            hdr[hlen++] = uint8_t(n >> 8);
            hdr[hlen++] = uint8_t(n & 0xFF);
        }
        hdr[hlen++]=0; hdr[hlen++]=0; hdr[hlen++]=0; hdr[hlen++]=0;
        if (SSL_write(ssl_, hdr, hlen) != hlen) return false;
        return SSL_write(ssl_, msg.data(), static_cast<int>(n)) == static_cast<int>(n);
    }

    bool is_connected() const noexcept { return connected_; }
    int  fd()           const noexcept { return fd_; }

    // [5.2] generation_id_ on its own cache line — isolated from frag_buf_ hot data.
    // Read by reactor core, written by reconnect worker → potential false sharing fixed.
    alignas(64) std::atomic<uint64_t> generation_id_{0};
    uint64_t generation() const noexcept {
        return generation_id_.load(std::memory_order_acquire);
    }

private:
    // ── [4.3] b64: zero-allocation — writes into caller-supplied buffer ──────────
    // Returns number of bytes written (excl. NUL), or 0 on size error.
    static int b64_into(const uint8_t* d, size_t n, char* out, size_t out_sz) noexcept {
        static constexpr char T[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const size_t needed = ((n + 2) / 3) * 4 + 1; // +1 for NUL
        if (out_sz < needed) return 0;
        int pos = 0;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = uint32_t(d[i]) << 16;
            if (i + 1 < n) v |= uint32_t(d[i + 1]) << 8;
            if (i + 2 < n) v |= d[i + 2];
            out[pos++] = T[(v >> 18) & 63];
            out[pos++] = T[(v >> 12) & 63];
            out[pos++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
            out[pos++] = (i + 2 < n) ? T[v & 63]        : '=';
        }
        out[pos] = '\0';
        return pos;
    }

    // ── [4.3] ws_accept: zero-allocation — writes accept token into out[] ────────
    // key_len must be the result of b64_into (no NUL counted).
    static void ws_accept_into(const char* key, int key_len,
                               char* out, size_t out_sz) noexcept {
        static constexpr char G[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        static constexpr int  GL  = sizeof(G) - 1;
        // Stack-build concatenation: key || GUID
        char cat[64 + GL + 1]; // WS key is 24 chars max → 24+36+1 = 61 ≤ 101
        std::memcpy(cat, key, static_cast<size_t>(key_len));
        std::memcpy(cat + key_len, G, static_cast<size_t>(GL));
        const int cat_len = key_len + GL;
        uint8_t dg[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const uint8_t*>(cat), static_cast<size_t>(cat_len), dg);
        b64_into(dg, SHA_DIGEST_LENGTH, out, out_sz);
    }

    // ── [2.1] Shared helper: ppoll with stop-flag check ─────────────────────────
    // Replaces all bare poll() calls inside connection logic so stop_ can interrupt
    // any I/O wait phase, enabling clean sub-100ms destructor join.
    static bool poll_with_stop(int fd, short events, int timeout_ms,
                               const std::atomic<bool>* stop_flag) noexcept {
        // Use ppoll with a short tick so we can re-check stop_ periodically.
        struct pollfd pfd{.fd = fd, .events = events, .revents = 0};
        const int TICK_MS = 100; // check stop every 100ms
        int remaining = timeout_ms;
        while (remaining > 0) {
            if (stop_flag && stop_flag->load(std::memory_order_acquire)) return false;
            const int wait = remaining < TICK_MS ? remaining : TICK_MS;
            struct timespec ts{ .tv_sec  = wait / 1000,
                                .tv_nsec = (wait % 1000) * 1'000'000L };
            int pr = ::ppoll(&pfd, 1, &ts, nullptr);
            if (pr > 0 && (pfd.revents & events)) return true;
            if (pr < 0 && errno == EINTR) continue;
            if (pr < 0) return false;
            remaining -= wait;
        }
        return false;
    }

    // ── [1.3] do_ws_handshake(): bulk buffered read, zero heap ───────────────────
    // Old: SSL_read(..., 1) in a loop — O(n) syscalls + poll overhead per byte.
    // New: SSL_read into a 4096-byte stack buf; scan for \r\n\r\n in memory.
    // [4.2] All string temporaries (key, expected, req, srv) are stack char arrays.
    bool do_ws_handshake(const char* host, const char* path,
                         const std::atomic<bool>* stop_flag) noexcept {
        // ── Random nonce ─────────────────────────────────────────────────────
        uint8_t raw[16];
        int rfd = ::open("/dev/urandom", O_RDONLY);
        if (rfd < 0 || ::read(rfd, raw, 16) != 16) {
            if (rfd >= 0) ::close(rfd);
            return false;
        }
        ::close(rfd);

        // [4.2] key and expected on stack — no heap
        char key[WS_KEY_B64_SZ];       // 25 bytes
        char expected[WS_ACCEPT_B64_SZ]; // 29 bytes
        const int key_len = b64_into(raw, 16, key, sizeof(key));
        if (key_len <= 0) return false;
        ws_accept_into(key, key_len, expected, sizeof(expected));

        // [4.2] Build HTTP request entirely on stack with snprintf
        char req[WS_REQ_BUF_SZ];
        const int req_len = ::snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            path, host, key);
        if (req_len <= 0 || req_len >= static_cast<int>(sizeof(req))) return false;

        // ── Async SSL_write ───────────────────────────────────────────────────
        int written = 0;
        while (written < req_len) {
            if (stop_flag && stop_flag->load(std::memory_order_acquire)) return false;
            int w = SSL_write(ssl_, req + written, req_len - written);
            if (w > 0) { written += w; continue; }
            int werr = SSL_get_error(ssl_, w);
            if (werr == SSL_ERROR_WANT_WRITE || werr == SSL_ERROR_WANT_READ) {
                short poll_ev = (werr == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                if (!poll_with_stop(fd_, poll_ev, POLL_TIMEOUT_MS, stop_flag))
                    return false;
                continue;
            }
            return false;
        }

        // ── [1.3] Async SSL_read — bulk buffered, scan for \r\n\r\n ───────────
        // Old code read one byte at a time with poll() per byte — hundreds of
        // syscalls. New: read up to 4096 bytes per SSL_read, scan in memory.
        char resp[HANDSHAKE_BUF_SZ];
        int rlen = 0;
        bool header_done = false;
        while (!header_done && rlen < static_cast<int>(sizeof(resp)) - 1) {
            if (stop_flag && stop_flag->load(std::memory_order_acquire)) return false;
            int n = SSL_read(ssl_, resp + rlen,
                             static_cast<int>(sizeof(resp)) - rlen - 1);
            if (n > 0) {
                rlen += n;
                // Scan for \r\n\r\n in newly accumulated data
                const int scan_from = rlen - n - 3 > 0 ? rlen - n - 3 : 0;
                for (int i = scan_from; i <= rlen - 4; ++i) {
                    if (resp[i]=='\r' && resp[i+1]=='\n' &&
                        resp[i+2]=='\r' && resp[i+3]=='\n') {
                        rlen = i + 4; // trim to end of headers
                        header_done = true;
                        break;
                    }
                }
                continue;
            }
            int rerr = SSL_get_error(ssl_, n);
            if (rerr == SSL_ERROR_WANT_READ || rerr == SSL_ERROR_WANT_WRITE) {
                short poll_ev = (rerr == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                if (!poll_with_stop(fd_, poll_ev, POLL_TIMEOUT_MS, stop_flag))
                    return false;
                continue;
            }
            return false;
        }
        if (!header_done) return false;

        resp[rlen] = '\0';
        std::string_view rv(resp, static_cast<size_t>(rlen));
        if (rv.find("101") == std::string_view::npos) return false;

        // Extract Sec-WebSocket-Accept value — pointer arithmetic, no alloc
        static constexpr std::string_view HDR_SV = "Sec-WebSocket-Accept: ";
        auto p2 = rv.find(HDR_SV);
        if (p2 == std::string_view::npos) return false;
        auto vs = p2 + HDR_SV.size();
        auto ve = rv.find("\r\n", vs);
        if (ve == std::string_view::npos) return false;

        // [4.2] Copy server accept token to stack buffer — no std::string
        char srv[WS_ACCEPT_B64_SZ];
        const size_t srv_len = ve - vs;
        if (srv_len == 0 || srv_len >= sizeof(srv)) return false;
        std::memcpy(srv, rv.data() + vs, srv_len);
        srv[srv_len] = '\0';

        // Constant-time comparison
        const size_t exp_len = std::strlen(expected);
        if (srv_len != exp_len) return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < exp_len; ++i) {
            diff |= uint8_t(srv[i]) ^ uint8_t(expected[i]);
        }
        return diff == 0;
    }

    int  fd_        = -1;
    SSL* ssl_       = nullptr;
    bool connected_ = false;

    // [5.2] generation_id_ declared above (public, alignas(64)), before frag_buf_
    // so it sits on its own cache line and cannot be evicted by frag_buf_ writes.
    // (declaration moved to public section above; repeated here as comment only)

    alignas(64) std::array<char, 65536> frag_buf_{};
    size_t write_off_ = 0;
    size_t parse_cur_ = 0;
};

// ── Zero-alloc JSON scanner (std::from_chars for numbers) ─────────────────────
namespace detail {

inline const char* skip_ws(const char* p, const char* e) noexcept {
    while (p < e && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p;
    return p;
}

inline const char* skip_value(const char* p, const char* e) noexcept {
    p = skip_ws(p, e);
    if (p >= e) return e;
    if (*p == '"') {
        ++p;
        while (p < e) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"')  return p + 1;
            ++p;
        }
        return e;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, cl = (open == '{') ? '}' : ']';
        int d = 1; ++p;
        while (p < e && d > 0) {
            if (*p == '"') {
                ++p;
                while (p < e) {
                    if (*p == '\\') { p += 2; continue; }
                    if (*p == '"')  { ++p; break; }
                    ++p;
                }
                continue;
            }
            if (*p == open) ++d;
            else if (*p == cl) --d;
            ++p;
        }
        return p;
    }
    while (p < e && *p!=',' && *p!='}' && *p!=']' &&
           *p!=' ' && *p!='\n' && *p!='\r' && *p!='\t') ++p;
    return p;
}

struct ScanResult { std::string_view value; bool is_string; };

inline bool scan_key(const char*& p, const char* e, std::string_view& key) noexcept {
    p = skip_ws(p, e);
    if (p >= e || *p != '"') return false;
    ++p;
    const char* ks = p;
    while (p < e && *p != '"') ++p;
    if (p >= e) return false;
    key = std::string_view(ks, static_cast<size_t>(p - ks));
    ++p;
    p = skip_ws(p, e);
    if (p >= e || *p != ':') return false;
    ++p;
    return true;
}

inline bool scan_value(const char*& p, const char* e, ScanResult& out) noexcept {
    p = skip_ws(p, e);
    if (p >= e) return false;
    if (*p == '"') {
        ++p;
        const char* vs = p;
        while (p < e) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') {
                out = {std::string_view(vs, static_cast<size_t>(p - vs)), true};
                ++p; return true;
            }
            ++p;
        }
        return false;
    }
    const char* vs = p;
    p = skip_value(p, e);
    out = {std::string_view(vs, static_cast<size_t>(p - vs)), false};
    return true;
}

inline bool parse_double_sv(std::string_view sv, double& out) noexcept {
    if (sv.empty()) return false;
    auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return r.ec == std::errc{};
}

inline bool parse_u64_sv(std::string_view sv, uint64_t& out) noexcept {
    if (sv.empty()) return false;
    auto r = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return r.ec == std::errc{};
}

inline bool parse_bool_sv(std::string_view sv) noexcept {
    return sv.size() == 4 && sv[0] == 't';
}

inline const char* find_event_object(const char* p, const char* e) noexcept {
    p = skip_ws(p, e);
    if (p >= e || *p != '{') return nullptr;
    ++p;
    bool has_stream = false, has_data = false;
    const char* data_val = nullptr;
    const char* scan = p;
    while (scan < e) {
        std::string_view key;
        scan = skip_ws(scan, e);
        if (scan >= e || *scan == '}') break;
        if (*scan == ',') { ++scan; continue; }
        if (!scan_key(scan, e, key)) break;
        if (key == "stream") {
            has_stream = true;
            scan = skip_value(scan, e);
        } else if (key == "data") {
            has_data = true;
            data_val = skip_ws(scan, e);
            scan = skip_value(scan, e);
        } else {
            scan = skip_value(scan, e);
        }
        if (scan < e && *scan == ',') ++scan;
    }
    if (has_stream && has_data && data_val) return data_val;
    return p - 1;
}

} // namespace detail

// ── MultipathWsClient<N> — V17 Epoll Reactor / Lock-Free / Pre-allocated Workers
template<size_t N=2>
class MultipathWsClient {
    static_assert(N>=2);
public:
    struct Config {
        std::array<WsEndpoint, N> endpoints;
        bool verbose = false;
    };

    // [3.0] Constructor: raw function pointers + void* context; no std::function
    MultipathWsClient(const Config& c,
                      OnBookTicker ticker_fn, void* ticker_ctx,
                      OnAggTrade   trade_fn,  void* trade_ctx)
        : cfg_(c)
        , on_ticker_(ticker_fn), ticker_ctx_(ticker_ctx)
        , on_trade_ (trade_fn),  trade_ctx_ (trade_ctx)
    {
        global_ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (global_ssl_ctx_) {
            SSL_CTX_set_verify(global_ssl_ctx_, SSL_VERIFY_NONE, nullptr);
            SSL_CTX_set_session_cache_mode(global_ssl_ctx_, SSL_SESS_CACHE_CLIENT);
        }
        for (size_t i = 0; i < N; ++i) {
            saved_sessions_[i].val.store(nullptr,  std::memory_order_relaxed);
            reconnect_wake_[i].val.store(false,    std::memory_order_relaxed);
            conn_state_[i].val.store(
                static_cast<int>(ConnState::DISCONNECTED), std::memory_order_relaxed);
            pending_fd_[i].val.store(-1,           std::memory_order_relaxed);
            registered_fd_[i].val.store(-1,        std::memory_order_relaxed);
            session_fails_[i].val.store(0,         std::memory_order_relaxed);
            conn_epoch_[i].val = 0;
        }

        // [1.2] Create eventfd — reconnect worker writes to it to wake epoll instantly
        notify_efd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        for (size_t i = 0; i < N; ++i) {
            reconnect_workers_[i] = std::thread([this, i]() {
                reconnect_worker_loop(static_cast<int>(i));
            });
        }
    }

    ~MultipathWsClient() {
        stop_.store(true, std::memory_order_release);
        // Wake workers via both flag and eventfd
        for (size_t i = 0; i < N; ++i)
            reconnect_wake_[i].val.store(true, std::memory_order_release);
        // Drain eventfd so reactor exits cleanly
        if (notify_efd_ >= 0) {
            uint64_t val = 1;
            ::write(notify_efd_, &val, sizeof(val));
        }
        for (size_t i = 0; i < N; ++i) {
            if (reconnect_workers_[i].joinable())
                reconnect_workers_[i].join();
            conns_[i].close();
        }
        for (size_t i = 0; i < N; ++i) {
            SSL_SESSION* s = saved_sessions_[i].val.load(std::memory_order_acquire);
            if (s) SSL_SESSION_free(s);
        }
        if (epfd_       >= 0) { ::close(epfd_);       epfd_       = -1; }
        if (notify_efd_ >= 0) { ::close(notify_efd_); notify_efd_ = -1; }
        if (global_ssl_ctx_) { SSL_CTX_free(global_ssl_ctx_); global_ssl_ctx_ = nullptr; }
    }

    void poll_callbacks() noexcept {
        for (int i = 0; i < 256; ++i) {
            const RingSlot* slot = ring_.peek();
            if (!slot) return;
            dispatch_raw(slot->payload, slot->len);
            ring_.consume();
        }
    }

    void run(const std::atomic<bool>& g_stop) {
        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd_ < 0) return;

        // [1.2] Register eventfd into epoll — reactor wakes instantly on reconnect
        if (notify_efd_ >= 0) {
            struct epoll_event eev{};
            eev.events  = EPOLLIN;
            eev.data.fd = NOTIFY_EFD_TOKEN; // sentinel: not a real conn index
            ::epoll_ctl(epfd_, EPOLL_CTL_ADD, notify_efd_, &eev);
        }

        // Initial connections
        for (size_t i = 0; i < N; ++i) {
            conn_state_[i].val.store(static_cast<int>(ConnState::CONNECTING),
                                     std::memory_order_release);
            if (conns_[i].open(cfg_.endpoints[i], global_ssl_ctx_,
                               saved_sessions_[i], session_fails_[i], &stop_)) {
                conns_[i].set_nonblock();
                epoll_register(conns_[i].fd(), static_cast<int>(i));
                registered_fd_[i].val.store(conns_[i].fd(), std::memory_order_release);
                // [2.2] Store pending_fd BEFORE conn_state so reactor never sees
                //       CONNECTED with stale fd. (No pending_fd needed here since
                //       we register directly, but ensure ordering holds.)
                conn_state_[i].val.store(static_cast<int>(ConnState::CONNECTED),
                                          std::memory_order_release);
                conn_epoch_[i].val = conns_[i].generation();
            } else {
                conn_state_[i].val.store(static_cast<int>(ConnState::DISCONNECTED),
                                          std::memory_order_release);
                registered_fd_[i].val.store(-1, std::memory_order_release);
            }
        }

        struct epoll_event events[8];

        while (!g_stop.load(std::memory_order_acquire) &&
               !stop_.load(std::memory_order_acquire))
        {
            // Check for pending fd swaps from reconnect threads
            for (size_t i = 0; i < N; ++i) {
                // [2.2] Load pending_fd first (was stored first by worker)
                int pfd = pending_fd_[i].val.load(std::memory_order_acquire);
                if (pfd >= 0) {
                    epoll_register(pfd, static_cast<int>(i));
                    registered_fd_[i].val.store(pfd, std::memory_order_release);
                    pending_fd_[i].val.store(-1, std::memory_order_release);
                    conn_epoch_[i].val = conns_[i].generation();
                }
            }

            // [2.3] Active-index failover: scan all conns for first CONNECTED
            {
                const int idx = active_idx_.load(std::memory_order_acquire);
                auto st = static_cast<ConnState>(
                    conn_state_[idx].val.load(std::memory_order_acquire));
                if (st != ConnState::CONNECTED) {
                    // Find first CONNECTED peer before committing switch
                    int next = -1;
                    for (size_t j = 0; j < N; ++j) {
                        int jj = static_cast<int>((idx + 1 + j) % N);
                        if (static_cast<ConnState>(
                                conn_state_[jj].val.load(std::memory_order_acquire))
                            == ConnState::CONNECTED) {
                            next = jj;
                            break;
                        }
                    }
                    if (next >= 0) {
                        active_idx_.store(next, std::memory_order_release);
                    }
                    // Trigger reconnect on the broken index regardless
                    reconnect_async(idx);
                }
            }

            // [1.2] epoll_wait with timeout=0; eventfd delivers instant wake on reconnect
            // Old: epoll_wait(..., 1) — 1ms artificial latency floor on every cycle.
            // New: timeout=0 (non-blocking poll) + eventfd fires when worker has new fd.
            int nev = ::epoll_wait(epfd_, events, 8, 0);
            if (nev < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (nev == 0) {
                // No events — yield briefly to avoid 100% CPU on idle
                std::this_thread::yield();
                continue;
            }

            for (int ev = 0; ev < nev; ++ev) {
                const int token = events[ev].data.fd;

                // [1.2] Drain eventfd notification — wake was just to check pending_fd_
                if (token == NOTIFY_EFD_TOKEN) {
                    uint64_t dummy;
                    ::read(notify_efd_, &dummy, sizeof(dummy));
                    continue;
                }

                const int conn_idx = token;
                if (static_cast<ConnState>(
                        conn_state_[conn_idx].val.load(std::memory_order_acquire))
                    != ConnState::CONNECTED) continue;

                // [1.5] Generation epoch barrier — drop zombie fd events.
                // UINT64_MAX sentinel (written in handle_disconnect) catches
                // in-flight events before new connection arrives.
                if (conns_[conn_idx].generation() != conn_epoch_[conn_idx].val) continue;

                if (events[ev].events & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
                    handle_disconnect(conn_idx);
                    continue;
                }
                if (events[ev].events & EPOLLIN) {
                    drain_and_parse(conn_idx);
                }
            }
        }

        if (epfd_ >= 0) { ::close(epfd_); epfd_ = -1; }
    }

private:
    // Sentinel stored in epoll data.fd to identify eventfd events (not a conn index)
    static constexpr int NOTIFY_EFD_TOKEN = -42;

    void epoll_register(int fd, int conn_idx) noexcept {
        struct epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = conn_idx;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void epoll_deregister(int fd) noexcept {
        if (fd >= 0 && epfd_ >= 0)
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    // [1.5] handle_disconnect: stamp epoch with UINT64_MAX immediately after
    // deregistration — guarantees any in-flight epoll events for this fd are
    // discarded by the generation check even before a new fd arrives.
    void handle_disconnect(int idx) noexcept {
        int old_fd = registered_fd_[idx].val.load(std::memory_order_acquire);
        epoll_deregister(old_fd);
        // [1.5] Poison epoch — zombie events from deregistered fd are filtered
        conn_epoch_[idx].val = UINT64_MAX;
        conns_[idx].close();
        registered_fd_[idx].val.store(-1, std::memory_order_release);
        conn_state_[idx].val.store(static_cast<int>(ConnState::DISCONNECTED),
                                    std::memory_order_release);
        if (active_idx_.load(std::memory_order_acquire) == idx) {
            // [2.3] Try to find another CONNECTED conn before blindly incrementing
            int next = -1;
            for (size_t j = 0; j < N; ++j) {
                int jj = static_cast<int>((idx + 1 + j) % N);
                if (static_cast<ConnState>(
                        conn_state_[jj].val.load(std::memory_order_acquire))
                    == ConnState::CONNECTED) {
                    next = jj; break;
                }
            }
            active_idx_.store(next >= 0 ? next : (idx + 1) % int(N),
                              std::memory_order_release);
        }
        reconnect_async(idx);
    }

    void drain_and_parse(int idx) noexcept {
        auto& conn = conns_[idx];
        while (true) {
            int r = conn.ssl_read_append();
            if (r == 0) break;
            if (r < 0) { handle_disconnect(idx); return; }
        }
        while (conn.available() >= 2) {
            const auto* p = reinterpret_cast<const uint8_t*>(conn.parse_ptr());
            const size_t avail = conn.available();
            const uint8_t b0 = p[0], b1 = p[1];
            const uint8_t op = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t payload_len = b1 & 0x7F;
            size_t hdr_size = 2;
            if (payload_len == 126) {
                if (avail < 4) break;
                payload_len = (uint64_t(p[2]) << 8) | p[3];
                hdr_size = 4;
            } else if (payload_len == 127) {
                if (avail < 10) break;
                payload_len = 0;
                for (int i = 0; i < 8; ++i) payload_len = (payload_len << 8) | p[2+i];
                hdr_size = 10;
            }
            if (masked) hdr_size += 4;
            const size_t frame_size = hdr_size + payload_len;

            if (frame_size > WsConnection::buf_capacity()) {
                // fprintf(stderr, "[KUSAK_DIAG] OVERSIZED FRAME: %zu bytes, dropping\n", frame_size);
                const size_t buffered = avail;
                const size_t remaining_on_wire = frame_size > buffered ? frame_size - buffered : 0;
                conn.reset_reassembly();
                if (remaining_on_wire > 0) {
                    if (!conn.discard_ssl_bytes(remaining_on_wire)) {
                        handle_disconnect(idx); return;
                    }
                }
                oversized_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (payload_len > sizeof(RingSlot::payload)) {
                if (avail < frame_size) break;
                conn.advance_cursor(frame_size);
                oversized_drops_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (avail < frame_size) break;

            const char* payload_ptr = conn.parse_ptr() + hdr_size;
            const int   plen        = static_cast<int>(payload_len);

            if (op == 0x8) { conn.advance_cursor(frame_size); handle_disconnect(idx); return; }
            if (op == 0x9) { conn.send_text(""); conn.advance_cursor(frame_size); continue; }
            if (op == 0x1 || op == 0x2) {
                if (masked) {
                    const uint8_t* mask_key =
                        reinterpret_cast<const uint8_t*>(conn.parse_ptr() + hdr_size - 4);
                    unmask_payload_64(const_cast<char*>(payload_ptr), plen, mask_key);
                }
                RingSlot* slot = ring_.try_claim();
                if (slot) {
                    const int copy_len = plen < int(sizeof(slot->payload))
                                       ? plen : int(sizeof(slot->payload));
                    std::memcpy(slot->payload, payload_ptr, copy_len);
                    slot->len = static_cast<uint16_t>(copy_len);
                    ring_.publish();
                    // fprintf(stderr, "[KUSAK_DIAG] RING: frame pushed len=%d\n", copy_len);
                } else {
                    // fprintf(stderr, "[KUSAK_DIAG] RING: DROP - ring full\n");
                }
            }
            conn.advance_cursor(frame_size);
        }
        conn.compact();
    }

    // [2.1] reconnect_worker_loop: ppoll with bounded tick so stop_ is honoured
    // promptly. Old: poll() inside open() blocked for up to 3s → destructor stall.
    // New: stop_ checked every 100ms inside poll_with_stop() used by open().
    void reconnect_worker_loop(int idx) {
        while (!stop_.load(std::memory_order_acquire)) {
            if (!reconnect_wake_[idx].val.load(std::memory_order_acquire)) {
                // [2.1] Use ppoll for the idle wait too — respond to stop_ promptly
                struct timespec ts{ .tv_sec = 0, .tv_nsec = 100'000'000L }; // 100ms
                ::ppoll(nullptr, 0, &ts, nullptr);
                continue;
            }
            reconnect_wake_[idx].val.store(false, std::memory_order_release);

            // fprintf(stderr, "[KUSAK_DIAG] RECONNECT: worker woke for conn=%d\n", idx);
            for (int i = 0; i < 20 && !stop_.load(std::memory_order_acquire); ++i) {
                // fprintf(stderr, "[KUSAK_DIAG] RECONNECT: attempt %d for conn=%d\n", i+1, idx);
                // [2.1] Pass stop_ into open() — every inner poll() can abort
                if (conns_[idx].open(cfg_.endpoints[idx], global_ssl_ctx_,
                                     saved_sessions_[idx], session_fails_[idx], &stop_)) {
                    // fprintf(stderr, "[KUSAK_DIAG] RECONNECT: SUCCESS conn=%d\n", idx);
                    conns_[idx].set_nonblock();

                    // [2.2] CRITICAL ordering: store pending_fd_ BEFORE conn_state_.
                    // Reactor loads pending_fd_ then conn_state_; if state is stored
                    // first the reactor may see CONNECTED with stale fd=-1 and skip
                    // registration entirely, leaving a connected but unmonitored socket.
                    pending_fd_[idx].val.store(conns_[idx].fd(),
                                               std::memory_order_release);
                    // [1.2] Wake reactor immediately via eventfd — no 1ms poll wait
                    if (notify_efd_ >= 0) {
                        uint64_t one = 1;
                        ::write(notify_efd_, &one, sizeof(one));
                    }
                    // [2.2] conn_state_ stored AFTER pending_fd_ — release ordering
                    conn_state_[idx].val.store(static_cast<int>(ConnState::CONNECTED),
                                               std::memory_order_release);
                    break;
                }
                // [2.1] Interruptible back-off: 50ms via ppoll, checks stop_
                struct timespec bt{ .tv_sec = 0, .tv_nsec = 50'000'000L };
                ::ppoll(nullptr, 0, &bt, nullptr);
            }
            if (static_cast<ConnState>(
                    conn_state_[idx].val.load(std::memory_order_acquire))
                != ConnState::CONNECTED) {
                conn_state_[idx].val.store(static_cast<int>(ConnState::DISCONNECTED),
                                            std::memory_order_release);
            }
        }
    }

    void reconnect_async(int idx) {
        int expected = static_cast<int>(ConnState::DISCONNECTED);
        if (!conn_state_[idx].val.compare_exchange_strong(
                expected,
                static_cast<int>(ConnState::CONNECTING),
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return;
        }
        reconnect_wake_[idx].val.store(true, std::memory_order_release);
    }

    // [3.0] dispatch_raw: direct fn-ptr calls — zero vtable, full inlining potential
    void dispatch_raw(const char* d, int len) noexcept {
        if (!d || len <= 0) return;
        const char* p = d;
        const char* e = d + len;
        p = detail::skip_ws(p, e);
        if (p >= e || *p != '{') return;

        const char* obj = detail::find_event_object(p, e);
        if (!obj || *obj != '{') return;

        const char* sp = obj + 1;
        std::string_view event_type;
        double val_p=0, val_q=0, val_b=0, val_a=0;
        uint64_t val_T=0;
        bool val_m=false;
        bool found_e=false, found_p=false, found_q=false, found_T=false, found_m=false;
        bool found_b=false, found_a=false;

        while (sp < e) {
            sp = detail::skip_ws(sp, e);
            if (sp >= e || *sp == '}') break;
            if (*sp == ',') { ++sp; continue; }
            std::string_view key;
            if (!detail::scan_key(sp, e, key)) break;
            detail::ScanResult val;
            if (!detail::scan_value(sp, e, val)) break;
            if (key.size() == 1) {
                switch (key[0]) {
                case 'e': event_type = val.value; found_e = true; break;
                case 'p': detail::parse_double_sv(val.value, val_p); found_p = true; break;
                case 'q': detail::parse_double_sv(val.value, val_q); found_q = true; break;
                case 'T': detail::parse_u64_sv(val.value, val_T);   found_T = true; break;
                case 'm': val_m = detail::parse_bool_sv(val.value);  found_m = true; break;
                case 'b': detail::parse_double_sv(val.value, val_b); found_b = true; break;
                case 'a': detail::parse_double_sv(val.value, val_a); found_a = true; break;
                default: break;
                }
            }
            if (sp < e && *sp == ',') ++sp;
        }

        // [3.0] Raw fn-ptr dispatch — compiler can inline if LTO is enabled
        if ((found_e && event_type == "bookTicker") || (!found_e && found_b && found_a)) {
            BookTicker bk{0, val_b, 0.0, val_a, 0.0};
            if (bk.best_bid > 0.0 && bk.best_ask > 0.0 && on_ticker_) {
                on_ticker_(bk, ticker_ctx_);
            }
        } else if (found_e && event_type == "aggTrade") {
            AggTrade t{val_T, val_p, val_q, val_m};
            if (t.price > 0.0 && t.qty > 0.0 && on_trade_) {
                on_trade_(t, trade_ctx_);
            }
        }
    }

    // ── Member layout ──────────────────────────────────────────────────────────

    Config cfg_;

    // [3.0] Raw fn pointers — no std::function, no type erasure
    OnBookTicker on_ticker_   = nullptr;
    void*        ticker_ctx_  = nullptr;
    OnAggTrade   on_trade_    = nullptr;
    void*        trade_ctx_   = nullptr;

    std::array<WsConnection, N> conns_;

    // [5.1] Per-connection atomics padded to 64 bytes — zero false sharing
    std::array<PaddedAtomicConnState, N> conn_state_;
    std::array<PaddedAtomicInt,       N> registered_fd_;
    std::array<PaddedAtomicInt,       N> pending_fd_;
    std::array<PaddedAtomicBool,      N> reconnect_wake_;
    std::array<PaddedAtomicSession,   N> saved_sessions_;
    std::array<PaddedAtomicU64,       N> session_fails_;
    // [5.1] Non-atomic epoch also padded (reactor-only, but adjacent to hot atomics)
    std::array<PaddedEpoch,           N> conn_epoch_;

    SpscFrameRing<4096> ring_;
    std::array<std::thread, N> reconnect_workers_;

    SSL_CTX* global_ssl_ctx_ = nullptr;
    std::atomic<uint64_t> oversized_drops_{0};
    int epfd_        = -1;
    int notify_efd_  = -1;  // [1.2] eventfd for zero-latency epoll reactor wake
    std::atomic<int>  active_idx_{0};
    std::atomic<bool> stop_{false};
};

// ── [1.4 + 4.1] make_binance_endpoints: snprintf, zero heap allocation ─────────
// Old: std::string path = "/stream?streams=" + sym_lower + ... — multiple heap allocs.
//      strncpy without null-termination guarantee.
// New: snprintf directly into WsEndpoint char fields — guaranteed NUL termination,
//      no dynamic memory, function is safe to call at runtime (symbol change).
inline std::array<WsEndpoint, 2>
make_binance_endpoints(std::string_view sym_lower) noexcept {
    WsEndpoint p{}, s{};

    // [1.4] snprintf guarantees NUL termination (unlike strncpy when src == dst size)
    ::snprintf(p.host, sizeof(p.host), "%s", "stream.binance.com");
    ::snprintf(p.port, sizeof(p.port), "%s", "9443");
    ::snprintf(p.path, sizeof(p.path), "/stream?streams=%.*s@aggTrade/%.*s@bookTicker",
               static_cast<int>(sym_lower.size()), sym_lower.data(),
               static_cast<int>(sym_lower.size()), sym_lower.data());

    ::snprintf(s.host, sizeof(s.host), "%s", "stream1.binance.com");
    ::snprintf(s.port, sizeof(s.port), "%s", "9443");
    ::snprintf(s.path, sizeof(s.path), "/stream?streams=%.*s@aggTrade/%.*s@bookTicker",
               static_cast<int>(sym_lower.size()), sym_lower.data(),
               static_cast<int>(sym_lower.size()), sym_lower.data());

    return {p, s};
}

} // namespace kusak::ws
