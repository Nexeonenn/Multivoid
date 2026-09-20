// coop/net/send_rate_control.cpp -- see coop/net/send_rate_control.h.

#include "coop/net/send_rate_control.h"

#include "ue_wrap/core/log.h"

#include <cmath>

namespace coop::net {

void SendRateControl::Reset(bool controlEnabled) {
    controlEnabled_ = controlEnabled;
    // Session::Start, with no net thread yet: the reset is performed here rather than armed, so a
    // session that never connects leaves nothing behind either.
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        s.queuedBytes.store(0, std::memory_order_relaxed);
        s.recvBytes.store(0, std::memory_order_relaxed);
        s.resetArmed.store(false, std::memory_order_relaxed);
        s.m = Measured{};
        s.m.rateBps = s.m.rateWritten = StartRateBps();
    }
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = 0;
    anyBusy_ = false;
    rateWriteUs_ = 0;
    rateWriteCount_ = 0;
}

int64_t SendRateControl::RateForThrottle(int throttle) {
    // THE ACTUATOR, and the port's larger divergence from `reference/enet/peer.c`. ENet's
    // packetThrottle is a fraction of a budget the APPLICATION CONFIGURES -- host.c's
    // enet_host_bandwidth_throttle turns an outgoingBandwidth the caller supplied into each peer's
    // limit -- and it scales an ack-clocked window by packetThrottle/SCALE (`protocol.c:1472`).
    // We have no configured budget, and refusing to ask the player for one is the design's own
    // rule; not knowing the link is the defect this controller answers. So the same 0..32 index
    // maps GEOMETRICALLY onto a rate ladder instead of linearly onto a budget. ENet's +-2 step is
    // kept unchanged, which makes one step a factor of (kCeilingBps/kFloorBps)^(2/32) = 1.46 and
    // traverses the ladder end to end in ENet's own 16 steps.
    if (throttle <= 0) return kFloorBps;
    if (throttle >= kThrottleScale) return kCeilingBps;
    const double span = static_cast<double>(kCeilingBps) / static_cast<double>(kFloorBps);
    return static_cast<int64_t>(static_cast<double>(kFloorBps) *
                                std::pow(span, static_cast<double>(throttle) / kThrottleScale));
}


int64_t SendRateControl::StartRateBps() { return RateForThrottle(kStartThrottle); }

void SendRateControl::FreeSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    // Arm only, and touch neither counter. Clearing them here would race the net thread between its
    // arm test and its own read of the totals -- two separate operations, which no fence can join --
    // and the sample in that window reads a zeroed total against a live window, i.e. a hugely
    // negative delivery. The whole reset happens on the net thread, in Take_.
    slots_[slot].resetArmed.store(true, std::memory_order_release);
}

SendRateControl::Measured& SendRateControl::Take_(int slot) {
    Slot& s = slots_[slot];
    if (s.resetArmed.exchange(false, std::memory_order_acquire)) {
        s.queuedBytes.store(0, std::memory_order_relaxed);
        s.recvBytes.store(0, std::memory_order_relaxed);
        s.m = Measured{};
        s.m.rateBps = s.m.rateWritten = StartRateBps();
    }
    return s.m;
}

void SendRateControl::NoteReliableQueued(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].queuedBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void SendRateControl::NoteReliableReceived(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].recvBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void SendRateControl::NoteEchoRefused(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    ++Take_(slot).echoesRefused;
}

int SendRateControl::RttMinFor_(const Measured& m, uint64_t nowMs) {
    const uint64_t sec = nowMs / 1000;
    int best = -1;
    for (int i = 0; i < kRttMinBuckets; ++i) {
        // An empty bucket has stamp 0; one older than the window is stale and ignored rather than
        // cleared, so no rotation pass is needed.
        if (m.rttBucketSec[i] == 0) continue;
        if (m.rttBucketSec[i] + kRttMinBuckets <= sec) continue;
        if (best < 0 || m.rttBucketMin[i] < best) best = m.rttBucketMin[i];
    }
    return best;
}

void SendRateControl::ExpireOutstanding_(Measured& m, uint64_t nowMs) {
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0) continue;
        if (nowMs - m.out[i].sentMs <= kProbeLostMs) continue;
        m.out[i].token = 0;
        ++m.probesLost;
    }
}

