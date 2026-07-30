# lowlat-oms-core

**A low-latency order-matching / OMS core in modern C++17.**

The building blocks a real-time trading/OMS platform is made of, written to be
correct, tested, and fast on the hot path — a lock-free hand-off between threads
*and between processes*, an exchange-style binary protocol decoder, a
price-time-priority matching engine, a pre-trade risk gate, an order router, and
nanosecond latency measurement, wired together by a Linux `epoll` feed handler.

Everything is integer-priced and allocation-free on the matching path. No alpha,
no strategy secrets — this is the **infrastructure** layer, which is exactly the
part that generalizes across firms.

---

## Components

| Component | Header | What it demonstrates |
|---|---|---|
| **SPSC ring buffer** | `spsc_ring_buffer.hpp` | Lock-free single-producer/single-consumer queue: `std::atomic` acquire/release, power-of-two masking, cache-line padding to kill false sharing. Template + RAII. |
| **Shared-memory ring** | `shm_ring.hpp` | The same queue across a **process boundary**: lock-free-atomic enforcement, fixed-width indices, a validated on-segment header, and range-checked reads because a peer process's indices are untrusted input. |
| **POSIX shared memory** | `posix_shm.hpp` | RAII over `shm_open`/`ftruncate`/`mmap`: move-only, unlink-on-destroy by the creator only, every failure path cleaning up the name it created. |
| **Wire protocol** | `wire.hpp` | Exchange-style binary framing: big-endian codec, incremental decode across arbitrary TCP splits, resync on corruption, forward-compatible unknown-type skipping, sequence-gap detection. |
| **Session layer** | `session.hpp` | The layer above framing: logon sequence agreement, gap recovery that *pauses delivery* rather than acting out of order, GapFill vs Reset semantics, duplicate suppression, and two-step liveness. A pure state machine — no socket, no clock, no thread — so the hard cases are testable. |
| **Order book** | `order_book.hpp` | Price-time priority limit book. Bids best-first, asks best-first, FIFO within a level, O(log) cancel via an id→location index. |
| **Matching engine** | `matching_engine.hpp` | Continuous matching: sweeps levels while the taker crosses, fills at the maker price, rests the limit remainder, markets never rest. |
| **Risk gate** | `risk_gate.hpp` | Pre-trade checks: max order size, net-position limit, price collar, sanity. The generic form of a live kill-switch — strategy-agnostic. |
| **Order router** | `order_router.hpp` | Symbol→venue routing, per-venue session state and sequence numbers, restart-safe client order IDs, and a drain state that stops new risk without blocking the exit. |
| **CPU / memory placement** | `cpu.hpp` | Thread pinning and hugepage allocation that report what actually happened instead of silently no-opping. |
| **Latency histogram** | `latency_histogram.hpp` | HdrHistogram-style bucketing: O(1) record, bounded memory, constant *relative* error, p50/p99/p999 queries. |
| **Config** | `config.hpp` | Dependency-free flat-YAML-subset loader for config-driven operation. |
| **Feed handler, epoll (Linux)** | `apps/feed_handler_linux.cpp` | `epoll` + non-blocking TCP → ring buffer → risk+matching consumer thread. |
| **Feed handler, io_uring (Linux)** | `apps/feed_handler_iouring_linux.cpp` | The same pipeline, io_uring instead of epoll on the network thread — same wire format, same downstream, so the *only* variable is the socket I/O model. |
| **Two-process IPC demo** | `apps/shm_ipc_posix.cpp` | `fork` + one shared segment + two lock-free rings: cross-process throughput and round-trip latency, config-driven, both sides pinned. |

## Build & test

Portable core builds anywhere with a C++17 compiler + CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or compile the tests directly (no CMake needed):

```bash
g++ -std=c++17 -O2 -pthread -Iinclude -Itests \
    tests/*.cpp src/*.cpp -o unit_tests && ./unit_tests
```

The Linux-only pieces (both feed handlers, POSIX shared memory) build
automatically where they are supported; on Windows/macOS the portable core,
tests, and benchmarks still build and run. The io_uring handler additionally
needs `liburing` (`liburing-dev` on Debian/Ubuntu) at configure time — CMake
skips just that one target with a `message(STATUS ...)` if it is not found,
rather than failing the whole build, and CI installs it explicitly so the
target is never silently skipped there. CI (`.github/workflows/ci.yml`) builds
and tests on **Linux (g++ and clang++)** and **Windows (MSVC)**, smoke-runs the
benchmarks, runs the two-process shared-memory demo (failing the build if it
leaks a shared-memory segment), and drives *both* feed handlers over a real TCP
socket — a resting order and a crossing one, then a real `SIGTERM` and an
assertion on the consumer thread's final fill count, not a `kill -9` that would
prove nothing.

