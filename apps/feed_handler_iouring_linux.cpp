// Linux market-data feed handler: io_uring instead of epoll on the network
// thread, everything downstream identical to feed_handler_linux.cpp (lock-free
// SPSC ring hand-off, then a risk+matching consumer thread) -- so the only
// variable between the two files is the socket I/O model, which is the point.
//
// Wire frame (little-endian, 24 bytes, packed) -- identical to
// feed_handler_linux.cpp, deliberately duplicated (not shared via a header)
// rather than factored out, so this file's build never depends on that one:
//   uint32 symbol | uint8 side(0=buy,1=sell) | uint8 type(0=limit,1=market)
//   uint16 pad    | int64 price | int64 qty
//
// What actually changes going from epoll to io_uring, and why it is not just
// "the same loop with different function names":
//
//   epoll tells you a socket is READABLE; you still make a synchronous read()
//   syscall yourself, into a buffer you own for exactly the duration of that
//   call. io_uring does the read itself, asynchronously, sometime between
//   submission and completion -- the buffer you hand it has to stay alive and
//   unmoved for that entire window, which can span multiple trips around the
//   event loop. A buffer that lived on the stack of the function that
//   submitted the request, or inside a std::vector that might reallocate, is a
//   use-after-free waiting for the kernel to win the race. That is why every
//   in-flight buffer here lives inside a heap-allocated, pointer-stable Conn
//   (owned by a std::unique_ptr in a map, never moved, freed only after the
//   connection has no outstanding request referencing it) rather than a
//   function-local array.
//
//   The other consequence: a completion cannot mean "this fd is ready, go read
//   it" -- io_uring needs to be told WHICH read (which Conn, which buffer)
//   completed, since several can be in flight across different connections at
//   once. Each submitted SQE is tagged with a heap-allocated Op carrying that
//   context, retrieved from the matching CQE and freed exactly once per
//   completion.
//
// Needs liburing (liburing-dev on Debian/Ubuntu) on a kernel new enough to
// support it; degrades to a stub message otherwise so the repo is not
// "io_uring or nothing" on a Linux box that simply lacks the library, the same
// way the epoll file degrades to a stub off Linux entirely. Verification for
// this file is CI-only: no Linux toolchain on the machine that wrote it.
#if defined(__linux__) && __has_include(<liburing.h>)
#define LLOMS_HAVE_LIBURING 1
#endif

#if defined(LLOMS_HAVE_LIBURING)

#include <arpa/inet.h>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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
// One outstanding accept plus one outstanding recv per live connection, so
// this comfortably covers ~255 concurrent connections before io_uring_get_sqe
// could return null from a full submission queue. Demo-scoped: a version meant
// to run past that would need to flush (io_uring_submit) and retry rather than
// assume a slot is always available, which the two submit_* helpers below do
// not do.
constexpr unsigned kQueueDepth = 256;
constexpr std::size_t kRecvBufSize = 4096;

std::atomic<bool> g_running{true};

// Set once, by the reader thread, strictly after it has pushed its very last
// order -- main loop AND the final drain pass both done. Deliberately NOT the
// same signal as g_running: see feed_handler_linux.cpp's identical field for
// why conflating the two loses in-flight orders whenever the shutdown-time
// drain pushes something after the ring last looked empty to the consumer.
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
    // Non-blocking so the shutdown-time backlog drain below (a plain ::accept
    // loop) can never block waiting for a connection that is never coming --
    // io_uring's own async accept does not care about this flag either way.
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

// Per-connection state. Allocated once on accept, never moved, freed only
// after the connection closes and no submitted SQE still points at it --
// see the file header comment on buffer lifetime.
struct Conn {
    int fd = -1;
    std::vector<unsigned char> partial;         // undecoded bytes carried across reads
    std::array<unsigned char, kRecvBufSize> buf{};  // the CURRENT in-flight read's target
};

enum class OpKind : std::uint8_t { Accept, Recv };

// Tag attached to one submitted SQE, retrieved from its CQE on completion.
// Heap-allocated per submission and freed exactly once, by whichever code path
// handles that completion -- there is exactly one CQE per SQE, so this cannot
// double-free or leak in the steady state (see the shutdown note below for the
// one place it deliberately does not bother).
struct Op {
    OpKind kind;
    Conn* conn;  // null for Accept
};