void SendRateControl::OpenWindow_(Measured& m, uint64_t nowMs, int64_t delivered, uint64_t recv) {
    m.windowMs = nowMs;
    m.windowDelivered = delivered;
    m.windowRecv = recv;
    m.peakBps = 0;
    m.sawTraffic = false;
    // The last round trip belongs to the window it was measured in. Carried across, a link that
    // stopped answering would keep republishing one reading as though it were fresh, and the
    // queueDelay derived from it with it.
    m.rttLastMs = -1;
    m.rttMinWindow = -1;
    m.rttMaxWindow = -1;
    m.rttSumWindow = 0;
    m.rttCountWindow = 0;
    m.probesSent = 0;
    m.probesReplied = 0;
    m.probesRefused = 0;
    m.probesLost = 0;
    m.probesUnknown = 0;
    m.probesSkipped = 0;
    m.echoesRefused = 0;
    m.openCount = 0;
    m.accelCount = 0;
    m.decelCount = 0;
    m.deadCount = 0;
    m.holdCount = 0;
    m.rateWrites = 0;
}

void SendRateControl::Decide_(Measured& m, int rtt) {
    // `enet_peer_throttle`, ported from `reference/enet/peer.c:63-91` (MIT; THIRD-PARTY-NOTICES.md
    // "ENet (ported algorithms)"). ENet runs it on every acknowledgement; we run it on every probe
    // reply, which is the same event -- a round trip closing -- at 10 Hz instead of at packet rate.

    // DIVERGENCE 1, idle. ENet's throttle bounds an ack-clocked window, so a link nobody is using
    // cannot be overdriven by opening it; ours is a RATE, and a rate accelerated on unloaded probes
    // is the rate the NEXT burst opens at -- root A, rebuilt by the thing that was meant to end it.
    // So an idle link decides nothing. `peer.c` has no equivalent because it needs none.
    if (m.demand < kIdleDemandBytes) { ++m.holdCount; return; }
    // There is always a snapshot to compare against by the time this runs: Decide_ is reached only
    // once a round trip has closed, and the epoch block below sets rttBaseMs on that very first one
    // (its `throttleEpochMs == 0` disjunct). ENet needs no equivalent either -- it seeds
    // `lastRoundTripTime` with a pessimistic 500 ms at connect (`peer.c:420`) instead.

    if (m.rttBaseMs <= m.rttBaseVarMs && m.rttBaseMs <= kSignalFloorMs) {
        // `peer.c:65-68` -- the delay signal is unusable, so OPEN rather than brake.
        // DIVERGENCE 2, the second test. ENet's condition alone is also true of a link DROWNING in
        // queue, where baseline and variance are both huge, and opening there would accelerate
        // exactly the 17x overdrive measured costing a thin uplink 71% of itself. The bound is the
        // instrument's own floor: an echo waits up to one 5 ms net-thread pass on each side, so
        // under ~10 ms the probe truly cannot discriminate, while above it a large variance means
        // a troubled link rather than an unmeasurable one.
        m.throttle = kThrottleScale;
        ++m.openCount;
    } else if (rtt <= m.rttBaseMs) {                        // `peer.c:70-78`
        m.throttle += kThrottleAccel;
        if (m.throttle > kThrottleScale) m.throttle = kThrottleScale;
        ++m.accelCount;
    } else if (rtt > m.rttBaseMs + 2 * m.rttBaseVarMs) {    // `peer.c:80-88`
        // ENet writes this as `if (throttle > decel) throttle -= decel; else throttle = 0;`
        // because its field is enet_uint32 and would wrap; on a signed int the two forms are
        // identical for every input, so the clamp is written directly.
        m.throttle -= kThrottleDecel;
        if (m.throttle < 0) m.throttle = 0;
        ++m.decelCount;
    } else {
        ++m.deadCount;   // `peer.c:90` -- a real dead band, and the reason the rate settles at all
        return;
    }
    m.rateBps = RateForThrottle(m.throttle);
}