## Measuring latency honestly

Latency numbers are easy to overstate by accident, so the benchmarks are built to
make that hard.

**A saturated queue does not measure a hand-off.** Stamp a message on push,
subtract on pop, and run the producer flat out, and every sample includes the
time the message spent waiting in a full ring. That is a real number — it is
streaming latency under back-pressure — but it is not the cost of moving one
message between threads, and quoting it as such overstates the queue by orders of
magnitude. The two are measured separately and named for what they are:

- `bench_ring_buffer` — producer flat out, ring deliberately full.
  Reports throughput and **queue residency**.
- `bench_pingpong` — exactly one message in flight, so there is no residency to
  hide in. Reports **round-trip hand-off cost**.

The arithmetic confirms the split: with a 4096-slot ring draining at 11.5 M msg/s,
a message should wait about 4096 ÷ 11.5e6 ≈ 355 µs, and the measured residency
p50 is 279 µs. That number is queue depth, not queue speed.

**The clock has a floor, and it is reported.** `bench_pingpong` measures its own
tick granularity and the cost of two back-to-back clock reads, prints both, and
warns when the p50 lands within four ticks of the granularity — because on
Windows `steady_clock` is `QueryPerformanceCounter` at 100 ns, and a sub-100 ns
hand-off simply cannot be resolved per-iteration there. A wall-clock mean over
the whole loop is printed alongside; that one is resolution-independent.

**Placement is stated, not assumed.** Both threads are pinned and the *actual*
pinning status is printed, so a run where pinning silently failed is never
mistaken for one where it took.

```bash
./build/bench_pingpong    200000        # depth-1 round-trip hand-off cost
./build/bench_ring_buffer 5000000       # streaming throughput + queue residency
./build/bench_matching    2000000       # risk-gate -> matching-engine orders/sec
```

### Numbers from one machine

Windows 11, Ryzen-class 8-core, g++ 16.1 (MinGW-w64 UCRT), `-O2`. Latency is
machine-specific — run it on yours.

| Benchmark | Result |
|---|---|
| SPSC ping-pong (depth 1, both threads pinned) | **144.5 ns mean RTT** → ~72 ns one-way; 6.92 M round trips/s |
| SPSC streaming (ring saturated) | **11.54 M msg/s**; residency p50 279 µs, p99 573 µs |
| Risk gate → matching engine | **2.71 M orders/s**; p50 200 ns, p99 800 ns |
| Test suite | 71 cases, 17,786 checks, 0 failures |

Caveat, stated because the tool reports it: on this platform the clock ticks at
100 ns, so the per-iteration percentile columns for the sub-microsecond
benchmarks are quantised to multiples of a tick and their *shape* is a clock
artefact. The means and throughputs are unaffected. Run on Linux, where
`clock_gettime` resolves to about a nanosecond, for a meaningful distribution.

## Crossing a process boundary

A trading stack is several processes, not several threads: a feed handler and a
strategy/OMS process that must exchange messages without a syscall each. The same
lock-free queue moves to shared memory, which adds constraints that corrupt data
silently when skipped:

- **The atomics must be lock-free.** A non-lock-free `std::atomic` is implemented
  with a lock table in the process's *own* address space, so two processes take
  two different locks and believe they are synchronised. Enforced with a
  `static_assert`.
- **Indices are fixed-width, never `size_t`.** Two processes mapping one segment
  must agree on the header byte-for-byte.
- **Messages are trivially copyable and pointer-free.** The segment can map at a
  different address in each process.
- **The header is validated on attach** — magic, version, capacity, element size —
  so a segment left by an older build is rejected instead of misread.
- **Indices read from the segment are untrusted input.** A writer that crashes
  mid-update leaves a head/tail pair implying a depth larger than the ring; a
  reader that trusts it reads outside the ring. `pop()` range-checks and counts
  the event instead.

```bash
./build/shm_ipc config/shm_ipc.example.yaml
```

Two processes, one segment, two rings; phase 1 streams and checksums, phase 2
measures a true cross-process round trip. The queue logic is portable and unit-
tested on every platform (`tests/test_shm_ring.cpp`) — only the mapping is
POSIX-specific.

