// Shared-memory ring tests.
//
// These run everywhere, including Windows, because the queue logic is deliberately
// independent of how the memory got shared: the tests back the segment with an
// aligned heap block instead of shm_open. That is the same code path the two
// processes in apps/shm_ipc_posix.cpp execute -- only the mapping differs.
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "lloms/shm_ring.hpp"
#include "microtest.hpp"

using namespace lloms;

namespace {

struct Msg {
    std::uint64_t seq;
    std::uint64_t payload;
};

struct Small {
    std::uint32_t x;
};

// Stands in for the mapping: a heap block, hand-aligned to the header's
// alignment the way mmap would already have given us.
struct Segment {
    std::vector<unsigned char> raw;
    void* base;
    std::size_t bytes;

    explicit Segment(std::size_t n) : raw(n + 64, 0), bytes(n) {
        const auto addr = reinterpret_cast<std::uintptr_t>(raw.data());
        const auto aligned = (addr + 63u) & ~static_cast<std::uintptr_t>(63u);
        base = reinterpret_cast<void*>(aligned);
    }

    ShmRingHeader* header() { return static_cast<ShmRingHeader*>(base); }
};

}  // namespace

TEST_CASE("segment sizing accounts for the header plus every slot") {
    CHECK_EQ(ShmSpscRing<Msg>::bytes_needed(8), sizeof(ShmRingHeader) + 8 * sizeof(Msg));
    CHECK_EQ(ShmSpscRing<Small>::bytes_needed(1024),
             sizeof(ShmRingHeader) + 1024 * sizeof(Small));
    CHECK_EQ(sizeof(ShmRingHeader), 192u);  // layout is contractual, not incidental
}

TEST_CASE("messages come out in order and the ring reports empty when drained") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(8));
    ShmSpscRing<Msg> w;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 8, w) == ShmAttachStatus::Ok);

    CHECK(w.empty());
    for (std::uint64_t i = 0; i < 5; ++i) {
        CHECK(w.push(Msg{i, i * 10}));
    }
    CHECK_EQ(w.size(), 5u);

    Msg m{};
    for (std::uint64_t i = 0; i < 5; ++i) {
        REQUIRE(w.pop(m));
        CHECK_EQ(m.seq, i);
        CHECK_EQ(m.payload, i * 10);
    }
    CHECK(w.empty());
    CHECK(!w.pop(m));
}

TEST_CASE("the ring refuses to overwrite unread data when full") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(4));
    ShmSpscRing<Msg> w;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 4, w) == ShmAttachStatus::Ok);

    for (std::uint64_t i = 0; i < 4; ++i) {
        CHECK(w.push(Msg{i, 0}));
    }
    CHECK(!w.push(Msg{99, 0}));  // full: back-pressure, not corruption
    CHECK_EQ(w.size(), 4u);

    Msg m{};
    REQUIRE(w.pop(m));
    CHECK_EQ(m.seq, 0u);       // oldest survives
    CHECK(w.push(Msg{99, 0}));  // one slot freed, one accepted
}

TEST_CASE("indices wrap many times over without losing order") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(4));
    ShmSpscRing<Msg> w;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 4, w) == ShmAttachStatus::Ok);

    Msg m{};
    for (std::uint64_t i = 0; i < 1000; ++i) {
        REQUIRE(w.push(Msg{i, ~i}));
        REQUIRE(w.pop(m));
        CHECK_EQ(m.seq, i);
        CHECK_EQ(m.payload, ~i);
    }
    CHECK(w.empty());
}

TEST_CASE("create rejects a geometry it cannot honour instead of half-initialising") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(8));
    ShmSpscRing<Msg> w;

    CHECK(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 6, w) == ShmAttachStatus::BadCapacity);
    CHECK(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 0, w) == ShmAttachStatus::BadCapacity);
    CHECK(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 1, w) == ShmAttachStatus::BadCapacity);
    CHECK(ShmSpscRing<Msg>::create(seg.base, 32, 8, w) == ShmAttachStatus::TooSmall);
    CHECK(ShmSpscRing<Msg>::create(nullptr, seg.bytes, 8, w) == ShmAttachStatus::TooSmall);

    // A base pointer the atomics are not aligned for.
    auto* off_by_one = static_cast<unsigned char*>(seg.base) + 1;
    CHECK(ShmSpscRing<Msg>::create(off_by_one, seg.bytes - 1, 8, w) ==
          ShmAttachStatus::Misaligned);

    CHECK(!w.valid());  // nothing bound by any of the rejections
}

