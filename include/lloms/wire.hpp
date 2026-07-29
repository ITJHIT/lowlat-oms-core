// Exchange-style binary wire protocol: framing, encoding, and an incremental
// decoder that survives what a real TCP feed actually does to you.
//
// Scope note: the layout is modelled on the *shape* of exchange binary feeds --
// fixed big-endian header, length-delimited body, monotonic sequence number,
// body checksum. It is not an implementation of any licensed exchange
// specification, and no venue-specific field semantics are encoded here.
//
// Frame layout (all integers big-endian, i.e. network byte order):
//
//   +---------------------- 20-byte header ---------------------------------+
//   | 0  u16 magic = 0x4B58 | 2  u8 version | 3  u8 msg_type                 |
//   | 4  u32 body_len       | 8  u64 seq                                     |
//   | 16 u16 checksum (Fletcher-16 over body) | 18 u16 reserved              |
//   +-----------------------------------------------------------------------+
//   | body_len bytes, fields packed big-endian, no padding                   |
//   +-----------------------------------------------------------------------+
//
// The four things that separate a decoder that works from one that only works
// in the unit test, all handled here and all covered by tests:
//
//   1. **A TCP read is not a message.** Frames arrive split across reads and
//      several-to-a-read. The decoder is a state machine over a rolling buffer,
//      not a "parse this packet" function.
//   2. **Corruption must resync, not desync forever.** A bad magic/checksum
//      scans forward for the next plausible header instead of consuming the
//      stream by a length field it just proved untrustworthy.
//   3. **Unknown message types are skipped by length, not resynced.** Venues add
//      message types; a decoder that resyncs on every unknown type turns a
//      routine spec bump into a total feed outage.
//   4. **Gaps are surfaced, not smoothed over.** A missed sequence number is the
//      signal to request a snapshot; silently continuing means trading on a book
//      you know is wrong. Every message reports how many preceded it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lloms/order.hpp"