void SendRateControl::Throttle_(Measured& m, int rtt, uint64_t nowMs) {
    // The estimator around the law, ported from `reference/enet/protocol.c:874-913`. Order matters
    // and is ENet's: the decision is taken on the raw sample BEFORE smoothing absorbs it
    // (`:876` precedes `:878`), so a round trip that just doubled is judged against a baseline it
    // has not yet moved.
    if (m.rttSmoothMs >= 0) Decide_(m, rtt);   // `:874` guards on lastReceiveTime; ours on the sentinel

    if (m.rttSmoothMs < 0) {
        // `:893-897`, the first sample. ENet seeds from a 500 ms default and lets the accumulators
        // below take the minimum against it; with an explicit "nothing known" sentinel the faithful
        // equivalent is to seed all four from the reading itself.
        m.rttSmoothMs     = rtt;
        m.rttVarMs        = (rtt + 1) / 2;
        m.rttLowestMs     = rtt;
        m.rttVarHighestMs = m.rttVarMs;
    } else {
        m.rttVarMs -= m.rttVarMs / 4;                       // `:878`
        if (rtt >= m.rttSmoothMs) {                         // `:880-885`
            const int diff = rtt - m.rttSmoothMs;
            m.rttVarMs    += diff / 4;
            m.rttSmoothMs += diff / 8;
        } else {                                            // `:886-891`
            const int diff = m.rttSmoothMs - rtt;
            m.rttVarMs    += diff / 4;
            m.rttSmoothMs -= diff / 8;
        }
        if (m.rttSmoothMs < m.rttLowestMs) m.rttLowestMs = m.rttSmoothMs;       // `:899-900`
        if (m.rttVarMs > m.rttVarHighestMs) m.rttVarHighestMs = m.rttVarMs;     // `:902-903`
    }

    // `:905-913`. This is what makes the baseline a BASELINE rather than a running average: the law
    // compares against the LOWEST smoothed round trip of the previous interval and the HIGHEST
    // variance in it, and both accumulators restart from the current reading -- so a queue that
    // drains inside an interval is seen, and one that does not is what the next snapshot reports.
    if (m.throttleEpochMs == 0 || nowMs - m.throttleEpochMs >= kThrottleIntervalMs) {
        m.rttBaseMs       = m.rttLowestMs;
        m.rttBaseVarMs    = m.rttVarHighestMs > 1 ? m.rttVarHighestMs : 1;   // ENET_MAX(.., 1)
        m.rttLowestMs     = m.rttSmoothMs;
        m.rttVarHighestMs = m.rttVarMs;
        m.throttleEpochMs = nowMs;
    }
}

int64_t SendRateControl::PendingRateWrite(int slot) {
    if (!controlEnabled_ || slot < 0 || slot >= kSlots) return -1;
    Measured& m = Take_(slot);
    if (m.rateBps <= 0 || m.rateBps == m.rateWritten) return -1;
    return m.rateBps;
}

void SendRateControl::NoteRateWritten(int slot, int64_t bps, uint64_t us) {
    if (slot < 0 || slot >= kSlots || bps <= 0) return;
    Measured& m = Take_(slot);
    m.rateWritten = bps;
    ++m.rateWrites;
    rateWriteUs_ += us;
    ++rateWriteCount_;
}

