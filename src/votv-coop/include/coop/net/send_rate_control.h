// coop/net/send_rate_control.h -- what a peer's link actually does, measured per slot.
//
// GameNetworkingSockets has no bandwidth estimation in this build: a connection's send rate is
// written once at connect from the ping and thereafter only clamped into [SendRateMin, SendRateMax],
// so a link past a few milliseconds of ping runs at the configured floor for its whole life.
// Steering that rate needs two numbers the transport does not offer, and this is where they come
// from: the goodput the peer acknowledges, and a round trip we time ourselves on the priority lane
// the pose stream rides. A busy link reports both once a second; an idle one is probed slowly and
// silently, because a baseline taken only under load is the queue it is supposed to reveal.
//
// This module measures and reports; it writes no send rate. `gnsRate` in its line is the pinned
// one (coop/net/session_start.cpp).
//
// Threading: the two byte counters are atomics, written at the send and receive choke points from
// whichever thread reached them. Everything else is net-thread-only, driven from the session loop.

#pragma once

#include "coop/net/protocol.h"             // LinkProbePayload
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>

namespace coop::net {

class SendRateControl {
public:
    // Below this much work in flight, in either direction, a link counts as idle: it reports
    // nothing, so an idle session costs no log lines. A rate is a ceiling, and with nothing queued a
    // ceiling costs nothing.
    static constexpr int64_t kIdleDemandBytes = 8 * 1024;

    // Probe cadence, busy and idle. The idle one is what makes `rttMin` a BASELINE: a minimum taken
    // only from loaded samples absorbs the bottleneck queue, which is the exact defect that
    // disqualifies the transport's own min-filtered ping as a delay input. 1 Hz of a 32-byte
    // datagram is 32 B/s per peer, so an idle link is measured for free.
    static constexpr uint64_t kProbeIntervalMs     = 100;
    static constexpr uint64_t kIdleProbeIntervalMs = 1'000;
    // How many probes may be outstanding at once. A ninth would ask nothing the eight unanswered
    // ones have not already asked, so the mint is skipped and counted; retirement is kProbeLostMs
    // below, not this.
    static constexpr int      kMaxOutstanding  = 8;
    // When an unanswered probe stops being a slow reading and becomes a lost one. This is our
    // policy, not the transport's: GNS's own connected-timeout defaults to 10 s, twice this, so the
    // link is still live when we retire a probe -- and an echo that arrives later lands in `stray`.
    static constexpr uint64_t kProbeLostMs = 5'000;

    // The window rttMin is taken over, in one-second buckets: long enough that a queue filling for
    // a few seconds cannot drag the baseline up behind it.
    static constexpr int kRttMinBuckets = 10;

    // Session::Start: a new session measures from zero. Called before the net thread exists.
    void Reset();
    // Slot teardown. GNS's pending counters restart with the next connection, so ours must too, or
    // the next occupant of the slot inherits a byte debt the identity below would read as delivery.
    // Any thread -- a host-side kick runs on the game thread -- so it ARMS only, and the net thread
    // performs the whole reset, counters included, at its next touch of the slot. Nothing else keeps
    // the block below single-owner: a fence cannot, because the reader's arm test and its counter
    // read are two separate operations.
    void FreeSlot(int slot);

    // Reliable wire bytes GNS ACCEPTED for this slot: the whole packet, which is the number the
    // peer counts on arrival. Any thread, from the send choke points.
    void NoteReliableQueued(int slot, int bytes);
    // Reliable wire bytes this slot's link delivered to us, counting the same kinds the sender
    // counts. Net thread. It is the receiving end of the same quantity, which is what makes a
    // sender's goodput estimate checkable against a second, independent measurement instead of
    // against itself.
    void NoteReliableReceived(int slot, int bytes);
    // The responder could not put an echo on the wire (the send buffer was at the brim). Counted
    // here, on the answering side, because to the prober a refused echo and a lost probe look
    // identical, and telling them apart is the whole question of whether headroom needs a rule.
    void NoteEchoRefused(int slot);

    // What the net thread read off one connection: GNS's send accounting, plus the depth of our own
    // queue in front of it. Passed in rather than read here, so this file stays free of the
    // transport API and the session keeps one status read per pass.
    struct LinkSample {
        int    pendingReliable     = 0;  // m_cbPendingReliable: handed to GNS, not yet on the wire
        int    sentUnackedReliable = 0;  // m_cbSentUnackedReliable: on the wire, not yet acked
        int    pendingUnreliable   = 0;  // m_cbPendingUnreliable
        int    gnsRateBps          = 0;  // m_nSendRateBytesPerSecond: the pin, for the record
        int    gnsPingMs           = -1; // m_nPing: an RTT floor (min-filtered), the cross-check
        size_t backlogBytes        = 0;  // SendBacklog::DepthBytes: queued before GNS ever saw it
    };

