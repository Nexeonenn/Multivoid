// coop/net/send_rate_control.cpp -- see coop/net/send_rate_control.h.

#include "coop/net/send_rate_control.h"

#include "ue_wrap/core/log.h"

namespace coop::net {

void SendRateControl::Reset() {
    // Session::Start, with no net thread yet: the reset is performed here rather than armed, so a
    // session that never connects leaves nothing behind either.
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        s.queuedBytes.store(0, std::memory_order_relaxed);
        s.recvBytes.store(0, std::memory_order_relaxed);
        s.resetArmed.store(false, std::memory_order_relaxed);
        s.m = Measured{};
    }
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = 0;
    anyBusy_ = false;
}

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

    // Quiet while the link is idle, plus one closing line on the falling edge so the last state of
    // a finished transfer is in the log.
    if (m.sawTraffic || m.wasBusy) {
        UE_LOGI("send_rate[slot %d]: goodput=%lld B/s (peak %lld B/s) recv=%lld B/s "
                "gnsRate=%d B/s rtt=%d ms win=%d/%d/%d (min/avg/max) rttMin=%d queueDelay=%d "
                "ping=%d probes sent=%d echo=%d refused=%d lost=%d stray=%d skipped=%d "
                "echoRefused=%d pendRel=%d unacked=%d pendUnrel=%d backlog=%zu "
                "queued=%lld delivered=%lld recvd=%llu%s",
                slot, static_cast<long long>(goodput), static_cast<long long>(m.peakBps),
                static_cast<long long>(recvBps), m.last.gnsRateBps, m.rttLastMs,
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
        const int rtt = static_cast<int>(nowMs - sentMs);
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
        UE_LOGI("send_rate: %d pass(es) over %d link sample(s) in %llu us during the last second "
                "(%llu us per link sampled)", costPasses_, costLinks_,
                static_cast<unsigned long long>(costUs_),
                static_cast<unsigned long long>(costUs_ / static_cast<uint64_t>(costLinks_)));
    costUs_ = 0;
    costPasses_ = 0;
    costLinks_ = 0;
    costWindowMs_ = nowMs;
    anyBusy_ = false;
}

}  // namespace coop::net