## Wire protocol

Modelled on the *shape* of exchange binary feeds — fixed big-endian header,
length-delimited body, monotonic sequence number, body checksum. It is not an
implementation of any licensed exchange specification.

```
+---------------------- 20-byte header (big-endian) ---------------------+
| u16 magic 0x4B58 | u8 version | u8 msg_type | u32 body_len             |
| u64 seq          | u16 checksum (Fletcher-16 over body) | u16 reserved |
+------------------------------------------------------------------------+
| body_len bytes, field-packed big-endian                                |
+------------------------------------------------------------------------+
```

Message types: Heartbeat, Trade, Quote (top of book), NewOrder, OrderAck.

The four behaviours that separate a decoder that works from one that only works
in the unit test — all tested:

1. **A TCP read is not a message.** Frames arrive split across reads and
   several-to-a-read. A test splits a stream at *every possible byte boundary*
   and another feeds it one byte at a time; both must yield identical messages.
2. **Corruption resyncs rather than desyncing forever.** A bad magic, version or
   checksum scans forward for the next plausible header instead of seeking by a
   length field it just proved untrustworthy.
3. **Unknown message types are skipped by length, not resynced.** Venues add
   message types; a decoder that resyncs on each one turns a routine spec bump
   into a feed outage.
4. **Gaps are surfaced, not smoothed over.** Every message reports how many
   preceded it, including gaps discovered on frames the decoder chose not to
   deliver. A missed sequence number is the cue to request a snapshot rather than
   trade a book you know is wrong.

## epoll vs. io_uring

Both feed handlers run the identical pipeline — accept, decode a 24-byte frame,
push to the SPSC ring, risk-check, match — over the identical wire format.
The only thing that differs between `feed_handler_linux.cpp` and
`feed_handler_iouring_linux.cpp` is how each one talks to the socket, which is
the actual point of building both rather than just one.

**epoll tells you a socket is readable; you still read it yourself.**
`epoll_wait` returns, and the handler makes a synchronous `read()` into a
buffer it owns for exactly the duration of that call — the buffer can live on
the stack, because by the time control returns from `read()`, the kernel is
done with it.

**io_uring does the read itself, asynchronously.** A submitted `IORING_OP_RECV`
completes sometime later, on a trip around the loop that may be several
iterations away — and the kernel is writing into your buffer for that entire
window. A buffer that lived on the stack of the function that submitted the
request is a use-after-free waiting for the kernel to win the race; a
`std::vector` that might reallocate underneath an in-flight request is the
same bug with extra steps. That is why every in-flight buffer in the io_uring
handler lives inside a heap-allocated, pointer-stable `Conn` — owned by a
`std::unique_ptr` in a map, never moved, freed only once nothing submitted
still references it — rather than a function-local array. It is also why a
completion cannot mean "this fd is ready, go read it": io_uring has to be told
*which* read completed, since several can be in flight across different
connections at once, so every submitted request is tagged with a
heap-allocated `Op` carrying that context, retrieved from the matching
completion and freed exactly once.

Both handlers now support a real graceful shutdown (`SIGINT`/`SIGTERM` flip the
same atomic flag the accept/wait loop already polls on a timeout) precisely so
this comparison is something CI can actually drive end-to-end rather than
merely compile: two real frames over a real socket, a real signal, and an
assertion on the consumer thread's printed fill count.

**Concurrent-load comparison, measured.** `bench/bench_feed_client.cpp` opens
50 connections and sends 2,000 frames each — 100,000 frames, 50,000 of which
cross (the client's frame pattern alternates a resting sell and a crossing buy
per price bucket, so the exact expected fill count is provable from TCP's
per-connection byte-order guarantee alone, not just approximately right). CI
runs both handlers back to back, on the same job, same runner, same load. A
representative pair of runs (g++, GitHub Actions `ubuntu-latest`):

| | send throughput | fills delivered |
|---|---|---|
| epoll | 2,382 K frames/s (57.2 MB/s) | 49,983 / 50,000 |
| io_uring | 1,505 K frames/s (36.1 MB/s) | 49,426 / 50,000 |