void SendRateControl::Sample(int slot, const LinkSample& in, uint64_t nowMs) {
    if (slot < 0 || slot >= kSlots) return;
    Measured& m = Take_(slot);
    Slot& s = slots_[slot];

    const int64_t queued = static_cast<int64_t>(s.queuedBytes.load(std::memory_order_relaxed));
    const uint64_t recv = s.recvBytes.load(std::memory_order_relaxed);
    // Delivery: our count of the bytes GNS took, less what it still holds. GNS keeps both pending
    // totals in its OWN units -- it grows each message by a 1 to 6 byte reliable-stream header before
    // counting it -- so this reads low by that header times the messages currently in flight. The
    // term is zero at quiescence and does not accumulate, but it moves with the in-flight message
    // COUNT, so a burst of small reliables perturbs a one-second reading by a few tens of KB where a
    // chunked blob perturbs it by a few hundred bytes. Bytes still in our own backlog were never
    // handed to GNS and appear in neither term; the admission exchange runs before a slot exists and
    // is not counted at all, a constant that differentiating removes.
    const int64_t delivered = queued - in.pendingReliable - in.sentUnackedReliable;
    m.demand = static_cast<int64_t>(in.pendingReliable) +
               static_cast<int64_t>(in.pendingUnreliable) +
               static_cast<int64_t>(in.backlogBytes);
    m.last = in;
    // Busy in EITHER direction. A link with queued work of its own is busy, and so is one merely
    // delivering to us at megabytes a second: the receive counter is the second, independent
    // measurement a sender's goodput estimate gets checked against, and a downloading joiner has
    // nothing queued of its own at all.
    m.busy = m.windowMs != 0 &&
             (m.demand >= kIdleDemandBytes ||
              recv - m.windowRecv >= static_cast<uint64_t>(kIdleDemandBytes));
    if (m.busy) {
        m.sawTraffic = true;
        anyBusy_ = true;
    }

    // The fastest sample interval inside the reported second: a link whose average hides a burst
    // says so here.
    if (m.havePrev && nowMs > m.prevSampleMs) {
        const int64_t bps = (delivered - m.prevDelivered) * 1000 /
                            static_cast<int64_t>(nowMs - m.prevSampleMs);
        if (bps > m.peakBps) m.peakBps = bps;
    }
    m.prevDelivered = delivered;
    m.prevSampleMs = nowMs;
    m.havePrev = true;

    ExpireOutstanding_(m, nowMs);

    if (m.windowMs == 0 || nowMs < m.windowMs) {
        OpenWindow_(m, nowMs, delivered, recv);
        return;
    }
    const uint64_t dt = nowMs - m.windowMs;
    if (dt < 1000) return;

    const int64_t goodput = (delivered - m.windowDelivered) * 1000 / static_cast<int64_t>(dt);
    const int64_t recvBps =
        static_cast<int64_t>(recv - m.windowRecv) * 1000 / static_cast<int64_t>(dt);
    const int rttMin = RttMinFor_(m, nowMs);
    const int rttAvg = m.rttCountWindow > 0
                           ? static_cast<int>(m.rttSumWindow / m.rttCountWindow)
                           : -1;
    // How much of the last round trip is queue: the whole point of timing our own probe, since
    // GNS's min-filtered ping cannot show it.
    const int queueDelay = (m.rttLastMs >= 0 && rttMin >= 0) ? m.rttLastMs - rttMin : -1;

    // NO BANDWIDTH LIMIT IS COMPUTED HERE, and the empty space is the finding. ENet's delay law
    // moves the throttle INSIDE `packetThrottleLimit`, which `enet_host_bandwidth_throttle`
    // (`reference/enet/host.c:376-407`) derives by dividing a bandwidth the APPLICATION CONFIGURED
    // by what the peers offered. Ported with measured goodput in place of that configured number
    // it is circular -- our own rate caps goodput, so the limit can never permit more than the rate
    // that produced it -- and it seized: at rung 1 of 32 on a link carrying eight times that, in
    // both regimes, whether the divisor was the last second or the interval's best. The margin
    // only moves which rung it seizes at. A round trip taken on the High lane is not the signal
    // either, since the scheduler serves that lane BEFORE the Bulk queue the transfer is standing
    // in -- which is why the delay law above reaches this link's ceiling and keeps accelerating.

    // Quiet while the link is idle, plus one closing line on the falling edge so the last state of
    // a finished transfer is in the log.
    if (m.sawTraffic || m.wasBusy) {
        UE_LOGI("send_rate[slot %d]: goodput=%lld B/s (peak %lld B/s) recv=%lld B/s "
                "rate=%lld B/s thr=%d/%d gnsRate=%d B/s srtt=%d var=%d base=%d baseVar=%d "
                "law open=%d acc=%d dec=%d dead=%d hold=%d writes=%d "
                "rtt=%d ms win=%d/%d/%d (min/avg/max) rttMin=%d queueDelay=%d "
                "ping=%d probes sent=%d echo=%d refused=%d lost=%d stray=%d skipped=%d "
                "echoRefused=%d pendRel=%d unacked=%d pendUnrel=%d backlog=%zu "
                "queued=%lld delivered=%lld recvd=%llu%s",
                slot, static_cast<long long>(goodput), static_cast<long long>(m.peakBps),
                static_cast<long long>(recvBps),
                static_cast<long long>(m.rateBps), m.throttle, kThrottleScale, m.last.gnsRateBps,
                m.rttSmoothMs, m.rttVarMs, m.rttBaseMs, m.rttBaseVarMs,
                m.openCount, m.accelCount, m.decelCount, m.deadCount, m.holdCount, m.rateWrites,
                m.rttLastMs,
                m.rttMinWindow, rttAvg, m.rttMaxWindow, rttMin, queueDelay, m.last.gnsPingMs,
                m.probesSent, m.probesReplied, m.probesRefused, m.probesLost, m.probesUnknown,
                m.probesSkipped, m.echoesRefused,
                m.last.pendingReliable, m.last.sentUnackedReliable, m.last.pendingUnreliable,
                m.last.backlogBytes, static_cast<long long>(queued),
                static_cast<long long>(delivered),
                static_cast<unsigned long long>(recv),
                m.sawTraffic ? "" : " (idle)");
    }
    m.wasBusy = m.sawTraffic;
    OpenWindow_(m, nowMs, delivered, recv);
}

