#include "lloms/session.hpp"

namespace lloms {
namespace session {

const char* to_string(State s) {
    switch (s) {
        case State::Disconnected: return "disconnected";
        case State::LogonSent:    return "logon-sent";
        case State::Active:       return "active";
        case State::Recovering:   return "recovering";
        case State::LoggedOut:    return "logged-out";
    }
    return "unknown";
}

const char* to_string(Reject r) {
    switch (r) {
        case Reject::None:                return "none";
        case Reject::NotLoggedOn:         return "traffic-before-logon";
        case Reject::SequenceTooLow:      return "sequence-already-processed";
        case Reject::LogonSequenceTooLow: return "logon-sequence-behind-ours";
        case Reject::ResetBackwards:      return "sequence-reset-moves-backwards";
        case Reject::AlreadyLoggedOn:     return "already-logged-on";
    }
    return "unknown";
}

Session::Session(Config cfg)
    : next_in_(cfg.next_inbound_seq == 0 ? 1 : cfg.next_inbound_seq),
      next_out_(cfg.next_outbound_seq == 0 ? 1 : cfg.next_outbound_seq),
      heartbeat_ms_(cfg.heartbeat_ms == 0 ? 30000 : cfg.heartbeat_ms) {}

std::uint64_t Session::begin_logon(std::int64_t now_ms) {
    state_ = State::LogonSent;
    last_inbound_ms_ = now_ms;
    test_request_outstanding_ = false;
    return claim_outbound_seq();
}

std::uint64_t Session::claim_outbound_seq() {
    return next_out_++;
}

std::vector<Inbound> Session::take_released() {
    std::vector<Inbound> out;
    out.swap(released_);
    return out;
}

// A message whose sequence is exactly what we expected. Everything that is safe
// to act on flows through here, and nowhere else.
Outcome Session::accept_in_sequence(const Inbound& in, std::int64_t now_ms) {
    Outcome out;
    last_inbound_ms_ = now_ms;
    test_request_outstanding_ = false;

    switch (in.admin) {
        case Admin::SequenceReset:
            // GapFill: the peer is telling us it declined to replay a run of
            // administrative messages, so skip to new_seq. Reset: it is
            // overriding the expected sequence outright.
            if (in.new_seq <= next_in_) {
                // Both forms may only move forward. A backwards reset would
                // re-deliver messages we have already acted on.
                ++stats_.rejects;
                out.drop = true;
                out.reject = Reject::ResetBackwards;
                return out;
            }
            next_in_ = in.new_seq;
            if (in.gap_fill) {
                ++stats_.gap_fills_applied;
            } else {
                ++stats_.resets_applied;
            }
            out.drop = true;  // the reset itself is not application data
            break;

        case Admin::TestRequest:
            ++next_in_;
            out.send_heartbeat = true;
            out.drop = true;
            break;

        case Admin::ResendRequest:
            ++next_in_;
            out.send_resend = true;
            out.replay_from = in.resend_from;
            out.replay_to = in.resend_to == 0 ? (next_out_ - 1) : in.resend_to;
            ++stats_.resend_requests_served;
            out.drop = true;
            break;

        case Admin::Logout:
            ++next_in_;
            state_ = State::LoggedOut;
            out.drop = true;
            out.disconnect = true;
            break;

        case Admin::Heartbeat:
            ++next_in_;
            out.drop = true;
            break;

        case Admin::Logon:
            ++next_in_;
            if (state_ == State::Active || state_ == State::Recovering) {
                ++stats_.rejects;
                out.drop = true;
                out.disconnect = true;
                out.reject = Reject::AlreadyLoggedOn;
                return out;
            }
            state_ = State::Active;
            out.drop = true;
            break;

        case Admin::None:
            if (state_ != State::Active && state_ != State::Recovering) {
                // Application traffic before the session is up. Never act on
                // it: we have not agreed a sequence, so we cannot know whether
                // it is fresh or a replay from a previous session.
                ++stats_.rejects;
                out.drop = true;
                out.reject = Reject::NotLoggedOn;
                return out;
            }
            ++next_in_;
            ++stats_.delivered;
            out.deliver = true;
            break;
    }

    // Closing a gap can make previously-queued messages contiguous again.
    if (state_ == State::Recovering) {
        release_queue(out);
    }
    return out;
}

// Walk the held messages in order, releasing every one that is now contiguous.
// Anything still ahead of a hole stays queued.
void Session::release_queue(Outcome& out) {
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (std::size_t i = 0; i < queued_.size(); ++i) {
            if (queued_[i].seq != next_in_) {
                continue;
            }
            Inbound m = queued_[i];
            queued_.erase(queued_.begin() + static_cast<std::ptrdiff_t>(i));
            ++next_in_;
            if (m.admin == Admin::None) {
                released_.push_back(m);
                ++stats_.delivered;
            }
            progressed = true;
            break;
        }
    }
    if (queued_.empty()) {
        state_ = State::Active;
        (void)out;  // nothing extra to signal; released_ carries the payload
    }
}

Outcome Session::on_message(const Inbound& in, std::int64_t now_ms) {
    Outcome out;

    if (state_ == State::Disconnected && in.admin != Admin::Logon) {
        ++stats_.rejects;
        out.drop = true;
        out.reject = Reject::NotLoggedOn;
        return out;
    }

    // A logon whose sequence is behind what we have already processed cannot be
    // repaired by asking for a resend -- there is nothing to resend, the peer
    // simply believes a different history. Dropping is the only safe answer.
    if (in.admin == Admin::Logon && in.seq < next_in_ && !in.poss_dup) {
        ++stats_.rejects;
        out.drop = true;
        out.disconnect = true;
        out.reject = Reject::LogonSequenceTooLow;
        return out;
    }

    if (in.seq < next_in_) {
        // Already processed. Marked duplicate: expected during a resend, drop
        // quietly. Not marked: the peer's sequence is broken, which is a
        // session-level error rather than something to recover from.
        last_inbound_ms_ = now_ms;
        out.drop = true;
        if (in.poss_dup) {
            ++stats_.duplicates_dropped;
        } else {
            ++stats_.rejects;
            out.disconnect = true;
            out.reject = Reject::SequenceTooLow;
        }
        return out;
    }

    if (in.seq > next_in_) {
        // A gap. A SequenceReset is the one message allowed to jump ahead --
        // it is precisely how the peer closes a gap it will not replay, so it
        // is processed rather than queued.
        if (in.admin == Admin::SequenceReset) {
            return accept_in_sequence(in, now_ms);
        }

        last_inbound_ms_ = now_ms;
        test_request_outstanding_ = false;

        const bool first_gap = (state_ != State::Recovering);
        if (first_gap) {
            ++stats_.gaps_detected;
            state_ = State::Recovering;
            out.send_resend_request = true;
            out.resend_from = next_in_;
            out.resend_to = in.seq - 1;
            ++stats_.resend_requests_sent;
        }
        // Hold it. Delivering now would put an application message in front of
        // ones the peer sent earlier, which is exactly how a fill gets counted
        // against the wrong state.
        queued_.push_back(in);
        ++stats_.queued_total;
        out.queue = true;
        return out;
    }

    return accept_in_sequence(in, now_ms);
}

Outcome Session::on_clock(std::int64_t now_ms) {
    Outcome out;
    if (state_ != State::Active && state_ != State::Recovering &&
        state_ != State::LogonSent) {
        return out;
    }

    const std::int64_t silent = now_ms - last_inbound_ms_;
    if (silent < static_cast<std::int64_t>(heartbeat_ms_)) {
        return out;
    }

    if (!test_request_outstanding_) {
        // Silence alone is not death -- a quiet market is silent too. Ask.
        test_request_outstanding_ = true;
        last_inbound_ms_ = now_ms;
        ++stats_.test_requests_sent;
        out.send_heartbeat = true;  // caller sends a TestRequest
        return out;
    }

    // Asked, and still nothing. Now it is dead.
    state_ = State::Disconnected;
    out.disconnect = true;
    return out;
}

}  // namespace session
}  // namespace lloms