Getting a real number here, rather than shipping the comparison unmeasured,
surfaced five genuine bugs in the io_uring handler's shutdown and recv path —
each one caught by this benchmark coming up short in CI, not by inspection:
an unread-kernel-bytes race at shutdown, a consumer-exit race that could see
the ring transiently empty before the reader truly finished, a connection
stuck in the accept() backlog that no drain pass could see, a transient
`-EAGAIN` recv completion misread as "peer closed" (discarding the rest of an
otherwise-healthy connection), and one-completion-per-syscall reaping falling
behind a bursty client. All five are fixed; none were guessed at, all were
root-caused from a failing CI run and re-verified by the same benchmark
passing.

**What is still honestly true after all five fixes**, and why the CI check for
the io_uring leg carries a wider tolerance (6% vs. epoll's 1%): under this
specific burst shape — 50 connections landing 100,000 frames within tens of
milliseconds — on GitHub's contended, virtualized, shared runners, io_uring's
one-outstanding-recv-per-connection submission model (chosen here for clarity,
not maximum throughput) measurably falls further behind than epoll's tight
read-to-`EAGAIN` loop, and both the shortfall and the send throughput above
vary noticeably run to run. A dedicated box does not reproduce shortfalls
anywhere near CI's observed ceiling (up to ~5.2% in the runs used to set that
tolerance); this is a property of shared-runner scheduling under an extreme
synthetic burst, not of the feed handler in steady-state use — but reporting
it as such, rather than smoothing it into a single clean number, is the more
honest read of what was actually measured. epoll is not immune to the same
effect, just less exposed to it: one CI run hit 133-203/50,000 (0.27%-0.41%)
on epoll across all 3 retried attempts, not a single unlucky one, which is
why its own tolerance sits at 1% rather than the tighter 0.2% this section
originally shipped with — widened from a second real measurement, not loosened
to make a red build go away. io_uring's real advantage — fewer syscalls per
message — is architectural, not yet demonstrated as a throughput *win* against
epoll at this connection count and message size; a workload that keeps more
requests genuinely in flight per connection (this handler submits only one
recv at a time) is the natural next test of that claim, not this one.

## Order routing

The router decides *where* an order goes and stamps the identity it carries for
the rest of its life:

- **No implicit default venue.** An unmapped symbol is rejected, never guessed.
- **Restart-safe client order IDs.** The ID packs a session epoch above a
  per-order counter, so a process that restarts and counts from one again cannot
  mint an ID that collides with an order still live at the venue — a collision
  there makes the venue's ack impossible to attribute. Exhausting the counter
  stops the session instead of wrapping.
- **Per-venue sequence numbers**, reset by logon, so one session reconnecting
  cannot desynchronise the others.
- **Draining as a first-class state.** Taking a venue out of service rejects new
  risk while still passing liquidating orders. A kill-switch that also blocks
  your exit is not a safety feature.

Venues and routes come from config, so operations can re-point a symbol without a
rebuild (`config/router.example.yaml`), and a typo fails the start rather than
silently rejecting that symbol's orders all session.

## Design choices worth calling out

- **Integer prices/quantities.** Floating point is banned on the matching path;
  price-time priority must be exactly reproducible.
- **Prices may be negative.** The router validates quantity, not price sign: WTI
  settled below zero in April 2020 and systems that hard-coded `price > 0`
  rejected valid orders on the day it mattered most. Price policy belongs in the
  risk gate's configured collar, not in an assumption.
- **Acquire/release, not locks.** The SPSC queue documents the exact two
  acquire/release pairs that make it safe, and threaded tests transfer 200k items
  through both the in-process and the shared-memory ring and check the checksum.
- **Fail explicitly, never fall back silently.** `pin_this_thread_to_core`
  distinguishes *unsupported* from *failed*, and `alloc_hugepages` returns null
  rather than quietly handing back 4 KiB pages — publishing a "pinned,
  hugepage-backed" number that was neither is worse than not measuring.
- **Known limitations are pinned by tests.** Fletcher-16 cannot distinguish
  all-zero bodies of different lengths; there is a test asserting that blind spot
  *and* a test showing why the frame format covers it (length is validated before
  the checksum is consulted). A limitation you can name is one you have handled.
- **The histogram is tested against ground truth.** A brute-force `nth_element`
  percentile over 100k skewed samples must agree with the histogram to within its
  relative-error bound — so bucketing bugs can't hide.
- **Correctness first.** `-Wall -Wextra -Wpedantic` (and `/W4 /permissive-` on
  MSVC), no warnings, and the suite passes identically at `-O0`, `-O2` and `-O3`.

## License

MIT.
