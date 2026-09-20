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

#pragma once

#include "coop/player/players_registry.h"  // kMaxPeers

#include <atomic>
#include <cstdint>

namespace coop::net {

// Threading: two atomics per slot, written at the send choke points from whichever thread reached
// them and re-anchored on the net thread; a test costs two acquire loads and no lock.
class SendAdmission {
public:
    // Headroom kept free for the unreliable streams and the probe pair. The worst realistic
    // concurrent unreliable load is ~1.2 KB per 16 ms tick (three peers of voice frames plus their
    // poses), so 64 KB is about 30x margin. A buffer too small to give that away yields a quarter
    // of itself instead, which keeps the rule from starving a drill-pinned buffer outright.
    static constexpr int kReserve = 64 * 1024;
    static constexpr int kMaxReserveFraction = 4;

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

    // One reliable send this rule held back, counted at the send paths for the diagnostics.
    void NoteRefused(int slot);
    // The count since the last call, for the per-second line.
    uint32_t TakeRefusals(int slot);

private:
    static constexpr int kSlots = static_cast<int>(coop::players::kMaxPeers);

    struct Slot {
        // The estimate is floor + handed. `floor` is the transport's pending total at the last
        // anchor minus the handed count latched just before it, so adding today's handed count
        // back gives that total plus everything handed since.
        std::atomic<int64_t>  floor{0};
        std::atomic<int64_t>  handed{0};
        std::atomic<uint32_t> refusals{0};
    };

    Slot slots_[kSlots];
    // One buffer size for every connection: the session configures them all alike.
    std::atomic<int> sendBufBytes_{512 * 1024};
};

}  // namespace coop::net
