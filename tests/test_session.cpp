// Session-layer tests. Every case here is a way a real session goes wrong: a
// gap mid-burst, a replay that looks like a gap, a reset pointing backwards, a
// quiet market mistaken for a dead one.
#include <vector>

#include "lloms/session.hpp"
#include "microtest.hpp"

using namespace lloms::session;

namespace {

Inbound app(std::uint64_t seq, bool poss_dup = false) {
    Inbound m;
    m.seq = seq;
    m.admin = Admin::None;
    m.poss_dup = poss_dup;
    return m;
}

Inbound adm(std::uint64_t seq, Admin a) {
    Inbound m;
    m.seq = seq;
    m.admin = a;
    return m;
}

Inbound seq_reset(std::uint64_t seq, std::uint64_t new_seq, bool gap_fill) {
    Inbound m;
    m.seq = seq;
    m.admin = Admin::SequenceReset;
    m.new_seq = new_seq;
    m.gap_fill = gap_fill;
    return m;
}

// Bring a session up so tests can start from Active.
Session live(std::uint64_t first_in = 1) {
    Config c;
    c.next_inbound_seq = first_in;
    Session s(c);
    s.begin_logon(0);
    (void)s.on_message(adm(first_in, Admin::Logon), 0);
    return s;
}

}  // namespace

TEST_CASE("a session refuses to act on traffic before logon") {
    Config c;
    Session s(c);
    CHECK(s.state() == State::Disconnected);

    const Outcome early = s.on_message(app(1), 0);
    CHECK(!early.deliver);
    CHECK(early.drop);
    CHECK(early.reject == Reject::NotLoggedOn);

    s.begin_logon(0);
    const Outcome logon = s.on_message(adm(1, Admin::Logon), 0);
    CHECK(logon.drop);  // administrative, not application data
    CHECK(s.state() == State::Active);

    const Outcome ok = s.on_message(app(2), 0);
    CHECK(ok.deliver);
    CHECK_EQ(s.stats().delivered, 1u);
}

TEST_CASE("in-sequence application messages are delivered and advance the sequence") {
    Session s = live();
    for (std::uint64_t i = 2; i <= 6; ++i) {
        const Outcome o = s.on_message(app(i), 0);
        CHECK(o.deliver);
        CHECK(!o.queue);
        CHECK_EQ(s.next_inbound_seq(), i + 1);
    }
    CHECK_EQ(s.stats().delivered, 5u);
    CHECK_EQ(s.stats().gaps_detected, 0u);
}

TEST_CASE("a gap pauses delivery instead of acting on messages out of order") {
    Session s = live();
    CHECK(s.on_message(app(2), 0).deliver);

    // 3 and 4 never arrive; 5 does.
    const Outcome gap = s.on_message(app(5), 0);
    CHECK(!gap.deliver);       // the whole point: NOT delivered
    CHECK(gap.queue);
    CHECK(gap.send_resend_request);
    CHECK_EQ(gap.resend_from, 3u);
    CHECK_EQ(gap.resend_to, 4u);
    CHECK(s.state() == State::Recovering);

    // More traffic during recovery keeps queueing, and does NOT re-request.
    const Outcome more = s.on_message(app(6), 0);
    CHECK(more.queue);
    CHECK(!more.send_resend_request);
    CHECK_EQ(s.queued_count(), 2u);
    CHECK_EQ(s.stats().resend_requests_sent, 1u);
}

TEST_CASE("closing a gap releases the held messages in order, all at once") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    (void)s.on_message(app(5), 0);
    (void)s.on_message(app(6), 0);
    CHECK_EQ(s.queued_count(), 2u);

    const Outcome r3 = s.on_message(app(3, /*poss_dup=*/true), 0);
    CHECK(r3.deliver);
    CHECK_EQ(s.queued_count(), 2u);  // 5 and 6 still stranded behind 4

    const Outcome r4 = s.on_message(app(4, /*poss_dup=*/true), 0);
    CHECK(r4.deliver);
    CHECK_EQ(s.queued_count(), 0u);
    CHECK(s.state() == State::Active);

    const std::vector<Inbound> released = s.take_released();
    REQUIRE(released.size() == 2u);
    CHECK_EQ(released[0].seq, 5u);  // order preserved
    CHECK_EQ(released[1].seq, 6u);
    CHECK_EQ(s.next_inbound_seq(), 7u);
    CHECK(s.take_released().empty());  // taking twice does not duplicate
}

TEST_CASE("a duplicate is dropped, never mistaken for a gap") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    (void)s.on_message(app(3), 0);

    // A replay of 2. Requesting a resend here would ask for a duplicate that
    // arrives and triggers another request -- a loop that never converges.
    const Outcome dup = s.on_message(app(2, /*poss_dup=*/true), 0);
    CHECK(dup.drop);
    CHECK(!dup.deliver);
    CHECK(!dup.send_resend_request);
    CHECK(!dup.disconnect);
    CHECK_EQ(s.stats().duplicates_dropped, 1u);
    CHECK_EQ(s.next_inbound_seq(), 4u);  // untouched
}

