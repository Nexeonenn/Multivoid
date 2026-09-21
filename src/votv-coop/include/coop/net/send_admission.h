// coop/net/send_admission.h -- who may put reliable bytes into a connection's send buffer.
//
// The transport admits a message only while a connection's pending bytes plus that message fit the
// send buffer, and that sum is connection-wide: a bulk stream at the brim refuses the unreliable
// pose and voice datagrams beside it, and the probe that times the path they ride. This rule bounds
// OUR contribution to that brim -- every reliable send but the probe pair stops short of a reserve.
// It is NOT a guarantee of room: the transport re-queues a NACKed segment from unacked back into
// pending with no admission check, so on a lossy link pending passes the buffer size from outside
// while we add nothing. A send this rule refuses is never lost -- it goes to the backlog, or it is
// the save pump's pacing signal, which is what a full buffer already meant to it.
//
// The occupancy compared is an estimate, re-anchored on the transport's own pending total at each
// link sample with every byte we hand a connection added on the spot. It reads high when a drain
// went unseen (the safe direction) and low by a bounded amount -- the stream header the transport
// adds after its own check, and the skew between two loads -- both corrected at the next anchor.
//
// THE SECOND BOUND IS IN TIME, AND ONLY THE UNQUEUED PATH PAYS IT. The rule above is in BYTES,
// which bounds a queue whose drain rate varies by more than an order of magnitude in nothing at
// all: the same 4 MiB brim measured 5.6 s of queue on one policed joiner and 125.96 s on a slot
// losing a three-way race for one uplink (`a3_evidence/policed512_on_HOST.log`,
// `postfix_conc768_HOST.log`; `pendRel / sendRate`, shipped defaults). Bytes in the transport's
// buffer can be neither retracted nor reordered, so that is how long a cancelled transfer keeps
// spending a host's uplink, and how far behind everything queued after it starts. So a send on
// the path that has NO queue behind it -- where a refusal IS the producer's pacing signal, which
// is what the save pump already reads it as -- is admitted only while this slot's queue is under
// the time the link needs to drain it.
//
// The backlog path keeps the byte rule ALONE, and the reason is not that moving bytes into our own
// queue would be wrong -- reordering them by lane is precisely what that backlog is for. It is that
// its producers are one-shot bursts against a sustained stream: the whole join burst measured
// ~740 KB (`send_backlog.h`), while the world blob is 17 MB offered at 13.1 MB/s, and only the blob
// was ever measured holding the buffer for two minutes. The time bound is aimed at what pinned it.
//
// The shape is MTA's `CLatentSendQueue::DoPulse`
// (`Shared/mods/deathmatch/logic/CLatentSendQueue.cpp:46-69`): a bulk transfer hands the transport
// `rate * elapsed` per pulse and keeps the remainder in its own buffer, which is what makes its
// `CancelSend` free. Our pump already has that buffer (the blob plus its read cursor); what it
// lacked was the meter. DELIBERATE DIVERGENCE: MTA meters by rate*time with a carried remainder
// because RakNet publishes no queue depth, while GNS does (`m_cbPendingReliable`, re-anchored
// here every link sample), so we bound the DEPTH directly -- the same discipline with nothing to
// integrate and so nothing to drift.

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>

namespace coop::net {

// Threading: five atomics per slot, written at the send choke points from whichever thread reached
// them and re-anchored on the net thread; a test costs two acquire loads for the occupancy and, for
// the paced bound, two relaxed ones beside them. No lock on any path.
class SendAdmission {
public:
    // Headroom kept free for the unreliable streams and the probe pair. The worst realistic
    // concurrent unreliable load is ~1.2 KB per 16 ms tick (three peers of voice frames plus their
    // poses), so 64 KB is about 30x margin. A buffer too small to give that away yields a quarter
    // of itself instead, which keeps the rule from starving a drill-pinned buffer outright.
    static constexpr int kReserve = 64 * 1024;
    static constexpr int kMaxReserveFraction = 4;

    // How much of a queue the unqueued path may stand behind, in milliseconds of this link's own
    // MEASURED DELIVERY (`SendRateControl::ServedBps`, not the rate the transport is paced at --
    // see there for what that cost). Two seconds is about four times the worst frame the host's
    // game thread takes between pump passes, so the transport never runs dry waiting for the next
    // one, and it is 63x under the 125.96 s the byte rule alone allowed on a contended uplink.
    // It bounds what is COMMITTED against the delivery measured at that moment and cannot retract:
    // a rate that collapses 30x inside a second re-prices bytes already handed over, which is why
    // a contended arm still shows a tail (section 8j). `[dev] bulk_queue_cap_ms` overrides it;
    // 0 disables the bound, which is the before-picture and the only way to stage one on a single
    // binary.
    static constexpr int64_t kQueueCapMs = 2000;

    // Session::Start: a new session opens every slot empty.
    void Reset();

    // Slot teardown. Any thread: the pair is re-anchored from the transport's own total at the
    // next sample, so a reader that catches the reset half-done costs one wrong verdict on a slot
    // that is going away -- and the transport's own admission check stands behind every send in
    // any case.
    void FreeSlot(int slot);

