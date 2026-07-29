#include "lloms/wire.hpp"

namespace lloms {
namespace wire {

// Body layouts, packed, big-endian, no padding.
//
//   Trade    (28) : u32 symbol | i64 price | i64 qty | i64 ts_ns
//   Quote    (44) : u32 symbol | i64 bid_px | i64 bid_qty | i64 ask_px
//                              | i64 ask_qty | i64 ts_ns
//   NewOrder (31) : u64 coid | u32 symbol | u8 side | u8 type | u8 tif
//                            | i64 price | i64 qty
//   OrderAck (34) : u64 coid | u64 exch_id | u8 status | u8 reject_reason
//                            | i64 filled_qty | i64 avg_px
//   Heartbeat (0)
namespace {

constexpr std::size_t kTradeBody = 28;
constexpr std::size_t kQuoteBody = 44;
constexpr std::size_t kNewOrderBody = 31;
constexpr std::size_t kOrderAckBody = 34;

void encode_body(const Message& m, unsigned char* b) {
    switch (m.type) {
        case MsgType::Heartbeat:
            break;
        case MsgType::Trade: {
            const Trade& t = m.body.trade;
            store_be32(b + 0, t.symbol);
            store_be_i64(b + 4, t.price);
            store_be_i64(b + 12, t.qty);
            store_be_i64(b + 20, t.ts_ns);
            break;
        }
        case MsgType::Quote: {
            const Quote& q = m.body.quote;
            store_be32(b + 0, q.symbol);
            store_be_i64(b + 4, q.bid_px);
            store_be_i64(b + 12, q.bid_qty);
            store_be_i64(b + 20, q.ask_px);
            store_be_i64(b + 28, q.ask_qty);
            store_be_i64(b + 36, q.ts_ns);
            break;
        }
        case MsgType::NewOrder: {
            const NewOrder& n = m.body.new_order;
            store_be64(b + 0, n.client_order_id);
            store_be32(b + 8, n.symbol);
            b[12] = static_cast<unsigned char>(n.side);
            b[13] = static_cast<unsigned char>(n.type);
            b[14] = n.tif;
            store_be_i64(b + 15, n.price);
            store_be_i64(b + 23, n.qty);
            break;
        }
        case MsgType::OrderAck: {
            const OrderAck& a = m.body.ack;
            store_be64(b + 0, a.client_order_id);
            store_be64(b + 8, a.exchange_order_id);
            b[16] = static_cast<unsigned char>(a.status);
            b[17] = a.reject_reason;
            store_be_i64(b + 18, a.filled_qty);
            store_be_i64(b + 26, a.avg_px);
            break;
        }
    }
}

// Precondition: `len` already validated to equal body_size_for(type), so this
// cannot run off the end and has nothing to reject.
void decode_body(MsgType type, const unsigned char* b, Message& m) {
    switch (type) {
        case MsgType::Heartbeat:
            break;
        case MsgType::Trade: {
            Trade t{};
            t.symbol = load_be32(b + 0);
            t.price = load_be_i64(b + 4);
            t.qty = load_be_i64(b + 12);
            t.ts_ns = load_be_i64(b + 20);
            m.body.trade = t;
            break;
        }
        case MsgType::Quote: {
            Quote q{};
            q.symbol = load_be32(b + 0);
            q.bid_px = load_be_i64(b + 4);
            q.bid_qty = load_be_i64(b + 12);
            q.ask_px = load_be_i64(b + 20);
            q.ask_qty = load_be_i64(b + 28);
            q.ts_ns = load_be_i64(b + 36);
            m.body.quote = q;
            break;
        }
        case MsgType::NewOrder: {
            NewOrder n{};
            n.client_order_id = load_be64(b + 0);
            n.symbol = load_be32(b + 8);
            n.side = b[12] == 0 ? Side::Buy : Side::Sell;
            n.type = b[13] == 0 ? Type::Limit : Type::Market;
            n.tif = b[14];
            n.price = load_be_i64(b + 15);
            n.qty = load_be_i64(b + 23);
            m.body.new_order = n;
            break;
        }
        case MsgType::OrderAck: {
            OrderAck a{};
            a.client_order_id = load_be64(b + 0);
            a.exchange_order_id = load_be64(b + 8);
            a.status = static_cast<AckStatus>(b[16]);
            a.reject_reason = b[17];
            a.filled_qty = load_be_i64(b + 18);
            a.avg_px = load_be_i64(b + 26);
            m.body.ack = a;
            break;
        }
    }
}

}  // namespace

bool body_size_for(MsgType type, std::size_t& out) {
    switch (type) {
        case MsgType::Heartbeat: out = 0;              return true;
        case MsgType::Trade:     out = kTradeBody;     return true;
        case MsgType::Quote:     out = kQuoteBody;     return true;
        case MsgType::NewOrder:  out = kNewOrderBody;  return true;
        case MsgType::OrderAck:  out = kOrderAckBody;  return true;
    }
    out = 0;
    return false;
}

std::uint16_t fletcher16(const unsigned char* data, std::size_t len) {
    std::uint32_t sum1 = 0xff;
    std::uint32_t sum2 = 0xff;
    while (len > 0) {
        // Defer the modulo: 21 bytes is the most that can be accumulated before
        // sum2 could overflow 32 bits.
        std::size_t block = len > 21 ? 21 : len;
        len -= block;
        while (block-- > 0) {
            sum1 += *data++;
            sum2 += sum1;
        }
        sum1 = (sum1 & 0xff) + (sum1 >> 8);
        sum2 = (sum2 & 0xff) + (sum2 >> 8);
    }
    sum1 = (sum1 & 0xff) + (sum1 >> 8);
    sum2 = (sum2 & 0xff) + (sum2 >> 8);
    return static_cast<std::uint16_t>((sum2 << 8) | sum1);
}

std::size_t encode(const Message& msg, unsigned char* out, std::size_t cap) {
    std::size_t body_len = 0;
    if (!body_size_for(msg.type, body_len)) {
        return 0;
    }
    if (cap < kHeaderSize + body_len) {
        return 0;
    }
    unsigned char* body = out + kHeaderSize;
    encode_body(msg, body);

    store_be16(out + 0, kMagic);
    out[2] = kVersion;
    out[3] = static_cast<unsigned char>(msg.type);
    store_be32(out + 4, static_cast<std::uint32_t>(body_len));
    store_be64(out + 8, msg.seq);
    store_be16(out + 16, fletcher16(body, body_len));
    store_be16(out + 18, 0);  // reserved
    return kHeaderSize + body_len;
}

FrameDecoder::FrameDecoder(std::size_t max_body) : max_body_(max_body) {
    buf_.reserve(4096);
}

void FrameDecoder::feed(const unsigned char* data, std::size_t len) {
    if (len == 0) {
        return;
    }
    compact();
    buf_.insert(buf_.end(), data, data + len);
}

void FrameDecoder::compact() {
    if (rpos_ == 0) {
        return;
    }
    if (rpos_ == buf_.size()) {
        buf_.clear();
        rpos_ = 0;
        return;
    }
    // Moving on every frame would make the decoder O(n) in buffer size per
    // message -- the classic "erase from the front" wart. Only pay for the move
    // once the dead prefix is worth reclaiming; waste stays bounded at 2x.
    if (rpos_ < 4096 && rpos_ * 2 < buf_.size()) {
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(rpos_));
    rpos_ = 0;
}

bool FrameDecoder::resync() {
    ++stats_.resyncs;
    // Whatever is at rpos_ failed validation, so its length field cannot be
    // trusted to tell us where the next frame starts. Step one byte and hunt
    // for the magic instead.
    std::size_t p = rpos_ + 1;
    const std::size_t end = buf_.size();
    while (p + 2 <= end && load_be16(buf_.data() + p) != kMagic) {
        ++p;
    }
    const bool found = (p + 2 <= end);
    if (!found) {
        // Keep the final byte: a magic can straddle this read and the next.
        p = end - 1;
    }
    stats_.bytes_discarded += p - rpos_;
    rpos_ = p;
    compact();
    return found;
}

bool FrameDecoder::next(Message& out) {
    for (;;) {
        if (buf_.size() - rpos_ < kHeaderSize) {
            return false;
        }
        const unsigned char* p = buf_.data() + rpos_;

        if (load_be16(p) != kMagic) {
            ++stats_.bad_magic;
            if (!resync()) {
                return false;
            }
            continue;
        }
        const std::uint8_t version = p[2];
        const MsgType type = static_cast<MsgType>(p[3]);
        const std::uint32_t body_len = load_be32(p + 4);
        const std::uint64_t seq = load_be64(p + 8);
        const std::uint16_t want_csum = load_be16(p + 16);

        if (version != kVersion) {
            ++stats_.bad_version;
            if (!resync()) {
                return false;
            }
            continue;
        }
        if (body_len > max_body_) {
            ++stats_.bad_length;
            if (!resync()) {
                return false;
            }
            continue;
        }
        std::size_t expected_body = 0;
        const bool known = body_size_for(type, expected_body);
        if (known && body_len != expected_body) {
            ++stats_.bad_length;
            if (!resync()) {
                return false;
            }
            continue;
        }

        const std::size_t frame_len = kHeaderSize + body_len;
        if (buf_.size() - rpos_ < frame_len) {
            return false;  // frame split across reads -- wait for the rest
        }

        const unsigned char* body = p + kHeaderSize;
        if (fletcher16(body, body_len) != want_csum) {
            ++stats_.bad_checksum;
            if (!resync()) {
                return false;
            }
            continue;
        }

        // Framing and integrity hold. Decode before advancing, because compact()
        // can move the buffer out from under `body`.
        Message m;
        m.type = type;
        m.seq = seq;
        if (known) {
            decode_body(type, body, m);
        }

        rpos_ += frame_len;
        stats_.bytes_consumed += frame_len;
        ++stats_.frames_decoded;

        // Sequence accounting covers every well-formed frame, including types
        // we do not decode -- they consume sequence numbers too, so skipping
        // them here would invent a gap that never happened.
        if (!have_seq_) {
            have_seq_ = true;
            next_seq_ = seq + 1;
        } else if (seq > next_seq_) {
            const std::uint64_t missing = seq - next_seq_;
            ++stats_.seq_gaps;
            stats_.seq_missing += missing;
            pending_gap_ += missing;
            next_seq_ = seq + 1;
        } else if (seq < next_seq_) {
            ++stats_.seq_regressions;  // duplicate or replay; deliver anyway
        } else {
            next_seq_ = seq + 1;
        }

        compact();

        if (!known) {
            // Forward compatibility: a type we have never heard of is skipped by
            // its length, leaving the stream framed. Resyncing here would turn a
            // routine venue spec bump into a feed outage.
            ++stats_.unknown_type;
            continue;
        }

        m.gap_before = pending_gap_;
        pending_gap_ = 0;
        out = m;
        return true;
    }
}

void FrameDecoder::reset() {
    buf_.clear();
    rpos_ = 0;
    next_seq_ = 0;
    pending_gap_ = 0;
    have_seq_ = false;
    stats_ = Stats{};
}

}  // namespace wire
}  // namespace lloms