bool SendRateControl::NextProbe(int slot, uint64_t nowMs, LinkProbePayload& out) {
    if (slot < 0 || slot >= kSlots) return false;
    Measured& m = Take_(slot);
    // A busy link is probed ten times a second, an idle one once -- and the idle case is what gives
    // rttMin an UNLOADED reading to be the minimum of. Probing only under load would make every
    // sample a loaded one, and a minimum over loaded samples absorbs the very queue the baseline
    // exists to expose: the defect that disqualifies the transport's own min-filtered ping.
    const uint64_t interval = m.busy ? kProbeIntervalMs : kIdleProbeIntervalMs;
    if (m.lastProbeMs != 0 && nowMs - m.lastProbeMs < interval) return false;
    ExpireOutstanding_(m, nowMs);
    int free = -1;
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0) { free = i; break; }
    }
    // Every slot in flight and none answered: a further probe would ask nothing these have not
    // already asked. Counted, because on a link whose round trip outruns the ring's span the skip is
    // why the readings thin out.
    if (free < 0) {
        ++m.probesSkipped;
        return false;
    }
    do { ++m.probeCounter; } while (m.probeCounter == 0);
    m.out[free].token = m.probeCounter;
    m.out[free].sentMs = nowMs;
    m.lastProbeMs = nowMs;
    ++m.probesSent;
    out.token = m.probeCounter;
    return true;
}

void SendRateControl::NoteProbeRefused(int slot, uint32_t token) {
    if (slot < 0 || slot >= kSlots || token == 0) return;
    Measured& m = Take_(slot);
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token != token) continue;
        m.out[i].token = 0;
        // It never went out, so it is not a probe this window sent; counted as refused instead.
        if (m.probesSent > 0) --m.probesSent;
        ++m.probesRefused;
        return;
    }
}

void SendRateControl::NoteProbeReply(int slot, const LinkProbePayload& reply, uint64_t nowMs) {
    if (slot < 0 || slot >= kSlots) return;
    Measured& m = Take_(slot);
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0 || m.out[i].token != reply.token) continue;
        // Both stamps come from one monotonic clock on one thread, so the difference is a duration
        // by construction and needs no guard; a guard here would also drop the probe out of every
        // counter and quietly stop the ledger from adding up.
        const uint64_t sentMs = m.out[i].sentMs;
        m.out[i].token = 0;
        // `protocol.c:872` floors the sample at 1 ms before anything reads it; on loopback ours
        // genuinely measures 0, and an unfloored 0 would make the accelerate test trivially true.
        const int raw = static_cast<int>(nowMs - sentMs);
        const int rtt = raw < 1 ? 1 : raw;
        m.rttLastMs = rtt;
        ++m.probesReplied;
        m.rttSumWindow += rtt;
        ++m.rttCountWindow;
        if (m.rttMinWindow < 0 || rtt < m.rttMinWindow) m.rttMinWindow = rtt;
        if (rtt > m.rttMaxWindow) m.rttMaxWindow = rtt;
        const uint64_t sec = nowMs / 1000;
        const int b = static_cast<int>(sec % kRttMinBuckets);
        if (m.rttBucketSec[b] != sec) {
            m.rttBucketSec[b] = sec;
            m.rttBucketMin[b] = rtt;
        } else if (rtt < m.rttBucketMin[b]) {
            m.rttBucketMin[b] = rtt;
        }
        // A closed round trip is the law's only input, so this is where it runs -- per reply, as
        // ENet runs per acknowledgement, and never on a pass that measured nothing.
        Throttle_(m, rtt, nowMs);
        return;
    }
    // An echo for a token we do not have outstanding: a reply that came back after its probe was
    // retired as lost, or a peer echoing something we never minted. Counted, never measured.
    ++m.probesUnknown;
}

void SendRateControl::NoteSampleCost(uint64_t us, int links, uint64_t nowMs) {
    costUs_ += us;
    ++costPasses_;
    costLinks_ += links;
    if (costWindowMs_ == 0 || nowMs < costWindowMs_) {
        costWindowMs_ = nowMs;
        return;
    }
    if (nowMs - costWindowMs_ < 1000) return;
    if (anyBusy_ && costLinks_ > 0)
        // The rate WRITE is reported beside the read because only the read had ever been measured:
        // a config write per decision at 10 Hz per slot is a claim of its own.
        UE_LOGI("send_rate: %d pass(es) over %d link sample(s) in %llu us during the last second "
                "(%llu us per link sampled); %d rate write(s) in %llu us",
                costPasses_, costLinks_,
                static_cast<unsigned long long>(costUs_),
                static_cast<unsigned long long>(costUs_ / static_cast<uint64_t>(costLinks_)),
                rateWriteCount_, static_cast<unsigned long long>(rateWriteUs_));
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = nowMs;
    anyBusy_ = false;
    rateWriteUs_ = 0;
    rateWriteCount_ = 0;
}

}  // namespace coop::net
