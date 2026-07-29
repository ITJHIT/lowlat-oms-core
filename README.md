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
| **Feed handler (Linux)** | `apps/feed_handler_linux.cpp` | `epoll` + non-blocking TCP → ring buffer → risk+matching consumer thread. |
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

The Linux-only pieces (`epoll` feed handler, POSIX shared memory) build
automatically where they are supported; on Windows/macOS the portable core,
tests, and benchmarks still build and run. CI (`.github/workflows/ci.yml`) builds
and tests on **Linux (g++ and clang++)** and **Windows (MSVC)**, smoke-runs the
benchmarks, runs the two-process shared-memory demo, and fails the build if that
demo leaks a shared-memory segment.

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
