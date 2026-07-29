// A lock-free SPSC ring buffer that lives in memory two *processes* share.
//
// The queue logic is the same as spsc_ring_buffer.hpp. Crossing a process
// boundary is what adds the interesting constraints, and each one below is a way
// this silently corrupts data if you skip it:
//
//  1. **The atomics must be lock-free.** A std::atomic that is not lock-free is
//     implemented with a lock table in the process's own address space. Two
//     processes then take two different locks and believe they are synchronised.
//     Enforced with a static_assert, not a comment.
//  2. **Indices are fixed-width, never size_t.** Two processes mapping one
//     segment must agree on the header byte-for-byte; size_t does not if their
//     word sizes differ.
//  3. **T must be trivially copyable and pointer-free.** The segment can map at
//     a different address in each process, so a pointer stored by the writer
//     means nothing to the reader. Trivial copyability is enforced; pointer-free
//     is a design rule stated here because the language cannot check it.
//  4. **The header is validated on attach.** A segment left behind by an older
//     build has a plausible-looking header; reading it as the current layout is
//     silent garbage. Magic, version, capacity and element size are all checked.
//  5. **Indices read from shared memory are untrusted input.** The writer is a
//     separate process that can crash mid-update or be replaced by a buggy
//     build. A reader that computes head - tail and indexes with it will happily
//     read outside the ring. pop() range-checks instead.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace lloms {

enum class ShmAttachStatus {
    Ok,
    TooSmall,      // mapping is shorter than the header + slots require
    Misaligned,    // base pointer is not suitably aligned for the atomics
    BadMagic,      // not one of our segments
    BadVersion,    // written by a different build of this header
    BadCapacity,   // zero, not a power of two, or inconsistent with the mapping
    BadElemSize,   // writer and reader disagree on the message type
};

const char* to_string(ShmAttachStatus s);

// Fixed on-segment layout. Both processes compile against this, so it must not
// depend on anything platform- or build-specific.
struct alignas(64) ShmRingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t capacity;   // power of two
    std::uint32_t elem_size;
    std::uint64_t reserved[6];

    // Producer and consumer indices on separate cache lines: false sharing is
    // worse across processes than within one, since the line ping-pongs between
    // cores that share nothing else.
    alignas(64) std::atomic<std::uint64_t> head;
    alignas(64) std::atomic<std::uint64_t> tail;
};

static_assert(sizeof(ShmRingHeader) == 192, "shared-memory header layout must be fixed");
static_assert(alignof(ShmRingHeader) == 64, "header must be cache-line aligned");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "a non-lock-free atomic would use a process-local lock and silently "
              "fail to synchronise two processes");

template <typename T>
class ShmSpscRing {
    static_assert(std::is_trivially_copyable<T>::value,
                  "shared-memory messages must be trivially copyable -- no vtables, "
                  "no heap-owning members");

  public:
    static constexpr std::uint32_t kMagic = 0x534D5231u;  // 'SMR1'
    static constexpr std::uint32_t kVersion = 1u;

    // Bytes a segment must be for this capacity. Call before creating the file.
    static std::size_t bytes_needed(std::uint32_t capacity) {
        return sizeof(ShmRingHeader) + static_cast<std::size_t>(capacity) * sizeof(T);
    }

    ShmSpscRing() = default;

    // Writer side, once per segment: stamp the header and zero the indices.
    static ShmAttachStatus create(void* mem, std::size_t bytes, std::uint32_t capacity,
                                  ShmSpscRing& out) {
        const ShmAttachStatus pre = precheck(mem, bytes, capacity);
        if (pre != ShmAttachStatus::Ok) {
            return pre;
        }
        auto* h = static_cast<ShmRingHeader*>(mem);
        h->capacity = capacity;
        h->elem_size = static_cast<std::uint32_t>(sizeof(T));
        h->version = kVersion;
        h->head.store(0, std::memory_order_relaxed);
        h->tail.store(0, std::memory_order_relaxed);
        for (auto& r : h->reserved) {
            r = 0;
        }
        // Magic last, with a release: a reader that sees the magic is guaranteed
        // to see a fully initialised header behind it.
        std::atomic_thread_fence(std::memory_order_release);
        h->magic = kMagic;

        out.bind(h);
        return ShmAttachStatus::Ok;
    }

