// Two processes, one shared-memory segment, two lock-free rings, no locks and
// no kernel on the data path.
//
// This is the multi-process shape a trading stack actually runs in: a feed
// process and a strategy/OMS process that must exchange messages without a
// syscall per message, started from one config file, each pinned to its own
// core. It measures both numbers that matter, and labels which is which:
//
//   Phase 1 -- streaming: writer floods the ring, reader drains and checksums.
//              Gives cross-process throughput and end-to-end latency *under
//              back-pressure* (queue residency included, and said so).
//   Phase 2 -- ping-pong: one message in flight at a time, so the round trip is
//              the IPC cost with no residency hiding in it.
//
// Two fork() details that are bugs everywhere they are skipped:
//   * The child calls _exit(), never exit() or return. A fork child that runs
//     the parent's destructors unlinks the parent's shared-memory name and
//     flushes its stdio buffers a second time.
//   * The child attaches by *name* with shm_open rather than relying on the
//     inherited mapping, so this exercises the same path two independently
//     launched processes would take.
#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "lloms/config.hpp"
#include "lloms/cpu.hpp"
#include "lloms/latency_histogram.hpp"
#include "lloms/posix_shm.hpp"
#include "lloms/shm_ring.hpp"

using namespace lloms;
using clk = std::chrono::steady_clock;

namespace {

struct Msg {
    std::uint64_t seq;
    std::uint64_t payload;
};

constexpr std::uint64_t kSentinel = ~0ULL;
// Any spin-wait that can outlive a crashed peer needs a deadline, or a dead
// counterparty turns into a wedged process holding a shared segment open.
constexpr double kSpinTimeoutSec = 30.0;

using Ring = ShmSpscRing<Msg>;

std::size_t round_up_64(std::size_t n) {
    return (n + 63u) & ~static_cast<std::size_t>(63u);
}

std::uint64_t mix(std::uint64_t i) {
    return i * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch())
        .count();
}

// Spin until a message arrives, or give up. Returns false on timeout.
bool pop_blocking(Ring& r, Msg& out, const char* what) {
    const auto deadline = clk::now() + std::chrono::duration<double>(kSpinTimeoutSec);
    while (!r.pop(out)) {
        if (clk::now() > deadline) {
            std::fprintf(stderr, "timed out waiting for %s\n", what);
            return false;
        }
    }
    return true;
}

