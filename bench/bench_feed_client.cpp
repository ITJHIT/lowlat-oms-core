// Multi-connection TCP client for comparing the epoll and io_uring feed
// handlers under the load where they are actually supposed to differ.
//
// One connection sending frames back-to-back does not exercise the thing that
// distinguishes the two I/O models: a single stream is dominated by whichever
// per-call syscall/context-switch cost is common to both (accept once, then a
// tight read-or-recv loop), and one-at-a-time it is not obviously cheaper to
// submit-and-complete than to wait-and-read. Concurrency is where they part
// ways -- epoll pays one `read()` syscall per readable socket per wakeup;
// io_uring can have many reads in flight and reap several completions per
// `io_uring_wait_cqe` call. So this opens many connections at once, each
// sending its own stream, and reports AGGREGATE throughput across all of them.
//
// What this measures, stated precisely because it is easy to overclaim:
// wall-clock from the first byte any connection sends to the last byte any
// connection finishes sending, across N concurrent connections, each blocked
// on its own `send()` and therefore subject to whatever backpressure the
// server's accept/read path applies. That is a real number -- it is
// **client-side send throughput under concurrent load**, which is bounded by
// how fast the server drains its side, the same relationship
// bench_ring_buffer's "streaming under back-pressure" throughput has to a true
// hand-off cost. It is NOT a per-message latency, because this wire format has
// no acknowledgement in either direction to time a round trip against; adding
// one would mean changing the feed handlers' protocol, not benchmarking the
// one they actually run in apps/. Correctness (were all sent frames actually
// matched, not silently dropped under load) is checked by the caller from the
// server's own printed fill tally after a graceful SIGTERM, not by this tool.
#if defined(__linux__)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

namespace {

struct Frame {
    std::uint32_t symbol;
    std::uint8_t side;
    std::uint8_t type;
    std::uint16_t pad;
    std::int64_t price;
    std::int64_t qty;
};
static_assert(sizeof(Frame) == 24, "must match the feed handlers' wire frame exactly");

// Alternates resting sells and crossing buys over a small, fixed set of price
// levels, so the book this order flow builds stays bounded (never grows
// unboundedly across a run) instead of the resting-side insert cost drifting
// upward as the benchmark progresses -- the point is to measure the network
// ingest path, not O(log levels) book-insert cost creeping in as a confound.
Frame make_frame(std::uint64_t i) {
    Frame f{};
    f.symbol = 1;
    f.side = (i % 2 == 0) ? 1 : 0;  // 1=sell rests, 0=buy crosses it
    f.type = 0;                    // limit
    f.pad = 0;
    f.price = 100 + static_cast<std::int64_t>((i / 2) % 32);
    f.qty = 1;
    return f;
}

int connect_to(const char* host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const void* buf, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 9001);
    const int connections = argc > 3 ? std::atoi(argv[3]) : 50;
    const std::uint64_t frames_per_conn = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 20000;

    if (connections <= 0 || frames_per_conn == 0) {
        std::fprintf(stderr, "usage: %s [host] [port] [connections] [frames_per_conn]\n", argv[0]);
        return 1;
    }

    std::vector<int> fds(static_cast<std::size_t>(connections), -1);
    for (int i = 0; i < connections; ++i) {
        fds[static_cast<std::size_t>(i)] = connect_to(host, port);
        if (fds[static_cast<std::size_t>(i)] < 0) {
            std::fprintf(stderr, "connection %d/%d failed to connect to %s:%u\n",
                         i + 1, connections, host, port);
            return 1;
        }
    }
    std::printf("bench_feed_client: %d connections established to %s:%u\n",
                connections, host, port);

    std::atomic<std::uint64_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(connections));

    const auto t0 = clk::now();
    for (int i = 0; i < connections; ++i) {
        const int fd = fds[static_cast<std::size_t>(i)];
        workers.emplace_back([fd, frames_per_conn, &failures] {
            for (std::uint64_t j = 0; j < frames_per_conn; ++j) {
                const Frame f = make_frame(j);
                if (!send_all(fd, &f, sizeof(f))) {
                    ++failures;
                    return;
                }
            }
        });
    }
    for (auto& t : workers) t.join();
    const auto t1 = clk::now();

    for (int fd : fds) ::close(fd);

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const std::uint64_t total_frames =
        static_cast<std::uint64_t>(connections) * frames_per_conn;
    const double bytes = static_cast<double>(total_frames) * sizeof(Frame);

    std::printf("bench_feed_client: %d connections x %llu frames = %llu frames sent in %.3f s\n",
                connections, (unsigned long long)frames_per_conn,
                (unsigned long long)total_frames, secs);
    std::printf("  aggregate send throughput: %.2f K frames/s, %.2f MB/s\n",
                total_frames / secs / 1e3, bytes / secs / 1e6);
    if (failures.load() > 0) {
        std::fprintf(stderr, "  %llu connection(s) failed mid-stream\n",
                     (unsigned long long)failures.load());
        return 1;
    }
    return 0;
}

#else  // not Linux

#include <cstdio>
int main() {
    std::printf("bench_feed_client targets the Linux-only feed handlers; "
                "nothing to benchmark on this platform.\n");
    return 0;
}

#endif