void submit_accept(io_uring* ring, int listen_fd) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, new Op{OpKind::Accept, nullptr});
}

void submit_recv(io_uring* ring, Conn* c) {
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, c->fd, c->buf.data(), c->buf.size(), 0);
    io_uring_sqe_set_data(sqe, new Op{OpKind::Recv, c});
}

}  // namespace

int main(int argc, char** argv) {
    // Graceful shutdown, matching feed_handler_linux.cpp exactly: flip the
    // flag, let the wait-with-timeout loop below notice it on its own.
    std::signal(SIGINT, [](int) { g_running.store(false); });
    std::signal(SIGTERM, [](int) { g_running.store(false); });

    Config cfg = argc > 1 ? Config::from_file(argv[1]) : Config{};
    const auto port = static_cast<std::uint16_t>(cfg.get_int("port", 9001));

    static SpscRingBuffer<Order, 1u << 16> ring;
    RiskGate gate(RiskLimits{cfg.get_int("max_order_qty", 1000),
                             cfg.get_int("max_position", 0),
                             cfg.get_int("reference_price", 0),
                             cfg.get_int("price_band", 0)});

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

    io_uring uring;
    if (io_uring_queue_init(kQueueDepth, &uring, 0) < 0) {
        std::perror("io_uring_queue_init");
        ::close(listen_fd);
        g_running.store(false);
        g_reader_done.store(true);
        consumer.join();
        return 1;
    }

    std::unordered_map<int, std::unique_ptr<Conn>> conns;

    submit_accept(&uring, listen_fd);
    io_uring_submit(&uring);

    std::printf("io_uring feed handler listening on port %u\n", port);

    while (g_running.load()) {
        io_uring_cqe* cqe = nullptr;
        // Timeout, not an unbounded wait, purely so the loop rechecks
        // g_running periodically -- exactly the role epoll_wait's 1000ms
        // timeout plays in feed_handler_linux.cpp.
        __kernel_timespec ts{1, 0};
        const int rc = io_uring_wait_cqe_timeout(&uring, &cqe, &ts);
        if (rc == -ETIME || rc == -EINTR) {
            // -ETIME: nothing completed within the second; go recheck
            // g_running. -EINTR: a signal (SIGINT/SIGTERM, see above) broke
            // the wait early -- exactly what should happen, not an error.
            continue;
        }
        if (rc < 0) {
            break;  // the ring itself is in a bad state; stop rather than spin
        }

        // Process the completion that woke us, then keep pulling any others
        // already sitting in the completion queue with a non-blocking peek
        // before paying for another io_uring_enter round trip, submitting
        // once for the whole batch at the end. Found necessary after the
        // accept-backlog and EAGAIN fixes above stopped explaining the
        // benchmark's remaining shortfall on their own: reaping strictly one
        // completion per syscall round trip is the one part of this loop
        // with no equivalent in feed_handler_linux.cpp, whose read-to-EAGAIN
        // loop drains an entire ready socket per epoll_wait wakeup, and
        // whose epoll_wait itself reports every ready fd in one call rather
        // than one syscall per fd. Under the bursty, all-at-once load this
        // benchmark sends, that per-completion round-trip cost was enough
        // for the server to fall behind the client on a contended runner --
        // not because any single completion was mishandled, but because
        // reaping them one at a time is slower than io_uring can produce
        // them.
        for (;;) {
            auto* op = static_cast<Op*>(io_uring_cqe_get_data(cqe));
            const int res = cqe->res;

            if (op->kind == OpKind::Accept) {
                if (res >= 0) {
                    const int conn_fd = res;
                    auto c = std::make_unique<Conn>();
                    c->fd = conn_fd;
                    Conn* raw = c.get();
                    conns.emplace(conn_fd, std::move(c));
                    submit_recv(&uring, raw);
                }
                // Re-arm regardless: a transient accept error must not stop
                // the listener from accepting the next connection.
                submit_accept(&uring, listen_fd);
            } else {
                Conn* c = op->conn;
                if (res > 0) {
                    c->partial.insert(c->partial.end(), c->buf.begin(), c->buf.begin() + res);
                    while (c->partial.size() >= kFrameSize) {
                        Order o = decode(c->partial.data());
                        c->partial.erase(c->partial.begin(), c->partial.begin() + kFrameSize);
                        // Unconditional, matching feed_handler_linux.cpp: an
                        // order that has already been decoded must always
                        // reach the consumer, which is still running at this
                        // point (its exit condition waits on g_reader_done,
                        // not g_running -- see that flag's own comment for
                        // why).
                        while (!ring.push(o)) {
                        }
                    }
                    submit_recv(&uring, c);
                } else if (res == -EAGAIN || res == -EINTR) {
                    // Transient, not a dead connection: io_uring's recv fast
                    // path can surface EAGAIN instead of internally arming a
                    // poll and waiting -- documented io_uring behavior under
                    // socket-buffer or io-wq scheduling pressure, which a
                    // busy shared CI runner reproduces far more often than a
                    // quiet box. Found by this being the one case still
                    // treated as "peer went away" even after the
                    // accept-backlog drain fix, and a benchmark that kept
                    // losing hundreds of frames from what should have been
                    // healthy connections -- discarding the connection here
                    // would throw away every frame it had left to send, not
                    // just the one recv that happened to come back EAGAIN.
                    // Resubmit and keep going, exactly like a real error is
                    // NOT handled.
                    submit_recv(&uring, c);
                } else {
                    // res == 0: peer closed. res < 0 other than the
                    // transient cases above: a real error. Nothing this feed
                    // handler can do with a dead connection except stop
                    // tracking it.
                    ::close(c->fd);
                    conns.erase(c->fd);
                }
            }

            delete op;
            io_uring_cqe_seen(&uring, cqe);

            if (io_uring_peek_cqe(&uring, &cqe) != 0) {
                break;  // nothing else completed yet -- submit and go wait again
            }
        }
        io_uring_submit(&uring);
    }

    // Final drain, found the hard way (a 200k-frame concurrent-load benchmark
    // came up short by a few dozen fills in CI). g_running flipping false only
    // guarantees the io_uring_wait_cqe_timeout call ALREADY IN PROGRESS
    // finishes; it says nothing about whether every recv SQE still
    // outstanding at that moment had already completed (bytes copied into its
    // Conn's buffer by the kernel) without anyone having reaped that CQE yet.
    //
    // This deliberately stays inside io_uring rather than falling back to a
    // synchronous recv() on the same fds: a plain recv() would race the
    // kernel against whichever outstanding recv SQE is still servicing that
    // exact socket, and there is no clean way to tell afterward which of the
    // two actually consumed a given byte. Reaping the completion queue is
    // race-free by construction -- it only ever reports what io_uring itself
    // already finished. A short bounded wait (rather than a non-blocking
    // peek) also gives genuinely in-flight loopback traffic -- already
    // in-kernel, arriving within microseconds -- one last real chance to
    // complete before shutdown proceeds; ETIME twice in a row means nothing
    // more is coming, so this does not turn a fast exit into a slow one in
    // the ordinary case. Nothing here is re-armed: shutdown is underway, and
    // submitting a fresh SQE at this point would just create one more
    // outstanding request nothing will ever reap.
    for (int misses = 0; misses < 2;) {
        io_uring_cqe* cqe = nullptr;
        __kernel_timespec ts{0, 50'000'000};  // 50ms
        const int rc = io_uring_wait_cqe_timeout(&uring, &cqe, &ts);
        if (rc == -ETIME) {
            ++misses;
            continue;
        }
        if (rc < 0) {
            break;
        }
        misses = 0;

        // Same peek-drain as the main loop above: a burst of completions
        // that all landed together should not each cost their own 50ms wait
        // cycle before the miss counter even starts counting down.
        for (;;) {
            auto* op = static_cast<Op*>(io_uring_cqe_get_data(cqe));
            if (op->kind == OpKind::Recv && cqe->res > 0) {
                Conn* c = op->conn;
                const int res = cqe->res;
                c->partial.insert(c->partial.end(), c->buf.begin(), c->buf.begin() + res);
                while (c->partial.size() >= kFrameSize) {
                    Order o = decode(c->partial.data());
                    c->partial.erase(c->partial.begin(), c->partial.begin() + kFrameSize);
                    while (!ring.push(o)) {
                    }
                }
            } else if (op->kind == OpKind::Accept && cqe->res >= 0) {
                // A connection whose accept only completed during this drain
                // phase has no outstanding recv SQE and nothing here will
                // submit one (shutdown is underway) -- so it gets one inline
                // synchronous drain instead, rather than being silently
                // discarded along with every frame it was ever going to
                // send. This is exactly the fallback the header comment
                // above says to avoid for a connection with a recv SQE
                // genuinely racing the kernel; there is no such race here,
                // since this connection never had one.
                const int conn_fd = cqe->res;
                std::vector<unsigned char> late;
                unsigned char tmp[kRecvBufSize];
                ssize_t r;
                while ((r = ::recv(conn_fd, tmp, sizeof(tmp), MSG_DONTWAIT)) > 0) {
                    late.insert(late.end(), tmp, tmp + r);
                    while (late.size() >= kFrameSize) {
                        Order o = decode(late.data());
                        late.erase(late.begin(), late.begin() + kFrameSize);
                        while (!ring.push(o)) {
                        }
                    }
                }
                ::close(conn_fd);
            }
            delete op;
            io_uring_cqe_seen(&uring, cqe);

            if (io_uring_peek_cqe(&uring, &cqe) != 0) {
                break;  // nothing else completed yet -- go back to the timed wait
            }
        }
    }

    // Accept-backlog drain, mirroring feed_handler_linux.cpp's identical fix
    // (found the same way: a concurrent-load benchmark coming up short, worse
    // the more connections piled up). Exactly one accept SQE is ever
    // outstanding at a time in the main loop above -- each Accept completion
    // re-arms only the next single one -- and nothing re-arms it anymore once
    // shutdown begins. So the CQE-reap loop just above can catch at most ONE
    // more connection from the kernel's accept backlog; every other
    // connection still sitting there is invisible to it, because a
    // connection this process has not called accept() on yet has no fd, no
    // Conn, and no outstanding SQE for any drain pass to find. listen_fd is
    // non-blocking, so this drains the backlog rather than blocking on it.
    {
        int conn_fd;
        while ((conn_fd = ::accept(listen_fd, nullptr, nullptr)) >= 0) {
            std::vector<unsigned char> late;
            unsigned char tmp[kRecvBufSize];
            ssize_t r;
            while ((r = ::recv(conn_fd, tmp, sizeof(tmp), MSG_DONTWAIT)) > 0) {
                late.insert(late.end(), tmp, tmp + r);
                while (late.size() >= kFrameSize) {
                    Order o = decode(late.data());
                    late.erase(late.begin(), late.begin() + kFrameSize);
                    while (!ring.push(o)) {
                    }
                }
            }
            ::close(conn_fd);
        }
    }

    // Outstanding SQEs (the last accept, and one recv per still-open
    // connection) are not drained here: their Op allocations leak, once, for a
    // process that is exiting immediately afterward -- the same category of
    // non-issue as not calling free() on live heap right before exit(). A
    // long-running service that starts and stops this loop repeatedly would
    // need to drain them (io_uring_wait_cqe per outstanding submission) rather
    // than skip this step; a feed handler process does not.
    for (auto& [fd, c] : conns) {
        ::close(fd);
    }
    ::close(listen_fd);
    io_uring_queue_exit(&uring);

    g_running.store(false);
    // Only now: every push this thread will ever make has already happened.
    // The consumer waits on this, not on g_running, precisely so it cannot
    // observe a transiently-empty ring in the window between the signal
    // arriving and the final drain above actually finishing.
    g_reader_done.store(true);
    consumer.join();
    return 0;
}

#else  // not Linux, or liburing unavailable

#include <cstdio>
int main() {
#if defined(__linux__)
    std::printf("feed_handler_iouring needs liburing (liburing-dev) at build "
                "time. Build on Linux with liburing installed; the portable "
                "core (ring buffer, order book, matching, risk) runs "
                "everywhere, and feed_handler_linux (epoll) needs only a "
                "Linux kernel, no extra library.\n");
#else
    std::printf("feed_handler_iouring is Linux-only. The portable core (ring "
                "buffer, order book, matching, risk) runs everywhere.\n");
#endif
    return 0;
}

#endif
