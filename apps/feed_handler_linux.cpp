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
        while (g_running.load(std::memory_order_relaxed) || !ring.empty()) {
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
                    while (!ring.push(o) && g_running.load()) {
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

    ::close(listen_fd);
    ::close(ep);
    g_running.store(false);
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
