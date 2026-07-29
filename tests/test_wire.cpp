// Wire-protocol tests. The interesting cases are not "does a good frame decode"
// -- they are the ones a live feed produces and a lab test never does: frames
// split at every possible byte boundary, garbage in the stream, bit flips,
// message types from a newer spec, and dropped sequence numbers.
#include <cstring>
#include <string>
#include <vector>

#include "lloms/wire.hpp"
#include "microtest.hpp"

using namespace lloms;
using namespace lloms::wire;

namespace {

std::vector<unsigned char> encode_all(const std::vector<Message>& msgs) {
    std::vector<unsigned char> out;
    unsigned char tmp[kMaxEncodedFrame];
    for (const auto& m : msgs) {
        const std::size_t n = encode(m, tmp, sizeof(tmp));
        out.insert(out.end(), tmp, tmp + n);
    }
    return out;
}

std::vector<Message> drain(FrameDecoder& d) {
    std::vector<Message> got;
    Message m;
    while (d.next(m)) {
        got.push_back(m);
    }
    return got;
}

Message make_trade(std::uint64_t seq, Symbol sym, Price px, Qty qty) {
    Message m;
    m.type = MsgType::Trade;
    m.seq = seq;
    m.body.trade = Trade{sym, px, qty, 1'700'000'000'123'456'789LL};
    return m;
}

Message make_quote(std::uint64_t seq, Symbol sym) {
    Message m;
    m.type = MsgType::Quote;
    m.seq = seq;
    m.body.quote = Quote{sym, 71000, 12, 71100, 34, -5};
    return m;
}

Message make_new_order(std::uint64_t seq) {
    Message m;
    m.type = MsgType::NewOrder;
    m.seq = seq;
    m.body.new_order = NewOrder{0xDEADBEEFCAFEF00DULL, 42, Side::Sell, Type::Market, 1, -123, 999};
    return m;
}

Message make_ack(std::uint64_t seq) {
    Message m;
    m.type = MsgType::OrderAck;
    m.seq = seq;
    m.body.ack = OrderAck{7, 8, AckStatus::Filled, 3, 500, -64000};
    return m;
}

Message make_heartbeat(std::uint64_t seq) {
    Message m;
    m.type = MsgType::Heartbeat;
    m.seq = seq;
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Byte order
// ---------------------------------------------------------------------------

TEST_CASE("big-endian helpers round trip, including the signed extremes") {
    unsigned char b[8];

    store_be16(b, 0x1234);
    CHECK_EQ(static_cast<int>(b[0]), 0x12);  // most significant byte first
    CHECK_EQ(static_cast<int>(b[1]), 0x34);
    CHECK_EQ(static_cast<int>(load_be16(b)), 0x1234);

    store_be32(b, 0xDEADBEEFu);
    CHECK_EQ(static_cast<int>(b[0]), 0xDE);
    CHECK_EQ(load_be32(b), 0xDEADBEEFu);

    store_be64(b, 0x0102030405060708ULL);
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(static_cast<int>(b[i]), i + 1);
    }
    CHECK(load_be64(b) == 0x0102030405060708ULL);

    const std::int64_t vals[] = {0, 1, -1, 42, -42, INT64_MAX, INT64_MIN};
    for (std::int64_t v : vals) {
        store_be_i64(b, v);
        CHECK(load_be_i64(b) == v);
    }
}

TEST_CASE("fletcher16 catches transposition, which a plain sum would not") {
    const unsigned char a[] = {1, 2, 3, 4};
    const unsigned char swapped[] = {1, 3, 2, 4};  // identical additive sum
    const unsigned char reversed[] = {4, 3, 2, 1};

    CHECK(fletcher16(a, 4) != fletcher16(swapped, 4));
    CHECK(fletcher16(a, 4) != fletcher16(reversed, 4));
    CHECK_EQ(fletcher16(a, 4), fletcher16(a, 4));  // deterministic

    // Deferred-modulo block boundary is 21 bytes; cross it.
    std::vector<unsigned char> big(100);
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    const std::uint16_t before = fletcher16(big.data(), big.size());
    big[57] ^= 0x01;
    CHECK(fletcher16(big.data(), big.size()) != before);
}

TEST_CASE("fletcher16's zero-run blind spot is real, and the frame format covers it") {
    // Pinned deliberately: with the standard 0xff seed, all-zero bodies collide
    // regardless of length. Asserting the limitation means a future change to
    // the checksum cannot quietly alter what the format does or does not detect.
    const unsigned char zeros[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ(fletcher16(zeros, 3), fletcher16(zeros, 4));
    CHECK_EQ(fletcher16(zeros, 3), 0xFFFFu);

    // Why it does not matter here: length is validated against the message type
    // before the checksum is consulted, so a truncated body never reaches the
    // comparison that would miss it.
    auto bytes = encode_all({make_trade(1, 0, 0, 0), make_trade(2, 1, 101, 2)});
    store_be32(bytes.data() + 4, 24);  // shrink a (mostly zero) Trade body

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].seq, 2u);
    CHECK_EQ(d.stats().bad_length, 1u);
    CHECK_EQ(d.stats().bad_checksum, 0u);  // rejected by length, one step earlier
}