TEST_CASE("a stale sequence that is NOT marked duplicate is a broken peer") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    (void)s.on_message(app(3), 0);

    const Outcome bad = s.on_message(app(2, /*poss_dup=*/false), 0);
    CHECK(bad.drop);
    CHECK(bad.disconnect);  // unrecoverable: we cannot know which history is real
    CHECK(bad.reject == Reject::SequenceTooLow);
    CHECK_EQ(s.stats().duplicates_dropped, 0u);
}

TEST_CASE("SequenceReset-GapFill skips the run the peer declined to replay") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    const Outcome gap = s.on_message(app(9), 0);
    CHECK(gap.queue);
    CHECK_EQ(gap.resend_from, 3u);
    CHECK_EQ(gap.resend_to, 8u);

    // Peer replays 3, then says "4..8 were admin, resume at 9".
    CHECK(s.on_message(app(3, true), 0).deliver);
    const Outcome fill = s.on_message(seq_reset(4, /*new_seq=*/9, /*gap_fill=*/true), 0);
    CHECK(fill.drop);  // the reset itself is not application data
    CHECK_EQ(s.stats().gap_fills_applied, 1u);

    CHECK_EQ(s.next_inbound_seq(), 10u);  // 9 was released from the queue
    CHECK(s.state() == State::Active);
    const std::vector<Inbound> released = s.take_released();
    REQUIRE(released.size() == 1u);
    CHECK_EQ(released[0].seq, 9u);
}

TEST_CASE("SequenceReset-Reset overrides the sequence but only forwards") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    (void)s.on_message(app(3), 0);
    CHECK_EQ(s.next_inbound_seq(), 4u);

    const Outcome fwd = s.on_message(seq_reset(4, /*new_seq=*/100, /*gap_fill=*/false), 0);
    CHECK(fwd.drop);
    CHECK(!fwd.disconnect);
    CHECK_EQ(s.next_inbound_seq(), 100u);
    CHECK_EQ(s.stats().resets_applied, 1u);

    // Backwards would re-deliver messages already acted on.
    const Outcome back = s.on_message(seq_reset(100, /*new_seq=*/50, /*gap_fill=*/false), 0);
    CHECK(back.drop);
    CHECK(back.reject == Reject::ResetBackwards);
    CHECK_EQ(s.next_inbound_seq(), 100u);  // unchanged

    const Outcome same = s.on_message(seq_reset(100, /*new_seq=*/100, /*gap_fill=*/true), 0);
    CHECK(same.reject == Reject::ResetBackwards);  // not forward either
}

TEST_CASE("a test request is answered, and a resend request is served from our stream") {
    Session s = live();
    for (int i = 0; i < 5; ++i) {
        (void)s.claim_outbound_seq();
    }
    CHECK_EQ(s.next_outbound_seq(), 7u);  // logon took 1, then 5 more

    const Outcome tr = s.on_message(adm(2, Admin::TestRequest), 0);
    CHECK(tr.send_heartbeat);
    CHECK(tr.drop);

    Inbound rr = adm(3, Admin::ResendRequest);
    rr.resend_from = 2;
    rr.resend_to = 4;
    const Outcome served = s.on_message(rr, 0);
    CHECK(served.send_resend);
    CHECK_EQ(served.replay_from, 2u);
    CHECK_EQ(served.replay_to, 4u);

    // resend_to == 0 means "everything you have sent".
    Inbound open = adm(4, Admin::ResendRequest);
    open.resend_from = 3;
    open.resend_to = 0;
    const Outcome all = s.on_message(open, 0);
    CHECK_EQ(all.replay_from, 3u);
    CHECK_EQ(all.replay_to, 6u);  // through our last claimed sequence
    CHECK_EQ(s.stats().resend_requests_served, 2u);
}

TEST_CASE("silence asks a question before it declares a death") {
    Config c;
    c.heartbeat_ms = 1000;
    Session s(c);
    s.begin_logon(0);
    (void)s.on_message(adm(1, Admin::Logon), 0);

    CHECK(!s.on_clock(500).send_heartbeat);   // still within the interval
    CHECK(!s.on_clock(999).disconnect);

    const Outcome ask = s.on_clock(1000);
    CHECK(ask.send_heartbeat);  // caller emits a TestRequest
    CHECK(!ask.disconnect);
    CHECK_EQ(s.stats().test_requests_sent, 1u);

    // A quiet market is not a dead one: any inbound message clears the doubt.
    (void)s.on_message(adm(2, Admin::Heartbeat), 1200);
    CHECK(!s.on_clock(2100).disconnect);
    CHECK(s.state() == State::Active);
}

TEST_CASE("silence past the unanswered question ends the session") {
    Config c;
    c.heartbeat_ms = 1000;
    Session s(c);
    s.begin_logon(0);
    (void)s.on_message(adm(1, Admin::Logon), 0);

    CHECK(s.on_clock(1000).send_heartbeat);
    const Outcome dead = s.on_clock(2000);
    CHECK(dead.disconnect);
    CHECK(s.state() == State::Disconnected);
    CHECK(!s.on_clock(3000).disconnect);  // stays down, does not re-fire
}

