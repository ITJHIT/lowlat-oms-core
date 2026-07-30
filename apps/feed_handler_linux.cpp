// Linux market-data feed handler: epoll + non-blocking TCP sockets on the
// network thread, a lock-free SPSC ring buffer as the IPC hand-off, and a
// matching/risk consumer thread. This is the "저지연 네트워크 -> 링버퍼 ->
// 매칭" path in one file.
//
// Wire frame (little-endian, 24 bytes, packed):
//   uint32 symbol | uint8 side(0=buy,1=sell) | uint8 type(0=limit,1=market)
//   uint16 pad    | int64 price | int64 qty
//
// Build only happens on Linux (see CMakeLists.txt); the #ifdef keeps the file
// compilable everywhere else so the repo is not "Linux or nothing".
#if defined(__linux__)

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

#include "lloms/config.hpp"
#include "lloms/matching_engine.hpp"
#include "lloms/risk_gate.hpp"
#include "lloms/spsc_ring_buffer.hpp"

using namespace lloms;

namespace {

constexpr std::size_t kFrameSize = 24;
constexpr int kMaxEvents = 64;

std::atomic<bool> g_running{true};

// Set once, by the reader thread, strictly after it has pushed its very last
// order -- main loop AND the final drain pass both done. Deliberately NOT the
// same signal as g_running. g_running flips true->false the instant SIGTERM
// arrives, which is exactly when the reader's final drain is only starting;
// if the consumer used g_running to decide "nothing more is coming," it could
// catch the ring transiently empty *between* two of the reader's post-signal
// pushes and exit right then, silently discarding everything the reader was
// about to push after that. Found by a concurrent-load benchmark coming up
// short by a few dozen fills even after the reader-side fix alone.
std::atomic<bool> g_reader_done{false};

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int make_listen_socket(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0 || !set_nonblocking(fd)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

Order decode(const unsigned char* buf) {
    Order o{};
    std::uint32_t symbol;
    std::uint8_t side, type;
    std::int64_t price, qty;
    std::memcpy(&symbol, buf + 0, 4);
    side = buf[4];
    type = buf[5];
    std::memcpy(&price, buf + 8, 8);
    std::memcpy(&qty, buf + 16, 8);
    o.symbol = symbol;
    o.side = side == 0 ? Side::Buy : Side::Sell;
    o.type = type == 0 ? Type::Limit : Type::Market;
    o.price = price;
    o.qty = qty;
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    // Graceful shutdown: a real operator (or a CI smoke test) needs to stop
    // this process and see the consumer's final tally rather than kill -9 it
    // and learn nothing. Both signals just flip the flag; the epoll_wait
    // timeout below (re-checked every loop) is what actually notices it.
    std::signal(SIGINT, [](int) { g_running.store(false); });
    std::signal(SIGTERM, [](int) { g_running.store(false); });

    Config cfg = argc > 1 ? Config::from_file(argv[1]) : Config{};
    const auto port = static_cast<std::uint16_t>(cfg.get_int("port", 9001));

    static SpscRingBuffer<Order, 1u << 16> ring;
    RiskGate gate(RiskLimits{cfg.get_int("max_order_qty", 1000),
                             cfg.get_int("max_position", 0),
                             cfg.get_int("reference_price", 0),
                             cfg.get_int("price_band", 0)});

    // Consumer: drain the ring, risk-check, match.
    std::thread consumer([&] {
        MatchingEngine eng(1);
        std::vector<Fill> fills;
        std::uint64_t matched = 0, rejected = 0;
        Order o;
        while (!g_reader_done.load(std::memory_order_relaxed) || !ring.empty()) {
            if (!ring.pop(o)) {
                std::this_thread::yield();
                continue;
            }
            if (gate.check(o, 0) != RiskDecision::Accept) {
                ++rejected;
                continue;
            }
            fills.clear();
            eng.submit(o, fills);
            matched += fills.size();
        }
        std::printf("consumer stopped: %llu fills, %llu risk-rejected\n",
                    (unsigned long long)matched, (unsigned long long)rejected);
    });

    const int listen_fd = make_listen_socket(port);
    if (listen_fd < 0) {
        std::perror("listen");
        g_running.store(false);
        g_reader_done.store(true);  // never entered the loop; nothing to wait for
        consumer.join();
        return 1;
    }
    const int ep = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);

    std::printf("feed handler listening on port %u\n", port);
    std::unordered_map<int, std::vector<unsigned char>> partial;
    epoll_event events[kMaxEvents];

    while (g_running.load()) {
        const int nfds = ::epoll_wait(ep, events, kMaxEvents, 1000);
        for (int i = 0; i < nfds; ++i) {
            const int fd = events[i].data.fd;
            if (fd == listen_fd) {
                int conn;
                while ((conn = ::accept(listen_fd, nullptr, nullptr)) >= 0) {
                    set_nonblocking(conn);
                    epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = conn;
                    ::epoll_ctl(ep, EPOLL_CTL_ADD, conn, &cev);
                    partial[conn];
                }
                continue;
            }
            auto& buf = partial[fd];
            unsigned char tmp[4096];
            ssize_t r;
            while ((r = ::read(fd, tmp, sizeof(tmp))) > 0) {
                buf.insert(buf.end(), tmp, tmp + r);
                while (buf.size() >= kFrameSize) {
                    Order o = decode(buf.data());
                    buf.erase(buf.begin(), buf.begin() + kFrameSize);
                    // Unconditional: an order that has already been decoded
                    // must always reach the consumer. Bailing out of this spin
                    // when g_running flips false during shutdown would
                    // silently drop it -- the consumer thread is still running
                    // at this point (its exit condition waits on g_reader_done,
                    // not g_running -- see that flag's own comment for why the
                    // two must not be conflated), so there is never a reason to
                    // abandon a push here.
                    while (!ring.push(o)) {
                    }
                }
            }
            if (r == 0) {  // peer closed
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                partial.erase(fd);
            }
        }
    }

    // Catch any connections still sitting in the accept backlog. A connection
    // whose TCP handshake completed but which this process had not yet called
    // accept() for does not exist in `partial` yet -- no drain pass below,
    // however thorough, could ever see data on a fd that was never opened.
    // listen_fd is already non-blocking (set at creation), so this drains the
    // backlog rather than blocking on it.
    {
        int conn;
        while ((conn = ::accept(listen_fd, nullptr, nullptr)) >= 0) {
            set_nonblocking(conn);
            partial[conn];
        }
    }

    // Final drain, found the hard way (a concurrent-load benchmark came up
    // short in CI, twice, for two different reasons). g_running flipping
    // false only guarantees the epoll_wait call ALREADY IN PROGRESS finishes;
    // it says nothing about whether every byte a connection's kernel receive
    // buffer was already holding got read before the outer loop stopped
    // asking. One more non-blocking pass over every now-known connection,
    // unconditional on g_running, closes that race instead of leaving
    // already-arrived data silently unread.
    for (auto& [fd, buf] : partial) {
        unsigned char tmp[4096];
        ssize_t r;
        while ((r = ::read(fd, tmp, sizeof(tmp))) > 0) {
            buf.insert(buf.end(), tmp, tmp + r);
            while (buf.size() >= kFrameSize) {
                Order o = decode(buf.data());
                buf.erase(buf.begin(), buf.begin() + kFrameSize);
                while (!ring.push(o)) {
                }
            }
        }
    }

    ::close(listen_fd);
    ::close(ep);
    g_running.store(false);
    // Only now: every push this thread will ever make has already happened.
    // The consumer waits on this, not on g_running, precisely so it cannot
    // observe a transiently-empty ring in the window between the signal
    // arriving and the final drain above actually finishing.
    g_reader_done.store(true);
    consumer.join();
    return 0;
}

#else  // not Linux

#include <cstdio>
int main() {
    std::printf("feed_handler is Linux-only (epoll). Build on Linux; "
                "the portable core (ring buffer, order book, matching, risk) "
                "runs everywhere.\n");
    return 0;
}

#endif