    // Reader side (or a writer re-attaching): validate before trusting anything.
    static ShmAttachStatus attach(void* mem, std::size_t bytes, ShmSpscRing& out) {
        if (mem == nullptr) {
            return ShmAttachStatus::TooSmall;
        }
        if (reinterpret_cast<std::uintptr_t>(mem) % alignof(ShmRingHeader) != 0) {
            return ShmAttachStatus::Misaligned;
        }
        if (bytes < sizeof(ShmRingHeader)) {
            return ShmAttachStatus::TooSmall;
        }
        auto* h = static_cast<ShmRingHeader*>(mem);
        if (h->magic != kMagic) {
            return ShmAttachStatus::BadMagic;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h->version != kVersion) {
            return ShmAttachStatus::BadVersion;
        }
        if (h->elem_size != sizeof(T)) {
            return ShmAttachStatus::BadElemSize;
        }
        const std::uint32_t cap = h->capacity;
        if (cap < 2 || (cap & (cap - 1)) != 0) {
            return ShmAttachStatus::BadCapacity;
        }
        if (bytes < bytes_needed(cap)) {
            return ShmAttachStatus::TooSmall;
        }
        out.bind(h);
        return ShmAttachStatus::Ok;
    }

    bool valid() const { return hdr_ != nullptr; }
    std::uint32_t capacity() const { return hdr_ ? hdr_->capacity : 0; }

    // Times pop() refused because the shared indices were inconsistent. Counted
    // per process, in private memory -- a corrupt segment must not be able to
    // rewrite the evidence that it is corrupt.
    std::uint64_t corruption_events() const { return corruption_; }

    bool push(const T& value) {
        const std::uint64_t head = hdr_->head.load(std::memory_order_relaxed);
        const std::uint64_t tail = hdr_->tail.load(std::memory_order_acquire);
        if (head - tail >= hdr_->capacity) {
            return false;  // full
        }
        std::memcpy(slot(head), &value, sizeof(T));
        hdr_->head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::uint64_t tail = hdr_->tail.load(std::memory_order_relaxed);
        const std::uint64_t head = hdr_->head.load(std::memory_order_acquire);
        if (head == tail) {
            return false;  // empty
        }
        if (head - tail > hdr_->capacity) {
            // Only reachable if the other process wrote a nonsense index --
            // crashed mid-update, or a build with a different layout. Refuse and
            // count it; do not index the ring with a number we just disproved.
            ++corruption_;
            return false;
        }
        std::memcpy(&out, slot(tail), sizeof(T));
        hdr_->tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate depth; both indices move under you in a live system.
    std::uint64_t size() const {
        const std::uint64_t head = hdr_->head.load(std::memory_order_acquire);
        const std::uint64_t tail = hdr_->tail.load(std::memory_order_acquire);
        return head - tail;
    }

    bool empty() const { return size() == 0; }

  private:
    void bind(ShmRingHeader* h) {
        hdr_ = h;
        slots_ = reinterpret_cast<unsigned char*>(h) + sizeof(ShmRingHeader);
        mask_ = h->capacity - 1;
    }

    static ShmAttachStatus precheck(void* mem, std::size_t bytes, std::uint32_t capacity) {
        if (mem == nullptr) {
            return ShmAttachStatus::TooSmall;
        }
        if (reinterpret_cast<std::uintptr_t>(mem) % alignof(ShmRingHeader) != 0) {
            return ShmAttachStatus::Misaligned;
        }
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            return ShmAttachStatus::BadCapacity;
        }
        if (bytes < bytes_needed(capacity)) {
            return ShmAttachStatus::TooSmall;
        }
        return ShmAttachStatus::Ok;
    }

    unsigned char* slot(std::uint64_t index) const {
        return slots_ + static_cast<std::size_t>(index & mask_) * sizeof(T);
    }

    ShmRingHeader* hdr_{nullptr};
    unsigned char* slots_{nullptr};
    std::uint64_t mask_{0};
    std::uint64_t corruption_{0};
};

}  // namespace lloms