    // The per-connection send buffer this session configures (the drill knob, or the default).
    // Written when a peer's lanes are configured, read by every test.
    void SetSendBufBytes(int bytes);

    // The time bound's budget, resolved once at Session::Start from `[dev] bulk_queue_cap_ms`.
    // 0 leaves the byte rule alone.
    void SetQueueCapMs(int64_t ms);

    // What this slot's link DELIVERS per second (`SendRateControl::ServedBps`), published from the
    // same link sample that re-anchors the occupancy. Measured delivery, not the rate the transport
    // is paced at: with the law on, GNS returns our own last written rate verbatim through its
    // nMin == nMax branch, which on a policed link stood 1.29x above what the link carried -- so a
    // budget built on it admitted 2.57 s while its name said 2 s, and the metric checking it
    // divided by the same inflated number. Net thread. A non-positive reading is not stored, so
    // the last measured rate stands until the next sample; a zero would switch the bound OFF for
    // this slot rather than tighten it.
    void SetDrainRate(int slot, int64_t bytesPerSec);

    // Re-anchor a slot on the transport's pending total (reliable plus unreliable, the sum the
    // transport itself compares against the buffer size). `readPending` returns that total, or a
    // negative number when the status could not be read, in which case the previous anchor stands.
    // The call order inside is the point of the callback: our own handed count is latched BEFORE
    // the status read, so a send that slips between the two is counted twice -- reading high, the
    // safe direction -- instead of vanishing from the estimate. Net thread.
    template <class ReadPendingFn>
    void Anchor(int slot, ReadPendingFn&& readPending) {
        if (slot < 0 || slot >= kSlots) return;
        const int64_t mark = slots_[slot].handed.load(std::memory_order_acquire);
        const int pending = readPending();
        if (pending < 0) return;
        slots_[slot].floor.store(static_cast<int64_t>(pending) - mark, std::memory_order_release);
    }

    // Bytes the connection accepted, reliable or unreliable. Any thread, at the send choke points.
    void NoteHanded(int slot, int bytes);

    // May `len` more non-urgent bytes enter this slot's buffer? Any thread. A pure predicate: the
    // backlog drain asks it once per queued head and its held-back state is already reported by
    // the episode lines, so only the send paths count a refusal.
    bool Admits(int slot, int len) const;

    // Both bounds, for the send path that has no queue behind it. Tests the queue as it STANDS
    // rather than the sum with `len`: a slot whose queue has drained under the budget always takes
    // one more message, whatever its size, so no message can be too large to ever depart and the
    // depth settles at the budget plus one message rather than deadlocking below it. Any thread.
    bool AdmitsPacedStream(int slot, int len) const;

    // One reliable send this rule held back, counted at the send paths for the diagnostics.
    void NoteRefused(int slot);
    // The count since the last call, for the per-second line.
    uint32_t TakeRefusals(int slot);

    // One send the TIME bound held back, counted apart from the byte rule's refusals. Folding the
    // two would rewrite the meaning of a number the headroom rule's own measurement rests on --
    // and they say different things: a refusal is a buffer with no room, a hold is a producer
    // being paced by a buffer that has plenty.
    void NotePacedHold(int slot);
    uint32_t TakePacedHolds(int slot);

private:
    static constexpr int kSlots = static_cast<int>(coop::players::kMaxPeers);

    struct Slot {
        // The estimate is floor + handed. `floor` is the transport's pending total at the last
        // anchor minus the handed count latched just before it, so adding today's handed count
        // back gives that total plus everything handed since.
        std::atomic<int64_t>  floor{0};
        std::atomic<int64_t>  handed{0};
        // The delivery measured on this connection, and the holds the time bound built from it.
        // Opens at zero, which reads as "not measured yet" and leaves the time bound out of the
        // way until the first link sample publishes one. Kept beside the two int64s above, ahead
        // of the two counters, so the struct packs to 32 bytes with no padding hole.
        std::atomic<int64_t>  drainBps{0};
        std::atomic<uint32_t> refusals{0};
        std::atomic<uint32_t> pacedHolds{0};
    };

    // Does `len` fit the buffer above the reserve, given an occupancy already read?
    bool FitsBuffer_(int64_t estimate, int len) const;

    // The occupancy estimate, read once. Both bounds go through this, so a paced test cannot pair
    // one bound's reading of the pair with the other's.
    int64_t Estimate_(const Slot& s) const {
        // Acquire on both, so a fresh floor cannot be paired with a handed count from before the
        // anchor that produced it -- the one pairing that would read low by more than the header
        // term.
        return s.floor.load(std::memory_order_acquire) + s.handed.load(std::memory_order_acquire);
    }

    Slot slots_[kSlots];
    // One buffer size for every connection: the session configures them all alike.
    std::atomic<int> sendBufBytes_{512 * 1024};
    // The time budget, one for the session for the same reason.
    std::atomic<int64_t> queueCapMs_{kQueueCapMs};
};

}  // namespace coop::net
