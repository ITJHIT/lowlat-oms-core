// Exchange session layer: sequence integrity, gap recovery, and liveness.
//
// wire.hpp gets bytes off the socket and turns them into framed messages. This
// is the layer above: deciding whether a message may be *acted on*, and what to
// do when the stream is not whole. Session bugs do not corrupt one message --
// they double-count fills and act on a book you cannot see.
//
// Deliberately a pure state machine. It owns no socket, no clock and no thread:
// you feed it events and it returns the actions to take. That is what makes the
// interesting cases -- a gap that arrives mid-burst, a resend that overlaps a
// second gap, a logon whose sequence disagrees -- testable at all, rather than
// only reproducible against a live venue at 09:00.
//
// The five behaviours that actually break in production, all modelled here:
//
//   1. **A gap stops delivery, it does not just get logged.** Messages arriving
//      after a detected gap are QUEUED, not delivered. Acting on them out of
//      order is how a fill is counted twice.
//   2. **A duplicate is not a gap.** A replayed message (lower sequence, marked
//      possibly-duplicate) is dropped. Treating it as a gap requests a resend
//      that requests another duplicate, forever.
//   3. **GapFill and Reset are different instructions.** GapFill skips over
//      administrative messages the peer chose not to replay. Reset overrides the
//      expected sequence outright and is only legal moving forward.
//   4. **Liveness is a two-step, not a timeout.** Silence past the interval
//      sends a test request; only silence past *that* is a dead session.
//   5. **A logon that disagrees about sequence is an error, not a negotiation.**
//      A peer claiming a LOWER sequence than we have already processed cannot be
//      reconciled by requesting a resend -- the only safe response is to drop.
#pragma once

#include <cstdint>
#include <vector>

namespace lloms {
namespace session {

enum class State : std::uint8_t {
    Disconnected,  // no session
    LogonSent,     // we initiated, awaiting their logon
    Active,        // both sides logged on, sequences agreed
    Recovering,    // gap detected, resend requested, delivery paused
    LoggedOut,     // orderly shutdown
};

const char* to_string(State s);

// Administrative message kinds the session itself understands. Anything else is
// application data and is the caller's business.
enum class Admin : std::uint8_t {
    None = 0,      // application message
    Logon,
    Logout,
    Heartbeat,
    TestRequest,
    ResendRequest,
    SequenceReset,
};

// One inbound message, as handed up by the framing layer.
struct Inbound {
    std::uint64_t seq{0};
    Admin admin{Admin::None};
    bool poss_dup{false};       // peer marked this a possible replay
    // SequenceReset only:
    bool gap_fill{false};       // true = GapFill (skip), false = Reset (override)
    std::uint64_t new_seq{0};   // sequence the peer says comes next
    // ResendRequest only: the range they want back from us.
    std::uint64_t resend_from{0};
    std::uint64_t resend_to{0};  // 0 means "through the end"
};

enum class Reject : std::uint8_t {
    None = 0,
    NotLoggedOn,          // application traffic before logon
    SequenceTooLow,       // already processed and not marked duplicate
    LogonSequenceTooLow,  // peer's logon disagrees, unrecoverable
    ResetBackwards,       // SequenceReset-Reset may only move forward
    AlreadyLoggedOn,      // a second logon on a live session
};

const char* to_string(Reject r);

// What the caller must do with this message. Exactly one of deliver/queue/drop
// is true; the send_* flags are independent and may accompany any of them.
struct Outcome {
    bool deliver{false};              // safe to act on now
    bool queue{false};                // held: a gap is open ahead of it
    bool drop{false};                 // duplicate or administrative
    bool send_heartbeat{false};       // answer a test request
    bool send_resend_request{false};
    std::uint64_t resend_from{0};
    std::uint64_t resend_to{0};       // 0 = through the end
    bool send_resend{false};          // peer asked us to replay
    std::uint64_t replay_from{0};
    std::uint64_t replay_to{0};
    bool disconnect{false};
    Reject reject{Reject::None};
};

struct Config {
    std::uint32_t heartbeat_ms{30000};
    std::uint64_t next_inbound_seq{1};   // first sequence we expect
    std::uint64_t next_outbound_seq{1};  // first sequence we will send
};

class Session {
  public:
    explicit Session(Config cfg);

    State state() const { return state_; }
    std::uint64_t next_inbound_seq() const { return next_in_; }
    std::uint64_t next_outbound_seq() const { return next_out_; }

    // Messages held because a gap is open ahead of them, oldest first. They
    // become deliverable once the gap closes.
    const std::vector<Inbound>& queued() const { return queued_; }
    std::size_t queued_count() const { return queued_.size(); }

    // We are initiating. Returns the sequence to stamp on our Logon.
    std::uint64_t begin_logon(std::int64_t now_ms);

    // Feed one inbound message. `now_ms` is a monotonic millisecond clock.
    Outcome on_message(const Inbound& in, std::int64_t now_ms);

    // Call periodically. Drives the two-step liveness check: silence past the
    // interval asks a question, silence past the answer ends the session.
    Outcome on_clock(std::int64_t now_ms);

    // Stamp an outbound message. Returns the sequence used.
    std::uint64_t claim_outbound_seq();

    // Messages released after a gap closed, oldest first. Ownership transfers;
    // the internal queue is emptied.
    std::vector<Inbound> take_released();

    struct Stats {
        std::uint64_t delivered{0};
        std::uint64_t queued_total{0};
        std::uint64_t duplicates_dropped{0};
        std::uint64_t gaps_detected{0};
        std::uint64_t gap_fills_applied{0};
        std::uint64_t resets_applied{0};
        std::uint64_t resend_requests_sent{0};
        std::uint64_t resend_requests_served{0};
        std::uint64_t test_requests_sent{0};
        std::uint64_t rejects{0};
    };
    const Stats& stats() const { return stats_; }

  private:
    Outcome accept_in_sequence(const Inbound& in, std::int64_t now_ms);
    void release_queue(Outcome& out);

    State state_{State::Disconnected};
    std::uint64_t next_in_;
    std::uint64_t next_out_;
    std::uint32_t heartbeat_ms_;
    std::int64_t last_inbound_ms_{0};
    bool test_request_outstanding_{false};
    std::vector<Inbound> queued_;
    std::vector<Inbound> released_;
    Stats stats_{};
};

}  // namespace session
}  // namespace lloms
