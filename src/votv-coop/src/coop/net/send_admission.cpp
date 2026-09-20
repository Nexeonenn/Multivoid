// coop/net/send_admission.cpp -- see coop/net/send_admission.h.

#include "coop/net/send_admission.h"

namespace coop::net {

void SendAdmission::Reset() {
    for (auto& s : slots_) {
        s.handed.store(0, std::memory_order_relaxed);
        s.floor.store(0, std::memory_order_relaxed);
        s.refusals.store(0, std::memory_order_relaxed);
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
}

void SendAdmission::SetSendBufBytes(int bytes) {
    if (bytes > 0) sendBufBytes_.store(bytes, std::memory_order_relaxed);
}

void SendAdmission::NoteHanded(int slot, int bytes) {
    if (slot < 0 || slot >= kSlots || bytes <= 0) return;
    slots_[slot].handed.fetch_add(bytes, std::memory_order_release);
}

bool SendAdmission::Admits(int slot, int len) const {
    if (slot < 0 || slot >= kSlots || len <= 0) return false;
    const Slot& s = slots_[slot];
    // Acquire on both, so a fresh floor cannot be paired with a handed count from before the
    // anchor that produced it -- the one pairing that would read low by more than the header term.
    const int64_t estimate = s.floor.load(std::memory_order_acquire) +
                             s.handed.load(std::memory_order_acquire);
    const int buf = sendBufBytes_.load(std::memory_order_relaxed);
    const int reserve = (kReserve < buf / kMaxReserveFraction) ? kReserve
                                                              : buf / kMaxReserveFraction;
    return estimate + len <= static_cast<int64_t>(buf) - reserve;
}

void SendAdmission::NoteRefused(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    slots_[slot].refusals.fetch_add(1, std::memory_order_relaxed);
}

uint32_t SendAdmission::TakeRefusals(int slot) {
    if (slot < 0 || slot >= kSlots) return 0;
    return slots_[slot].refusals.exchange(0, std::memory_order_relaxed);
}

}  // namespace coop::net