    // Fold one sample for a slot (net thread, ~10 Hz) and emit that slot's line at 1 Hz.
    void Sample(int slot, const LinkSample& in, uint64_t nowMs);

    // The probe this slot owes, if one is due; false when the link is idle or a probe went out
    // within the cadence. Net thread. The caller sends `out` and, if the send fails, says so.
    bool NextProbe(int slot, uint64_t nowMs, LinkProbePayload& out);
    // The send of a minted probe failed. Expected, not exceptional: with the send buffer at the
    // brim -- the state a bulk transfer creates -- GNS refuses every new message, probes included.
    void NoteProbeRefused(int slot, uint32_t token);
    // An echo came back. The round trip is measured against OUR record of when that token went out,
    // never against the sentMs the reply carries, so a wrong or hostile echo cannot invent one.
    void NoteProbeReply(int slot, const LinkProbePayload& reply, uint64_t nowMs);

    // What the status reads themselves cost, per net-thread pass; reported once a second while any
    // link is busy, because a 10 Hz per-peer telemetry read is a claim that has to be measured.
    // `links` is how many connections this pass read, which is why the line reports passes and link
    // samples as two numbers -- on a three-client host they differ by 3x.
    void NoteSampleCost(uint64_t us, int links, uint64_t nowMs);

private:
    static constexpr int kSlots = static_cast<int>(coop::players::kMaxPeers);

    struct Outstanding {
        uint32_t token  = 0;   // 0 = the entry is free
        uint64_t sentMs = 0;
    };

    // Everything the net thread alone owns, in one struct so an armed teardown is one assignment
    // (the two counters beside it are atomics, written from any thread, and cannot be copied over).
    struct Measured {
        int64_t  demand = 0;          // in-flight work on OUR send side
        bool     busy = false;        // in flight in EITHER direction: the probe's cadence choice
        LinkSample last{};

        // The differentiated delivery: the previous sample, and the 1 Hz window the line reports.
        int64_t  prevDelivered = 0;
        uint64_t prevSampleMs = 0;
        bool     havePrev = false;
        int64_t  peakBps = 0;         // the fastest 100 ms interval inside the reported second

        uint64_t windowMs = 0;        // when the reported second began (0 = not started)
        int64_t  windowDelivered = 0;
        uint64_t windowRecv = 0;
        bool     sawTraffic = false;  // busy in either direction at any sample in this window
        bool     wasBusy = false;     // did the previous window report, so the quiet edge says so

        // The probe.
        uint32_t    probeCounter = 0;
        uint64_t    lastProbeMs = 0;
        Outstanding out[kMaxOutstanding];
        int         rttLastMs = -1;
        int         rttMinWindow = -1, rttMaxWindow = -1;
        int64_t     rttSumWindow = 0;
        int         rttCountWindow = 0;
        int         probesSent = 0, probesReplied = 0, probesRefused = 0, probesLost = 0;
        int         probesUnknown = 0;  // an echo for no token of ours
        int         probesSkipped = 0;  // the mint the full outstanding ring refused
        int         echoesRefused = 0;  // OUR answers to the peer the send buffer would not take
        int         rttBucketMin[kRttMinBuckets]{};
        uint64_t    rttBucketSec[kRttMinBuckets]{};
    };

    struct Slot {
        // The two cumulative counters, written from any thread at the choke points.
        std::atomic<uint64_t> queuedBytes{0};
        std::atomic<uint64_t> recvBytes{0};
        // Set by FreeSlot from whichever thread tore the slot down; consumed by the net thread.
        std::atomic<bool> resetArmed{false};
        Measured m;
    };

    // The slot's measured block, with an armed teardown performed first. Net thread only -- this is
    // the one door to `Measured`, which is what makes its single ownership checkable.
    Measured& Take_(int slot);

    // The smallest round trip in the rttMin window, or -1 with nothing measured yet.
    static int RttMinFor_(const Measured& m, uint64_t nowMs);
    // Retire outstanding probes older than kProbeLostMs, counting them lost.
    static void ExpireOutstanding_(Measured& m, uint64_t nowMs);
    // Start a fresh 1 Hz reporting window at `nowMs`.
    static void OpenWindow_(Measured& m, uint64_t nowMs, int64_t delivered, uint64_t recv);

    Slot slots_[kSlots];

    // The sample-cost accounting, session-wide (the reads are one pass over every live slot).
    uint64_t costUs_ = 0;
    int      costPasses_ = 0;   // passes over the slot array
    int      costLinks_ = 0;    // connections actually read, summed over those passes
    uint64_t costWindowMs_ = 0;
    bool     anyBusy_ = false;
};

}  // namespace coop::net