TEST_CASE("a logon claiming a sequence behind ours is dropped, not negotiated") {
    Session s = live(/*first_in=*/1);
    for (std::uint64_t i = 2; i <= 20; ++i) {
        (void)s.on_message(app(i), 0);
    }
    CHECK_EQ(s.next_inbound_seq(), 21u);

    // Peer reconnects believing it is at 5. There is nothing to resend -- it
    // simply has a different history, and acting on it would replay 16 messages.
    const Outcome behind = s.on_message(adm(5, Admin::Logon), 0);
    CHECK(behind.drop);
    CHECK(behind.disconnect);
    CHECK(behind.reject == Reject::LogonSequenceTooLow);
    CHECK_EQ(s.next_inbound_seq(), 21u);
}

TEST_CASE("a second logon on a live session is an error") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    const Outcome again = s.on_message(adm(3, Admin::Logon), 0);
    CHECK(again.disconnect);
    CHECK(again.reject == Reject::AlreadyLoggedOn);
}

TEST_CASE("logout is orderly: the session closes without being an error") {
    Session s = live();
    (void)s.on_message(app(2), 0);
    const Outcome bye = s.on_message(adm(3, Admin::Logout), 0);
    CHECK(bye.disconnect);
    CHECK(bye.drop);
    CHECK(bye.reject == Reject::None);
    CHECK(s.state() == State::LoggedOut);
}

TEST_CASE("a session resuming mid-stream expects the configured sequence, not 1") {
    Config c;
    c.next_inbound_seq = 500;
    c.next_outbound_seq = 900;
    Session s(c);
    CHECK_EQ(s.begin_logon(0), 900u);
    CHECK_EQ(s.next_outbound_seq(), 901u);

    (void)s.on_message(adm(500, Admin::Logon), 0);
    CHECK(s.state() == State::Active);
    CHECK(s.on_message(app(501), 0).deliver);
    CHECK_EQ(s.next_inbound_seq(), 502u);
}

TEST_CASE("a gap that opens during recovery does not lose the earlier hole") {
    Session s = live();
    (void)s.on_message(app(2), 0);

    const Outcome first = s.on_message(app(5), 0);  // hole at 3-4
    CHECK(first.send_resend_request);
    CHECK_EQ(first.resend_from, 3u);

    (void)s.on_message(app(9), 0);  // second hole at 6-8, still recovering
    CHECK_EQ(s.queued_count(), 2u);

    // Fill the first hole; 5 becomes deliverable, 9 must stay held.
    CHECK(s.on_message(app(3, true), 0).deliver);
    CHECK(s.on_message(app(4, true), 0).deliver);
    CHECK_EQ(s.next_inbound_seq(), 6u);
    CHECK_EQ(s.queued_count(), 1u);
    CHECK(s.state() == State::Recovering);

    std::vector<Inbound> rel = s.take_released();
    REQUIRE(rel.size() == 1u);
    CHECK_EQ(rel[0].seq, 5u);

    for (std::uint64_t i = 6; i <= 8; ++i) {
        CHECK(s.on_message(app(i, true), 0).deliver);
    }
    CHECK_EQ(s.queued_count(), 0u);
    CHECK(s.state() == State::Active);
    rel = s.take_released();
    REQUIRE(rel.size() == 1u);
    CHECK_EQ(rel[0].seq, 9u);
    CHECK_EQ(s.next_inbound_seq(), 10u);
}

TEST_CASE("nothing is ever delivered twice, however the gap is closed") {
    Session s = live();
    std::vector<std::uint64_t> seen;

    auto note = [&](const Outcome& o, std::uint64_t seq) {
        if (o.deliver) seen.push_back(seq);
        for (const Inbound& m : s.take_released()) seen.push_back(m.seq);
    };

    note(s.on_message(app(2), 0), 2);
    note(s.on_message(app(6), 0), 6);              // gap 3-5
    note(s.on_message(app(3, true), 0), 3);
    note(s.on_message(app(3, true), 0), 3);        // peer replays 3 again
    note(s.on_message(app(4, true), 0), 4);
    note(s.on_message(app(5, true), 0), 5);
    note(s.on_message(app(7), 0), 7);

    REQUIRE(seen.size() == 6u);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK_EQ(seen[i], static_cast<std::uint64_t>(i + 2));  // 2,3,4,5,6,7 exactly once
    }
    CHECK_EQ(s.stats().delivered, 6u);
    CHECK_EQ(s.stats().duplicates_dropped, 1u);
}

TEST_CASE("every state and reject has a distinct name for the log") {
    CHECK(std::string(to_string(State::Recovering)) == "recovering");
    CHECK(std::string(to_string(Reject::LogonSequenceTooLow)) == "logon-sequence-behind-ours");
    CHECK(std::string(to_string(Reject::None)) == "none");
    CHECK(std::string(to_string(State::Disconnected)) != std::string(to_string(State::LoggedOut)));
}
