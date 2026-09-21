// coop/net/send_admission.cpp -- see coop/net/send_admission.h.

#include "coop/net/send_admission.h"

namespace coop::net {

void SendAdmission::Reset() {
    for (auto& s : slots_) {
        s.handed.store(0, std::memory_order_relaxed);
        s.floor.store(0, std::memory_order_relaxed);
        s.refusals.store(0, std::memory_order_relaxed);
        s.drainBps.store(0, std::memory_order_relaxed);
        s.pacedHolds.store(0, std::memory_order_relaxed);
    }
}

void SendAdmission::FreeSlot(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    // Handed first: the intermediate state that leaves is floor-negative, which reads LOW and so
    // admits, rather than the floor-zero-with-a-live-count state, which would refuse every send to
    // the slot's next occupant until the first anchor.
    slots_[slot].handed.store(0, std::memory_order_relaxed);
    slots_[slot].floor.store(0, std::memory_order_release);
    slots_[slot].refusals.store(0, std::memory_order_relaxed);
    // The rate belonged to the connection that is leaving: the next occupant's own link sample
    // publishes its own, and until it does the time bound stands aside rather than pacing a new
    // connection at a dead one's measurement.
    slots_[slot].drainBps.store(0, std::memory_order_relaxed);
    slots_[slot].pacedHolds.store(0, std::memory_order_relaxed);
}

void SendAdmission::SetSendBufBytes(int bytes) {
    if (bytes > 0) sendBufBytes_.store(bytes, std::memory_order_relaxed);
}

void SendAdmission::SetQueueCapMs(int64_t ms) {
    if (ms >= 0) queueCapMs_.store(ms, std::memory_order_relaxed);
}

void SendAdmission::SetDrainRate(int slot, int64_t bytesPerSec) {
    if (slot < 0 || slot >= kSlots || bytesPerSec <= 0) return;
    slots_[slot].drainBps.store(bytesPerSec, std::memory_order_relaxed);
}

void SendAdmission::NoteHanded(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].handed.fetch_add(bytes, std::memory_order_release);
}

bool SendAdmission::FitsBuffer_(int64_t estimate, int len) const {
    const int buf = sendBufBytes_.load(std::memory_order_relaxed);
    const int reserve = (kReserve < buf / kMaxReserveFraction) ? kReserve
                                                              : buf / kMaxReserveFraction;
    return estimate + len <= static_cast<int64_t>(buf) - reserve;
}

bool SendAdmission::Admits(int slot, int len) const {
    if (slot < 0 || slot >= kSlots || len <= 0) return false;
    return FitsBuffer_(Estimate_(slots_[slot]), len);
}

bool SendAdmission::AdmitsPacedStream(int slot, int len) const {
    if (slot < 0 || slot >= kSlots || len <= 0) return false;
    const Slot& s = slots_[slot];
    // One reading of the occupancy for both bounds: two separate reads could pair a byte verdict
    // with a time verdict taken against different queues.
    const int64_t queued = Estimate_(s);
    if (!FitsBuffer_(queued, len)) return false;
    const int64_t capMs = queueCapMs_.load(std::memory_order_relaxed);
    if (capMs <= 0) return true;  // the bound is off: the byte rule alone, the before-picture
    const int64_t bps = s.drainBps.load(std::memory_order_relaxed);
    if (bps <= 0) return true;    // no delivery measured yet -- nothing to pace against
    // A budget that rounds to nothing would refuse this slot forever, since no queue is below
    // zero. The invariant this rule states -- a drained slot always takes one more message -- has
    // to hold for every rate and every budget, not only for the ones a third-party clamp happens
    // to keep above the rounding floor.
    const int64_t cap = bps * capMs / 1000;
    if (cap <= 0) return true;
    return queued < cap;
}

void SendAdmission::NoteRefused(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    slots_[slot].refusals.fetch_add(1, std::memory_order_relaxed);
}

uint32_t SendAdmission::TakeRefusals(int slot) {
    if (slot < 0 || slot >= kSlots) return 0;
    return slots_[slot].refusals.exchange(0, std::memory_order_relaxed);
}

void SendAdmission::NotePacedHold(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    slots_[slot].pacedHolds.fetch_add(1, std::memory_order_relaxed);
}

uint32_t SendAdmission::TakePacedHolds(int slot) {
    if (slot < 0 || slot >= kSlots) return 0;
    return slots_[slot].pacedHolds.exchange(0, std::memory_order_relaxed);
}

}  // namespace coop::net