namespace lloms {
namespace wire {

// ---------------------------------------------------------------------------
// Byte order
//
// Done with shifts over an unsigned char buffer rather than a cast-and-bswap:
// this is correct on a host of either endianness, needs no alignment, and never
// forms a misaligned pointer or violates strict aliasing.
// ---------------------------------------------------------------------------

inline void store_be16(unsigned char* p, std::uint16_t v) {
    p[0] = static_cast<unsigned char>(v >> 8);
    p[1] = static_cast<unsigned char>(v);
}
inline void store_be32(unsigned char* p, std::uint32_t v) {
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
}
inline void store_be64(unsigned char* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<unsigned char>(v >> (56 - 8 * i));
    }
}
inline std::uint16_t load_be16(const unsigned char* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                      static_cast<std::uint16_t>(p[1]));
}
inline std::uint32_t load_be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline std::uint64_t load_be64(const unsigned char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

// Signed values travel as two's-complement bit patterns. memcpy is the
// conversion with no implementation-defined behaviour attached to it.
inline void store_be_i64(unsigned char* p, std::int64_t v) {
    std::uint64_t u;
    std::memcpy(&u, &v, sizeof(u));
    store_be64(p, u);
}
inline std::int64_t load_be_i64(const unsigned char* p) {
    const std::uint64_t u = load_be64(p);
    std::int64_t v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Frame constants and messages
// ---------------------------------------------------------------------------

constexpr std::uint16_t kMagic = 0x4B58;  // 'K','X'
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kDefaultMaxBody = 64u * 1024u;

enum class MsgType : std::uint8_t {
    Heartbeat = 0,
    Trade = 1,
    Quote = 2,
    NewOrder = 3,
    OrderAck = 4,
};

struct Trade {
    Symbol symbol{};
    Price price{};
    Qty qty{};
    std::int64_t ts_ns{};
};

// Top of book, both sides.
struct Quote {
    Symbol symbol{};
    Price bid_px{};
    Qty bid_qty{};
    Price ask_px{};
    Qty ask_qty{};
    std::int64_t ts_ns{};
};

struct NewOrder {
    std::uint64_t client_order_id{};
    Symbol symbol{};
    Side side{Side::Buy};
    Type type{Type::Limit};
    std::uint8_t tif{};  // 0 = day, 1 = IOC, 2 = FOK
    Price price{};
    Qty qty{};
};

enum class AckStatus : std::uint8_t { Accepted = 0, Rejected = 1, Filled = 2, Cancelled = 3 };

struct OrderAck {
    std::uint64_t client_order_id{};
    std::uint64_t exchange_order_id{};
    AckStatus status{AckStatus::Accepted};
    std::uint8_t reject_reason{};
    Qty filled_qty{};
    Price avg_px{};
};

// A decoded message. The body is a union of trivially-copyable structs -- the
// tagged-union form costs no allocation and no indirection on the hot path.
struct Message {
    MsgType type{MsgType::Heartbeat};
    std::uint64_t seq{0};
    // Messages missing immediately before this one (0 in a healthy stream).
    // Non-zero is the cue to fall back to a snapshot rather than trust the book.
    std::uint64_t gap_before{0};

    union Body {
        Trade trade;
        Quote quote;
        NewOrder new_order;
        OrderAck ack;
        Body() : trade{} {}
    } body{};
};

// Body size for a known type; 0 for Heartbeat. Returns false for unknown types,
// which are legal on the wire and must be skipped by body_len.
bool body_size_for(MsgType type, std::size_t& out);

// Fletcher-16 over the body. Chosen over a plain additive sum because the
// position-weighted second accumulator catches reordering and transposition,
// which a plain sum cannot see at all -- and misframing produces reordered
// bytes far more often than it produces random ones.
//
// Known blind spot, stated rather than papered over: with the standard 0xff
// initialisation, a body of all-zero bytes checksums to 0xFFFF regardless of its
// length. That is harmless *in this frame format* because length is carried in
// the header and validated against the message type before the checksum is ever
// consulted, so a length change is rejected one step earlier. It would not be
// harmless in a format that inferred length from the payload.
std::uint16_t fletcher16(const unsigned char* data, std::size_t len);

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Serialise `msg` (using `msg.seq`) into `out`. Returns bytes written, or 0 if
// the buffer is too small or the type is not encodable.
std::size_t encode(const Message& msg, unsigned char* out, std::size_t cap);

// Upper bound on a single encoded frame for the known message types.
constexpr std::size_t kMaxEncodedFrame = kHeaderSize + 64;

// ---------------------------------------------------------------------------
// Incremental decoding
// ---------------------------------------------------------------------------

class FrameDecoder {
  public:
    struct Stats {
        std::uint64_t frames_decoded{0};
        std::uint64_t bytes_consumed{0};
        std::uint64_t resyncs{0};          // corrupt-header recovery events
        std::uint64_t bytes_discarded{0};  // bytes thrown away while resyncing
        std::uint64_t bad_magic{0};
        std::uint64_t bad_version{0};
        std::uint64_t bad_length{0};    // body_len implausible or wrong for type
        std::uint64_t bad_checksum{0};
        std::uint64_t unknown_type{0};  // valid frame, type we do not decode
        std::uint64_t seq_gaps{0};      // gap events
        std::uint64_t seq_missing{0};   // total messages missing across gaps
        std::uint64_t seq_regressions{0};  // seq <= last seen: duplicate or replay
    };

    explicit FrameDecoder(std::size_t max_body = kDefaultMaxBody);

    // Append raw bytes as they come off the socket. Any split is legal,
    // including one byte at a time.
    void feed(const unsigned char* data, std::size_t len);

    // Pop the next decoded message. Returns false when more bytes are needed.
    //
    // Recoverable stream damage (bad magic, bad checksum, bad length, unknown
    // type) is handled inside and counted in stats() rather than surfaced as an
    // error per call: the caller's job is to consume messages, and a feed
    // handler that has to branch on every corruption mode inline gets it wrong.
    // Watch stats() -- and Message::gap_before -- for stream health.
    bool next(Message& out);

    const Stats& stats() const { return stats_; }

    // Next sequence number expected. 0 before the first frame is seen.
    std::uint64_t expected_seq() const { return have_seq_ ? next_seq_ : 0; }

    // Bytes held that do not yet form a complete frame.
    std::size_t buffered() const { return buf_.size() - rpos_; }

    void reset();

  private:
    // Scan forward for the next plausible magic. Returns true if one was found.
    bool resync();
    void compact();

    std::vector<unsigned char> buf_;
    std::size_t rpos_{0};
    std::size_t max_body_;
    std::uint64_t next_seq_{0};
    // A gap discovered on a frame we do not deliver (unknown type) must still
    // reach the caller, so it is carried to the next message we do deliver.
    std::uint64_t pending_gap_{0};
    bool have_seq_{false};
    Stats stats_{};
};

}  // namespace wire
}  // namespace lloms