// ---------------------------------------------------------------------------
// Encode / decode
// ---------------------------------------------------------------------------

TEST_CASE("every message type survives an encode/decode round trip") {
    const std::vector<Message> sent = {
        make_trade(1, 5, 70500, 3), make_quote(2, 5),      make_new_order(3),
        make_ack(4),                make_heartbeat(5),
    };
    const auto bytes = encode_all(sent);

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == sent.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK_EQ(static_cast<int>(got[i].type), static_cast<int>(sent[i].type));
        CHECK_EQ(got[i].seq, sent[i].seq);
        CHECK_EQ(got[i].gap_before, 0u);
    }

    CHECK_EQ(got[0].body.trade.symbol, 5u);
    CHECK_EQ(got[0].body.trade.price, 70500);
    CHECK_EQ(got[0].body.trade.qty, 3);
    CHECK_EQ(got[0].body.trade.ts_ns, 1'700'000'000'123'456'789LL);

    CHECK_EQ(got[1].body.quote.bid_px, 71000);
    CHECK_EQ(got[1].body.quote.ask_qty, 34);
    CHECK_EQ(got[1].body.quote.ts_ns, -5);  // negative field is not mangled

    CHECK(got[2].body.new_order.client_order_id == 0xDEADBEEFCAFEF00DULL);
    CHECK(got[2].body.new_order.side == Side::Sell);
    CHECK(got[2].body.new_order.type == Type::Market);
    CHECK_EQ(static_cast<int>(got[2].body.new_order.tif), 1);
    CHECK_EQ(got[2].body.new_order.price, -123);

    CHECK_EQ(got[3].body.ack.exchange_order_id, 8u);
    CHECK(got[3].body.ack.status == AckStatus::Filled);
    CHECK_EQ(got[3].body.ack.avg_px, -64000);

    CHECK_EQ(d.stats().frames_decoded, 5u);
    CHECK_EQ(d.stats().resyncs, 0u);
    CHECK_EQ(d.stats().bad_checksum, 0u);
    CHECK_EQ(d.buffered(), 0u);
}

TEST_CASE("encode refuses a buffer that is too small rather than overrunning it") {
    const Message m = make_quote(1, 9);
    unsigned char small[kHeaderSize + 10];
    CHECK_EQ(encode(m, small, sizeof(small)), 0u);
    CHECK_EQ(encode(m, small, 0), 0u);

    unsigned char ok[kMaxEncodedFrame];
    CHECK_EQ(encode(m, ok, sizeof(ok)), kHeaderSize + 44);
}

// ---------------------------------------------------------------------------
// Framing: a TCP read is not a message
// ---------------------------------------------------------------------------

