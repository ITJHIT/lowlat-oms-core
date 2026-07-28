# lowlat-oms-core

**A low-latency order-matching / OMS core in modern C++17.**

The building blocks a real-time trading/OMS platform is made of, written to be
correct, tested, and fast on the hot path — a lock-free hand-off, a
price-time-priority matching engine, a pre-trade risk gate, and nanosecond
latency measurement, wired together by a Linux `epoll` feed handler.

Everything is integer-priced and allocation-free on the matching path. No alpha,
no strategy secrets — this is the **infrastructure** layer, which is exactly the
part that generalizes across firms.

---

## Components

| Component | Header | What it demonstrates |
|---|---|---|
| **SPSC ring buffer** | `spsc_ring_buffer.hpp` | Lock-free single-producer/single-consumer queue: `std::atomic` acquire/release, power-of-two masking, cache-line padding to kill false sharing. Template + RAII. |
| **Order book** | `order_book.hpp` | Price-time priority limit book. Bids best-first, asks best-first, FIFO within a level, O(log) cancel via an id→location index. |
| **Matching engine** | `matching_engine.hpp` | Continuous matching: sweeps levels while the taker crosses, fills at the maker price, rests the limit remainder, markets never rest. |
| **Risk gate** | `risk_gate.hpp` | Pre-trade checks: max order size, net-position limit, price collar, sanity. The generic form of a live kill-switch — strategy-agnostic. |
| **Latency histogram** | `latency_histogram.hpp` | HdrHistogram-style bucketing: O(1) record, bounded memory, constant *relative* error, p50/p99/p999 queries. |
| **Config** | `config.hpp` | Dependency-free flat-YAML-subset loader for config-driven operation. |
| **Feed handler (Linux)** | `apps/feed_handler_linux.cpp` | `epoll` + non-blocking TCP → ring buffer → risk+matching consumer thread. |

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

The Linux feed handler is built automatically on Linux (`epoll`); on Windows/macOS
the portable core, tests, and benchmarks still build and run. CI
(`.github/workflows/ci.yml`) builds and tests on **Linux (g++ and clang++)** and
**Windows (MSVC)**, and smoke-runs the benchmarks.

## Benchmarks

```bash
./build/bench_ring_buffer 5000000    # producer/consumer hand-off latency + throughput
./build/bench_matching   2000000    # risk-gate -> matching-engine orders/sec + latency
```

Each prints throughput and a p50/p99/p999 latency profile from the built-in
histogram — the numbers depend on the machine, so run it on yours.

## Design choices worth calling out

- **Integer prices/quantities.** Floating point is banned on the matching path;
  price-time priority must be exactly reproducible.
- **Acquire/release, not locks.** The SPSC queue documents the exact two
  acquire/release pairs that make it safe, and a threaded test transfers 200k
  items and checks the checksum.
- **The histogram is tested against ground truth.** A brute-force `nth_element`
  percentile over 100k skewed samples must agree with the histogram to within
  its relative-error bound — so bucketing bugs can't hide.
- **Correctness first.** `-Wall -Wextra -Wpedantic` (and `/W4 /permissive-` on
  MSVC), no warnings.

## Wire protocol (feed handler)

Little-endian, 24-byte packed frame:

```
uint32 symbol | uint8 side(0=buy,1=sell) | uint8 type(0=limit,1=market)
uint16 pad    | int64 price              | int64 qty
```

## License

MIT.