TEST_CASE("a second process attaching sees the writer's ring") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(16));
    ShmSpscRing<Msg> writer;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 16, writer) == ShmAttachStatus::Ok);
    CHECK(writer.push(Msg{7, 77}));

    // What the reader process does: map the same bytes, validate, then read.
    ShmSpscRing<Msg> reader;
    REQUIRE(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, reader) == ShmAttachStatus::Ok);
    CHECK_EQ(reader.capacity(), 16u);

    Msg m{};
    REQUIRE(reader.pop(m));
    CHECK_EQ(m.seq, 7u);
    CHECK_EQ(m.payload, 77u);
    CHECK(writer.empty());  // both views agree
}

TEST_CASE("attach refuses a segment it cannot prove is ours") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(16));
    ShmSpscRing<Msg> ring;

    // Never initialised -- zeroed memory is not a valid header.
    CHECK(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, ring) == ShmAttachStatus::BadMagic);

    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 16, ring) == ShmAttachStatus::Ok);
    CHECK(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, ring) == ShmAttachStatus::Ok);

    // A leftover segment from a build with a different header layout.
    seg.header()->version = ShmSpscRing<Msg>::kVersion + 1;
    CHECK(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, ring) == ShmAttachStatus::BadVersion);
    seg.header()->version = ShmSpscRing<Msg>::kVersion;

    // Writer and reader disagree about the message type. Without this check the
    // reader would decode every message at the wrong stride.
    ShmSpscRing<Small> wrong_type;
    CHECK(ShmSpscRing<Small>::attach(seg.base, seg.bytes, wrong_type) ==
          ShmAttachStatus::BadElemSize);

    // Mapping shorter than the header says the ring is.
    CHECK(ShmSpscRing<Msg>::attach(seg.base, sizeof(ShmRingHeader) + 8, ring) ==
          ShmAttachStatus::TooSmall);
    CHECK(ShmSpscRing<Msg>::attach(seg.base, 16, ring) == ShmAttachStatus::TooSmall);

    seg.header()->capacity = 17;  // not a power of two
    CHECK(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, ring) == ShmAttachStatus::BadCapacity);
}

TEST_CASE("a nonsense index left by a dead writer is refused, not indexed with") {
    Segment seg(ShmSpscRing<Msg>::bytes_needed(8));
    ShmSpscRing<Msg> ring;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 8, ring) == ShmAttachStatus::Ok);
    CHECK(ring.push(Msg{1, 1}));

    // Simulate the writer process dying mid-update, or a foreign build stomping
    // the segment: head races far past tail. A reader that trusted head - tail
    // would compute a depth larger than the ring and read outside it.
    seg.header()->head.store(seg.header()->tail.load() + 8 + 5);

    Msg m{};
    CHECK(!ring.pop(m));
    CHECK_EQ(ring.corruption_events(), 1u);

    // Recovering the segment restores normal service; the evidence is kept.
    seg.header()->head.store(seg.header()->tail.load() + 1);
    CHECK(ring.pop(m));
    CHECK_EQ(ring.corruption_events(), 1u);
}

TEST_CASE("a producer and a consumer transfer 200k messages across the segment intact") {
    constexpr std::uint64_t kCount = 200'000;
    Segment seg(ShmSpscRing<Msg>::bytes_needed(1024));

    ShmSpscRing<Msg> writer;
    REQUIRE(ShmSpscRing<Msg>::create(seg.base, seg.bytes, 1024, writer) == ShmAttachStatus::Ok);
    ShmSpscRing<Msg> reader;
    REQUIRE(ShmSpscRing<Msg>::attach(seg.base, seg.bytes, reader) == ShmAttachStatus::Ok);

    std::uint64_t sent_sum = 0;
    std::uint64_t recv_sum = 0;
    std::uint64_t out_of_order = 0;

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            const Msg m{i, i * 2654435761u};
            sent_sum += m.payload;
            while (!writer.push(m)) {
            }
        }
    });

    std::thread consumer([&] {
        Msg m{};
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!reader.pop(m)) {
            }
            if (m.seq != i) {
                ++out_of_order;
            }
            recv_sum += m.payload;
        }
    });

    producer.join();
    consumer.join();

    CHECK_EQ(out_of_order, 0u);
    CHECK_EQ(recv_sum, sent_sum);
    CHECK_EQ(reader.corruption_events(), 0u);
    CHECK(reader.empty());
}