TEST_CASE("a stream fed one byte at a time decodes identically") {
    const std::vector<Message> sent = {make_trade(10, 1, 100, 1), make_quote(11, 1),
                                       make_ack(12), make_trade(13, 2, 200, 5)};
    const auto bytes = encode_all(sent);

    FrameDecoder d;
    std::vector<Message> got;
    Message m;
    for (unsigned char byte : bytes) {
        d.feed(&byte, 1);
        while (d.next(m)) {
            got.push_back(m);
        }
    }
    REQUIRE(got.size() == 4u);
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK_EQ(got[i].seq, sent[i].seq);
    }
    CHECK_EQ(d.stats().resyncs, 0u);
    CHECK_EQ(d.buffered(), 0u);
}

TEST_CASE("splitting the stream at every possible boundary always yields the same messages") {
    const std::vector<Message> sent = {make_trade(1, 1, 100, 1), make_new_order(2),
                                       make_heartbeat(3), make_quote(4, 7)};
    const auto bytes = encode_all(sent);

    for (std::size_t cut = 1; cut < bytes.size(); ++cut) {
        FrameDecoder d;
        d.feed(bytes.data(), cut);
        auto got = drain(d);
        d.feed(bytes.data() + cut, bytes.size() - cut);
        for (const auto& m : drain(d)) {
            got.push_back(m);
        }

        REQUIRE(got.size() == sent.size());
        for (std::size_t i = 0; i < sent.size(); ++i) {
            CHECK_EQ(got[i].seq, sent[i].seq);
            CHECK_EQ(static_cast<int>(got[i].type), static_cast<int>(sent[i].type));
        }
        CHECK_EQ(d.stats().resyncs, 0u);
    }
}

TEST_CASE("a magic pattern inside a body does not confuse length-based framing") {
    Message m = make_trade(1, 0x4B584B58u, 0x4B584B584B584B58LL, 0x4B58);
    Message after = make_heartbeat(2);
    const auto bytes = encode_all({m, after});

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 2u);
    CHECK_EQ(got[0].body.trade.symbol, 0x4B584B58u);
    CHECK_EQ(got[1].seq, 2u);
    CHECK_EQ(d.stats().resyncs, 0u);
}

// ---------------------------------------------------------------------------
// Damage: resync rather than desync forever
// ---------------------------------------------------------------------------

TEST_CASE("garbage before the first frame is discarded and the stream resyncs") {
    const auto good = encode_all({make_trade(1, 1, 100, 1), make_trade(2, 1, 101, 2)});
    std::vector<unsigned char> stream = {0x00, 0xFF, 0x11, 0x22, 0x33, 0x44, 0x55,
                                         0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
                                         0xDD, 0xEE, 0x01, 0x02, 0x03, 0x04, 0x05};
    const std::size_t junk = stream.size();
    stream.insert(stream.end(), good.begin(), good.end());

    FrameDecoder d;
    d.feed(stream.data(), stream.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 2u);
    CHECK_EQ(got[0].seq, 1u);
    CHECK_EQ(got[1].seq, 2u);
    CHECK(d.stats().resyncs >= 1u);
    CHECK_EQ(d.stats().bytes_discarded, junk);
}

TEST_CASE("a flipped body bit is caught by the checksum and the next frame still decodes") {
    auto bytes = encode_all({make_trade(1, 1, 100, 1), make_trade(2, 1, 101, 2)});
    bytes[kHeaderSize + 3] ^= 0x40;  // corrupt the first body

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].seq, 2u);  // the surviving frame is the second one
    CHECK_EQ(d.stats().bad_checksum, 1u);
    CHECK_EQ(d.stats().resyncs, 1u);
    CHECK_EQ(d.stats().frames_decoded, 1u);
}

TEST_CASE("a frame from an unsupported protocol version is rejected, not decoded") {
    auto bytes = encode_all({make_trade(1, 1, 100, 1), make_trade(2, 1, 101, 2)});
    bytes[2] = kVersion + 1;

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].seq, 2u);
    CHECK_EQ(d.stats().bad_version, 1u);
}

TEST_CASE("a body_len that disagrees with the message type is rejected") {
    auto bytes = encode_all({make_trade(1, 1, 100, 1), make_trade(2, 1, 101, 2)});
    store_be32(bytes.data() + 4, 27);  // Trade is 28

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].seq, 2u);
    CHECK_EQ(d.stats().bad_length, 1u);
}

