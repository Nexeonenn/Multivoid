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
    costCalls_ = 0;
    costWindowMs_ = 0;
    anyBusy_ = false;
}

void SendRateControl::FreeSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    Slot& s = slots_[slot];
    // Armed before the counters are cleared, so the net thread cannot read a zeroed total against a
    // live measured block; in practice the slot's connection handle is already gone by here, so it
    // samples nothing in between.
    s.resetArmed.store(true, std::memory_order_release);
    s.queuedBytes.store(0, std::memory_order_relaxed);
    s.recvBytes.store(0, std::memory_order_relaxed);
}

SendRateControl::Measured& SendRateControl::Take_(int slot) {
    Slot& s = slots_[slot];
    if (s.resetArmed.exchange(false, std::memory_order_acquire)) s.m = Measured{};
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
    m.rttMinWindow = -1;
    m.rttMaxWindow = -1;
    m.rttSumWindow = 0;
    m.rttCountWindow = 0;
    m.probesSent = 0;
    m.probesReplied = 0;
    m.probesRefused = 0;
    m.probesLost = 0;
    m.probesUnknown = 0;
}

void SendRateControl::Sample(int slot, const LinkSample& in, uint64_t nowMs) {
    if (slot < 0 || slot >= kSlots) return;
    Measured& m = Take_(slot);
    Slot& s = slots_[slot];

    const int64_t queued = static_cast<int64_t>(s.queuedBytes.load(std::memory_order_relaxed));
    const uint64_t recv = s.recvBytes.load(std::memory_order_relaxed);
    // Delivery, in our own byte units: GNS keeps both pending totals in message bytes (m_cbSize for
    // a queued message, and segment sizes sliced out of it), so the difference is the bytes the peer
    // has acknowledged and needs no framing term. Bytes still in our own backlog were never handed
    // to GNS and appear in neither term. The admission exchange runs before a slot exists and is not
    // counted, which offsets this by a constant that differentiating removes.
    const int64_t delivered = queued - in.pendingReliable - in.sentUnackedReliable;
    m.demand = static_cast<int64_t>(in.pendingReliable) +
               static_cast<int64_t>(in.pendingUnreliable) +
               static_cast<int64_t>(in.backlogBytes);
    m.last = in;
    // Busy in EITHER direction. A link with queued work of its own is busy, and so is one merely
    // delivering to us at megabytes a second: the receive counter is the second, independent
    // measurement a sender's goodput estimate gets checked against, and gating the line on send
    // demand alone left a downloading joiner reporting nothing at all.
    if (m.windowMs != 0 &&
        (m.demand >= kIdleDemandBytes ||
         recv - m.windowRecv >= static_cast<uint64_t>(kIdleDemandBytes))) {
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
                "gnsRate=%d B/s rtt=%d ms win=%d/%d/%d rttMin=%d queueDelay=%d ping=%d "
                "probes sent=%d echo=%d refused=%d lost=%d stray=%d "
                "pendRel=%d unacked=%d pendUnrel=%d backlog=%zu queued=%lld acked=%lld "
                "recvd=%llu%s",
                slot, static_cast<long long>(goodput), static_cast<long long>(m.peakBps),
                static_cast<long long>(recvBps), m.last.gnsRateBps, m.rttLastMs,
                m.rttMinWindow, rttAvg, m.rttMaxWindow, rttMin, queueDelay, m.last.gnsPingMs,
                m.probesSent, m.probesReplied, m.probesRefused, m.probesLost, m.probesUnknown,
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
    if (m.demand < kIdleDemandBytes) return false;
    if (m.lastProbeMs != 0 && nowMs - m.lastProbeMs < kProbeIntervalMs) return false;
    ExpireOutstanding_(m, nowMs);
    int free = -1;
    for (int i = 0; i < kMaxOutstanding; ++i) {
        if (m.out[i].token == 0) { free = i; break; }
    }
    // Every slot in flight and none answered: a further probe would measure nothing this one does
    // not already owe an answer for.
    if (free < 0) return false;
    do { ++m.probeCounter; } while (m.probeCounter == 0);
    m.out[free].token = m.probeCounter;
    m.out[free].sentMs = nowMs;
    m.lastProbeMs = nowMs;
    ++m.probesSent;
    out.token = m.probeCounter;
    out.sentMs = static_cast<uint32_t>(nowMs);
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
        const uint64_t sentMs = m.out[i].sentMs;
        m.out[i].token = 0;
        if (nowMs < sentMs) return;  // a clock cannot run backwards; no reading to take
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
    costCalls_ += links;
    if (costWindowMs_ == 0 || nowMs < costWindowMs_) {
        costWindowMs_ = nowMs;
        return;
    }
    if (nowMs - costWindowMs_ < 1000) return;
    if (anyBusy_ && costCalls_ > 0)
        UE_LOGI("send_rate: %d link sample pass(es) in %llu us over the last second "
                "(%llu us per link sampled)", costCalls_,
                static_cast<unsigned long long>(costUs_),
                static_cast<unsigned long long>(costUs_ / static_cast<uint64_t>(costCalls_)));
    costUs_ = 0;
    costCalls_ = 0;
    costWindowMs_ = nowMs;
    anyBusy_ = false;
}

}  // namespace coop::net
