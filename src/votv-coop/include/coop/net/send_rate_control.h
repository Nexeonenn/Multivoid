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
// From those two numbers this module also DECIDES the rate, and the session writes it to the
// connection. The law is anchored to the delivery it measures: a rate may not stand far above the
// bytes the peer is acknowledging, and it brakes when the bytes in flight grow past a few hundred
// milliseconds of that delivery. Both terms are byte counters, which is the whole point -- see the
// block below for the two laws that were built and refuted before it.
//
// This is now the ONLY writer of a send rate. The 1 MiB/s global floor that used to stand under
// every connection is deleted (`coop/net/session_start`), so a link is paced by what it measures or
// by nothing: `net.ratecontrol=0` is a drill's control arm and leaves the transport at its own
// stock 256 KB/s, and `net.sendrate_kbs` pins a fixed rate for an experiment. Neither is a
// fallback to the old behaviour, because the old behaviour was the defect.

#pragma once

#include "coop/net/protocol.h"             // LinkProbePayload
#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>

// WHY A BYTE SIGNAL, AND WHAT THE TWO REFUTED LAWS HAD IN COMMON. Both of them judged the link by
// comparing a DELAY against a reference computed from recent DELAYS -- LEDBAT against a running
// delay base, ENet's peer throttle against the previous interval's lowest smoothed round trip
// (`reference/enet/protocol.c:908-911`). A standing queue moves that reference, so "better than
// baseline" stops meaning anything exactly when it matters. ENet's port was measured doing it: the
// baseline went from 18 ms to a median of 1,545 ms, its accelerate branch turned true of nearly
// every reading, and the rate ratcheted to the ceiling for 180 of 210 seconds.
//
// The replacement reads the same logs and asks what was ALREADY on the line. Two quantities, both
// byte counters, neither with a reference that can drift:
//
//   served    -- bytes the peer acknowledged plus unreliable bytes handed over, per second. A queue
//                cannot inflate it: no amount of buffering makes a link deliver more than it can.
//   inflight  -- `m_cbSentUnackedReliable` divided by the acknowledged rate, i.e. Little's law read
//                backwards: the milliseconds of delivery standing on the wire. Compared against a
//                fixed target, never against its own past.
//
// Measured on the archived closed-loop run, the two regimes separate with no overlap: while the
// link was healthy `inflight` ran a median of 60 ms (p90 163) and afterwards a median of 13,055
// (p10 1,458), and the rate the law applied stood a median of 442x above the delivery it was
// printing on the same line. Neither term needs the probe, so the probe's own confound -- it rides
// the High lane, which the scheduler serves ahead of the Bulk queue a transfer stands in -- is
// structurally absent here. It stays as a DIAGNOSTIC, which is what it now is.
//
// Threading: the three byte counters are atomics, written at the send and receive choke points from
// whichever thread reached them, and a fourth atomic ARMS a slot's teardown from whichever thread
// tore it down. Everything else is net-thread-only, driven from the session loop.

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

    // ---- The control law's constants ----

    // The envelope. The floor is an honest minimum -- a join on a link that thin is slow, not
    // broken. The ceiling is the save pump's own output, which is the biggest producer in the tree:
    // kChunksPerTick = 4 (coop/save/save_transfer.cpp:151) x kSaveChunkBytes = 56 KiB
    // (coop/net/protocol.h:1257) at 60 Hz. A rate above that paces nothing that exists.
    static constexpr int64_t kFloorBps   = 32 * 1024;
    static constexpr int64_t kCeilingBps = 4LL * 56 * 1024 * 60;

    // Where a link opens, for the connect-time write: the window between the transport's
    // ping-derived guess and this controller's first decision belongs to nobody otherwise. Opening
    // WIDE is precisely the overdrive this controller exists to end -- 17x into a thin link was
    // measured costing 71% of it -- and opening at the floor makes every join on a good link crawl.
    // It opens at 4.5x the floor, about 1% of the ceiling: measured reaching the ceiling in three
    // seconds on a link that has the headroom, and inside a thin link's capacity rather than 17x
    // outside it. The value is also the rung the refuted ladder happened to open on, kept because
    // two runs measured it and nothing argues for moving it.
    static constexpr int64_t kStartRateBps = 145 * 1024;

    // THE ANCHOR, and the one guard that would have stopped both refuted laws on its own. A rate
    // may not stand above this multiple of the delivery it is measured against, while there is
    // reliable work to measure. The archived refuted run sat at a median of 442x.
    //
    // The margin above 1.0 is what probes for capacity the link has not yet been asked for, and it
    // is paid for in LOSS: pacing at k times what the link carries offers (k-1)/k of every packet
    // to a bottleneck that cannot take it. Measured at k=2 on the drop-policed rig -- goodput 95.3%
    // of nominal, but the peer reported `qual=51/100`, i.e. about half of what we sent had to be
    // retransmitted. That is not free here: reliable RETRANSMISSIONS are gathered before the lane
    // priority loop (§5.2 of the arc doc, `snp.cpp:2476-2547`), so a Bulk retry outranks the lane-0
    // pose datagram this controller exists to protect. 5/4 is the same probe margin BBR uses and
    // the same step the climb takes, so the rate tracks delivery instead of standing above it.
    static constexpr int kOverdriveNum = 5, kOverdriveDen = 4;

    // THE BRAKE and its dead band, in milliseconds of delivery standing on the wire. Measured on
    // the archived closed-loop run: the healthy regime's p90 was 163 ms and the collapsed regime's
    // p10 was 1,458, so a threshold anywhere between flags 93.4% of the collapse and 0.0% of the
    // health. The climb stops where health stops; the brake fires with an order of magnitude of
    // margin; between them the rate is left alone, because a controller with no dead band hunts.
    static constexpr int64_t kQueueClimbMs = 150;
    static constexpr int64_t kQueueBrakeMs = 400;

    // The climb, applied per decision while the brake is silent. The additive term is what carries
    // a rate off the floor, where a purely multiplicative step crawls.
    static constexpr int     kClimbNum = 5, kClimbDen = 4;
    static constexpr int64_t kClimbStepBps = 8 * 1024;

    // The delivery estimate's smoothing over the 10 Hz samples, as a shift: new = old - old/4 +
    // sample/4, a ~400 ms time constant. One 100 ms sample is too noisy to brake on, and a full
    // second is too slow for a join to climb inside.
    static constexpr int kServedEwmaShift = 2;
    // How many samples the ANCHOR takes its maximum over, which is the same span as the EWMA's time
    // constant. The anchor may not read the EWMA, and the reason is a measured regression: while a
    // link is climbing, delivery equals the rate and the EWMA lags it by about half, so an anchor
    // fed by the EWMA clamps BELOW the rate that is already succeeding and fights its own climb --
    // an unpoliced join went from 5 s under no control at all to 13 s under the law, never reaching
    // the ceiling. A maximum over the same span has no lag on the way up, because the newest sample
    // enters it whole, while on the way down it still expires in 400 ms. The two quantities answer
    // different questions: the EWMA asks "what is this link carrying", the maximum asks "what has
    // it just been shown to carry".
    static constexpr int kServedPeakSamples = 1 << kServedEwmaShift;
    // And how many samples that estimate is worth dividing by. An EWMA is not a measurement before
    // its own time constant has passed, and the first sample of a transfer is the worst case there
    // is: the save pump offers 13.1 MiB/s, so the send buffer is already deep while the delivery
    // estimate is one 100 ms reading old, and `inflight` computed from that pair reads as a stalled
    // link. Measured: without this the opening second took one spurious brake to the floor on a
    // link with megabytes of headroom. Until the estimate is warm the link holds at its opening
    // rate, which is the one state the law has no reason to act on.
    static constexpr int kServedWarmupSamples = 1 << kServedEwmaShift;

    // The rate a connection opens at, for the connect-time write.
    static int64_t StartRateBps();

    // Session::Start: a new session measures from zero. Called before the net thread exists.
    // `controlEnabled` false leaves every link at whatever the transport was configured with, and
    // the measurements still run: that is the A1 behaviour, kept as the before-picture.
    void Reset(bool controlEnabled);
    // Whether this session's links are paced by the law. The session's own resolved answer, so a
    // connection being tuned asks THIS rather than re-reading an ini that resolves live.
    bool Enabled() const { return controlEnabled_; }
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
    // Unreliable wire bytes GNS accepted. Counted because the RATE paces the whole connection while
    // acknowledged delivery can only ever cover the reliable half: anchoring a connection-wide rate
    // to a reliable-only measurement would clamp an ordinary play session, whose traffic is pose and
    // voice, down to the floor. There is no acknowledgement to count against these, so the offer is
    // what stands in -- exact for the anchor's purpose, which is to know what the rate is FOR.
    void NoteUnreliableQueued(int slot, int bytes);
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
        int    gnsRateBps          = 0;  // m_nSendRateBytesPerSecond: what it is pacing at now
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

    // The rate this slot's link should now be paced at, or -1 when there is nothing to write --
    // control disabled, or the law left the rate where the connection already has it. Net thread,
    // and a PEEK: the decision is not spent until `NoteRateWritten` says it reached the transport.
    // Latching here instead would retire a decision a failed write never delivered, and the law
    // offers nothing new while its decision leaves the rate where it is -- and the dead band means
    // it can leave it there for a long time -- so one missed write would strand the link.
    int64_t PendingRateWrite(int slot);
    // The write landed. `us` is what it cost, folded into the per-second cost line beside the
    // status read's, because a config write per decision is a claim of its own and only the READ
    // had ever been measured. Net thread.
    void NoteRateWritten(int slot, int64_t bps, uint64_t us);

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

        // ---- The law's state: two smoothed byte rates and what they decided ----
        // Acknowledged delivery, smoothed over the 10 Hz samples. This is the term `inflight` is
        // divided by, so it is RELIABLE-ONLY -- `m_cbSentUnackedReliable` is reliable-only too, and
        // dividing one traffic class by another would measure neither.
        int64_t  relBps    = 0;
        // The same, plus unreliable bytes handed over: what the RATE is actually for. This is the
        // term the anchor bounds against, and the distinction between the two is the answer to the
        // objection that sank the first law's trigger -- goodput counts reliable delivery while the
        // rate paces everything, so each comparison here is given the traffic it belongs to.
        int64_t  servedBps = 0;
        int      servedSamples = 0;    // until the EWMA is warm, neither guard may bind
        // The anchor's own view of the same quantity: the last kServedPeakSamples raw readings, of
        // which it takes the maximum. See kServedPeakSamples for why this may not be the EWMA.
        int64_t  servedRing[kServedPeakSamples]{};
        int      servedRingIdx = 0;
        int64_t  prevUnrel  = 0;       // the unreliable counter at the previous sample
        // The last decision's inputs, kept for the reported line: a field log that cannot show why
        // the rate moved is the state this whole arc started in.
        int64_t  inflightMs = 0;
        // What the law wants, and what the connection was last told, so a write costs one per move.
        // Both open at the rate `TuneConnection` already wrote, NOT at zero: a link whose demand
        // never reaches kIdleDemandBytes decides nothing -- ordinary play is a few KB/s of pose and
        // voice -- and zeroes here would report a rate of 0 while the transport paced the opening
        // one. Set by Reset and by Take_'s armed teardown.
        int64_t  rateBps     = 0;
        int64_t  rateWritten = 0;
        // Which branch the law took, per reported second. `anchor` counts the decisions the
        // overdrive bound had to pull back, which is the number that says whether it is load-bearing.
        int      climbCount = 0, brakeCount = 0, anchorCount = 0, deadCount = 0, holdCount = 0;
        int      rateWrites = 0;
    };

    struct Slot {
        // The three cumulative counters, written from any thread at the choke points.
        std::atomic<uint64_t> queuedBytes{0};
        std::atomic<uint64_t> unrelBytes{0};
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

    // The law, run once per 10 Hz sample against the two smoothed rates. It runs on the SAMPLE
    // cadence and not on the reported second, because a join has to climb inside its own download;
    // it does NOT run per round trip, because a round trip is no longer an input.
    static void Steer_(Measured& m);

    Slot slots_[kSlots];
    // Whether the law's decisions are handed to the transport at all.
    bool controlEnabled_ = false;

    // The sample-cost accounting, session-wide (the reads are one pass over every live slot).
    uint64_t costUs_ = 0;
    int      costPasses_ = 0;   // passes over the slot array
    int      costLinks_ = 0;    // connections actually read, summed over those passes
    uint64_t costWindowMs_ = 0;
    bool     anyBusy_ = false;
    // The config WRITE's own cost, which the read's measurement never covered.
    uint64_t rateWriteUs_ = 0;
    int      rateWriteCount_ = 0;
};

}  // namespace coop::net