TEST_CASE("an absurd body_len cannot make the decoder wait forever or allocate wildly") {
    auto bytes = encode_all({make_trade(1, 1, 100, 1), make_trade(2, 1, 101, 2)});
    store_be32(bytes.data() + 4, 0xFFFFFFFFu);

    FrameDecoder d(1024);  // small ceiling
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].seq, 2u);
    CHECK_EQ(d.stats().bad_length, 1u);
    CHECK(d.buffered() < 4096u);  // did not swallow the stream waiting for 4 GiB
}

// ---------------------------------------------------------------------------
// Forward compatibility
// ---------------------------------------------------------------------------

TEST_CASE("an unknown message type is skipped by length, keeping the stream framed") {
    // Hand-build a frame with type 99 and a 7-byte body, between two good ones.
    std::vector<unsigned char> stream;
    const auto first = encode_all({make_trade(1, 1, 100, 1)});
    stream.insert(stream.end(), first.begin(), first.end());

    unsigned char unknown[kHeaderSize + 7];
    const unsigned char body[7] = {9, 8, 7, 6, 5, 4, 3};
    std::memcpy(unknown + kHeaderSize, body, sizeof(body));
    store_be16(unknown + 0, kMagic);
    unknown[2] = kVersion;
    unknown[3] = 99;
    store_be32(unknown + 4, 7);
    store_be64(unknown + 8, 2);
    store_be16(unknown + 16, fletcher16(unknown + kHeaderSize, 7));
    store_be16(unknown + 18, 0);
    stream.insert(stream.end(), unknown, unknown + sizeof(unknown));

    const auto last = encode_all({make_trade(3, 1, 102, 3)});
    stream.insert(stream.end(), last.begin(), last.end());

    FrameDecoder d;
    d.feed(stream.data(), stream.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 2u);
    CHECK_EQ(got[0].seq, 1u);
    CHECK_EQ(got[1].seq, 3u);
    CHECK_EQ(d.stats().unknown_type, 1u);
    CHECK_EQ(d.stats().resyncs, 0u);   // skipped cleanly, not recovered from
    CHECK_EQ(d.stats().seq_gaps, 0u);  // it held seq 2, so nothing is missing
    CHECK_EQ(got[1].gap_before, 0u);
}

// ---------------------------------------------------------------------------
// Sequence integrity
// ---------------------------------------------------------------------------

TEST_CASE("a dropped sequence number is reported on the message that follows it") {
    const auto bytes = encode_all({make_trade(100, 1, 1, 1), make_trade(101, 1, 2, 1),
                                   make_trade(105, 1, 3, 1), make_trade(106, 1, 4, 1)});
    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 4u);
    CHECK_EQ(got[0].gap_before, 0u);
    CHECK_EQ(got[1].gap_before, 0u);
    CHECK_EQ(got[2].gap_before, 3u);  // 102, 103, 104 never arrived
    CHECK_EQ(got[3].gap_before, 0u);

    CHECK_EQ(d.stats().seq_gaps, 1u);
    CHECK_EQ(d.stats().seq_missing, 3u);
    CHECK_EQ(d.expected_seq(), 107u);
}

TEST_CASE("a repeated sequence number is flagged as a regression, not a gap") {
    const auto bytes = encode_all({make_trade(5, 1, 1, 1), make_trade(6, 1, 2, 1),
                                   make_trade(6, 1, 2, 1), make_trade(7, 1, 3, 1)});
    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 4u);
    CHECK_EQ(d.stats().seq_regressions, 1u);
    CHECK_EQ(d.stats().seq_gaps, 0u);
    CHECK_EQ(d.expected_seq(), 8u);
    for (const auto& m : got) {
        CHECK_EQ(m.gap_before, 0u);
    }
}