bool push_blocking(Ring& r, const Msg& m, const char* what) {
    const auto deadline = clk::now() + std::chrono::duration<double>(kSpinTimeoutSec);
    while (!r.push(m)) {
        if (clk::now() > deadline) {
            std::fprintf(stderr, "timed out pushing %s\n", what);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Child: the "strategy/OMS" side.
// ---------------------------------------------------------------------------
[[noreturn]] void run_reader(const std::string& shm_name, std::size_t seg_bytes,
                             std::size_t ring_bytes, unsigned core) {
    PosixSharedMemory seg = PosixSharedMemory::open_existing(shm_name, seg_bytes);
    if (!seg.valid()) {
        std::fprintf(stderr, "reader: %s\n", seg.error().c_str());
        _exit(2);
    }
    auto* base = static_cast<unsigned char*>(seg.data());

    Ring to_reader, to_writer;
    if (Ring::attach(base, ring_bytes, to_reader) != ShmAttachStatus::Ok ||
        Ring::attach(base + ring_bytes, ring_bytes, to_writer) != ShmAttachStatus::Ok) {
        std::fprintf(stderr, "reader: attach failed\n");
        _exit(2);
    }
    const AffinityStatus pin = pin_this_thread_to_core(core);

    // Phase 1: drain until the sentinel, checksumming as we go.
    std::uint64_t count = 0;
    std::uint64_t checksum = 0;
    Msg m{};
    for (;;) {
        if (!pop_blocking(to_reader, m, "stream")) {
            _exit(3);
        }
        if (m.seq == kSentinel) {
            break;
        }
        checksum ^= m.payload + m.seq;
        ++count;
    }
    // Report back through the return ring: count in seq, checksum in payload.
    if (!push_blocking(to_writer, Msg{count, checksum}, "stream ack")) {
        _exit(3);
    }

    // Phase 2: echo every message straight back.
    for (;;) {
        if (!pop_blocking(to_reader, m, "ping")) {
            _exit(3);
        }
        if (m.seq == kSentinel) {
            break;
        }
        if (!push_blocking(to_writer, m, "pong")) {
            _exit(3);
        }
    }

    std::fprintf(stderr, "reader: pinned to core %u [%s], %llu streamed messages verified\n",
                 core, to_string(pin), (unsigned long long)count);
    // _exit, not exit: the inherited PosixSharedMemory still believes it owns
    // the segment name, and its destructor would unlink it out from under the
    // parent.
    _exit(0);
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = argc > 1 ? Config::from_file(argv[1]) : Config{};
    const std::string shm_name = cfg.get_str("shm_name", "/lloms_ipc_demo");
    const auto capacity = static_cast<std::uint32_t>(cfg.get_int("capacity", 4096));
    const auto stream_count = static_cast<std::uint64_t>(cfg.get_int("stream_count", 2'000'000));
    const auto rtt_iters = static_cast<std::uint64_t>(cfg.get_int("rtt_iters", 200'000));
    const auto writer_core = static_cast<unsigned>(cfg.get_int("writer_core", 0));
    const auto reader_core = static_cast<unsigned>(cfg.get_int("reader_core", 1));

    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
        std::fprintf(stderr, "capacity must be a power of two >= 2 (got %u)\n", capacity);
        return 1;
    }

    const std::size_t ring_bytes = round_up_64(Ring::bytes_needed(capacity));
    const std::size_t seg_bytes = ring_bytes * 2;

    // A leftover segment from a killed run would make shm_open(O_EXCL) fail.
    // Clearing it is safe here because this demo owns the name; a real service
    // should refuse to start instead, since a live instance may be using it.
    PosixSharedMemory::unlink(shm_name);

    PosixSharedMemory seg = PosixSharedMemory::create(shm_name, seg_bytes);
    if (!seg.valid()) {
        std::fprintf(stderr, "create %s: %s\n", shm_name.c_str(), seg.error().c_str());
        return 1;
    }
    auto* base = static_cast<unsigned char*>(seg.data());

    Ring to_reader, to_writer;
    if (Ring::create(base, ring_bytes, capacity, to_reader) != ShmAttachStatus::Ok ||
        Ring::create(base + ring_bytes, ring_bytes, capacity, to_writer) != ShmAttachStatus::Ok) {
        std::fprintf(stderr, "ring init failed\n");
        return 1;
    }

    std::fflush(nullptr);  // never let a forked child inherit a dirty stdio buffer
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        return 1;
    }
    if (pid == 0) {
        run_reader(shm_name, seg_bytes, ring_bytes, reader_core);
    }

    const AffinityStatus pin = pin_this_thread_to_core(writer_core);
    std::printf("shm IPC demo: segment %s, %zu bytes, two rings of %u slots\n",
                shm_name.c_str(), seg_bytes, capacity);
    std::printf("  writer pid %ld -> core %u [%s], reader pid %ld -> core %u\n",
                (long)::getpid(), writer_core, to_string(pin), (long)pid, reader_core);

    // ---- Phase 1: streaming throughput ------------------------------------
    std::uint64_t expect_checksum = 0;
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < stream_count; ++i) {
        const Msg m{i, mix(i)};
        expect_checksum ^= m.payload + m.seq;
        if (!push_blocking(to_reader, m, "stream")) {
            return 1;
        }
    }
    if (!push_blocking(to_reader, Msg{kSentinel, 0}, "stream sentinel")) {
        return 1;
    }
    Msg ack{};
    if (!pop_blocking(to_writer, ack, "stream ack")) {
        return 1;
    }
    const auto t1 = clk::now();
    const double stream_secs = std::chrono::duration<double>(t1 - t0).count();

    const bool stream_ok = (ack.seq == stream_count && ack.payload == expect_checksum);
    std::printf("  phase 1 (streaming): %llu messages in %.3f s = %.2f M msg/s across processes\n",
                (unsigned long long)stream_count, stream_secs,
                stream_count / stream_secs / 1e6);
    std::printf("           integrity: %s (reader saw %llu messages, checksum %s)\n",
                stream_ok ? "OK" : "MISMATCH", (unsigned long long)ack.seq,
                ack.payload == expect_checksum ? "match" : "MISMATCH");
    if (!stream_ok) {
        std::fprintf(stderr, "cross-process data loss detected\n");
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        return 1;
    }

    // ---- Phase 2: cross-process round trip ---------------------------------
    // Queue depth is 1, so this is the IPC hand-off cost, not queue residency.
    LatencyHistogram rtt(6);
    Msg echo{};
    const std::uint64_t warmup = rtt_iters / 10 + 1000;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        if (!push_blocking(to_reader, Msg{i, 0}, "warmup") ||
            !pop_blocking(to_writer, echo, "warmup echo")) {
            return 1;
        }
    }
    for (std::uint64_t i = 0; i < rtt_iters; ++i) {
        const std::int64_t start = now_ns();
        if (!push_blocking(to_reader, Msg{i, 0}, "ping") ||
            !pop_blocking(to_writer, echo, "pong")) {
            return 1;
        }
        const std::int64_t end = now_ns();
        if (echo.seq != i) {
            std::fprintf(stderr, "ping-pong mismatch: sent %llu got %llu\n",
                         (unsigned long long)i, (unsigned long long)echo.seq);
            return 1;
        }
        rtt.record(end > start ? static_cast<std::uint64_t>(end - start) : 0);
    }
    (void)push_blocking(to_reader, Msg{kSentinel, 0}, "stop");

    std::printf("  phase 2 (ping-pong, depth 1): RTT ns p50=%llu p99=%llu p99.9=%llu max=%llu\n",
                (unsigned long long)rtt.percentile(50), (unsigned long long)rtt.percentile(99),
                (unsigned long long)rtt.percentile(99.9), (unsigned long long)rtt.max());
    std::printf("           one-way estimate (RTT/2): p50=%llu ns\n",
                (unsigned long long)(rtt.percentile(50) / 2));

    int status = 0;
    ::waitpid(pid, &status, 0);
    const int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    std::printf("  reader exited rc=%d; segment unlinked by the creator on scope exit\n", child_rc);
    return child_rc == 0 ? 0 : 1;
}

#else  // not POSIX

#include <cstdio>
int main() {
    std::printf("shm_ipc demo needs POSIX shared memory (shm_open/mmap/fork).\n"
                "The ring itself is portable and is covered by tests/test_shm_ring.cpp\n"
                "on every platform.\n");
    return 0;
}

#endif