TEST_CASE("a gap discovered on an undeliverable frame is not lost") {
    // seq 1 arrives, 2 and 3 are dropped, seq 4 is an unknown type, seq 5 is
    // good. The gap must surface on seq 5 rather than vanish with seq 4.
    std::vector<unsigned char> stream;
    const auto first = encode_all({make_trade(1, 1, 100, 1)});
    stream.insert(stream.end(), first.begin(), first.end());

    unsigned char unknown[kHeaderSize];
    store_be16(unknown + 0, kMagic);
    unknown[2] = kVersion;
    unknown[3] = 77;
    store_be32(unknown + 4, 0);
    store_be64(unknown + 8, 4);
    store_be16(unknown + 16, fletcher16(unknown + kHeaderSize, 0));
    store_be16(unknown + 18, 0);
    stream.insert(stream.end(), unknown, unknown + sizeof(unknown));

    const auto last = encode_all({make_trade(5, 1, 101, 1)});
    stream.insert(stream.end(), last.begin(), last.end());

    FrameDecoder d;
    d.feed(stream.data(), stream.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 2u);
    CHECK_EQ(got[1].seq, 5u);
    CHECK_EQ(got[1].gap_before, 2u);  // 2 and 3
    CHECK_EQ(d.stats().seq_missing, 2u);
    CHECK_EQ(d.stats().unknown_type, 1u);
}

TEST_CASE("the first frame seen sets the baseline instead of reporting a huge gap") {
    const auto bytes = encode_all({make_trade(9'000'000, 1, 1, 1)});
    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    const auto got = drain(d);

    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].gap_before, 0u);
    CHECK_EQ(d.stats().seq_gaps, 0u);
    CHECK_EQ(d.expected_seq(), 9'000'001u);
}

// ---------------------------------------------------------------------------
// Buffer hygiene
// ---------------------------------------------------------------------------

TEST_CASE("a partial frame is held, not dropped, and the buffer drains to empty") {
    const auto bytes = encode_all({make_quote(1, 3)});
    FrameDecoder d;

    d.feed(bytes.data(), bytes.size() - 5);
    Message m;
    CHECK(!d.next(m));
    CHECK_EQ(d.buffered(), bytes.size() - 5);

    d.feed(bytes.data() + bytes.size() - 5, 5);
    CHECK(d.next(m));
    CHECK_EQ(m.seq, 1u);
    CHECK_EQ(d.buffered(), 0u);
}

TEST_CASE("reset clears buffered bytes, stats and sequence state") {
    auto bytes = encode_all({make_trade(1, 1, 100, 1)});
    bytes[2] = 0x7F;  // make it fail, so stats are non-zero

    FrameDecoder d;
    d.feed(bytes.data(), bytes.size());
    (void)drain(d);
    CHECK(d.stats().bad_version >= 1u);

    d.reset();
    CHECK_EQ(d.stats().bad_version, 0u);
    CHECK_EQ(d.stats().frames_decoded, 0u);
    CHECK_EQ(d.buffered(), 0u);
    CHECK_EQ(d.expected_seq(), 0u);

    const auto clean = encode_all({make_trade(50, 1, 100, 1)});
    d.feed(clean.data(), clean.size());
    const auto got = drain(d);
    REQUIRE(got.size() == 1u);
    CHECK_EQ(got[0].gap_before, 0u);
}

TEST_CASE("a long stream stays memory-bounded rather than growing with every frame") {
    std::vector<Message> many;
    for (std::uint64_t i = 1; i <= 5000; ++i) {
        many.push_back(make_trade(i, 1, 100 + static_cast<Price>(i), 1));
    }
    const auto bytes = encode_all(many);

    FrameDecoder d;
    std::size_t decoded = 0;
    Message m;
    for (std::size_t off = 0; off < bytes.size(); off += 37) {  // ragged reads
        const std::size_t chunk = (off + 37 <= bytes.size()) ? 37 : bytes.size() - off;
        d.feed(bytes.data() + off, chunk);
        while (d.next(m)) {
            ++decoded;
        }
        CHECK(d.buffered() < 8192u);
    }
    CHECK_EQ(decoded, many.size());
    CHECK_EQ(d.stats().seq_gaps, 0u);
    CHECK_EQ(d.stats().bytes_consumed, bytes.size());
}
