// coop/session/join_beacon.cpp -- see coop/session/join_beacon.h.

#include "coop/session/join_beacon.h"

#include "coop/net/session.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>

namespace coop::join_beacon {
namespace {

using coop::net::HostJoinPhase;

// Set once at boot from the harness thread, read on the game thread.
std::atomic<coop::net::Session*> g_session{nullptr};

constexpr int64_t kBeaconIntervalMs = 1000;

// The cadence is kept on ATTEMPTS and the log on SENDS, and the two are separate fields because
// the interesting second is the one where they disagree. A refused send leaves nothing behind to
// throttle against, so a cadence measured from the last SUCCESS stops throttling the moment the
// link pushes back -- which is the exact second this beacon exists for, and the send buffer is at
// the brim for the whole of a world transfer. The same is true of a phase change, whose
// "send it now" bypass stays armed until a send lands. Both became a per-tick send attempt on the
// host, tens of times a second, on the machine already carrying the transfer.
struct SlotNote {
    bool          noted = false;        // an owner worked this slot since the last Tick
    HostJoinPhase phase = HostJoinPhase::CapturingWorld;
    uint32_t      done = 0;
    uint32_t      total = 0;
    // The throttle's own state: what was last ATTEMPTED, and when.
    bool          everAttempted = false;
    HostJoinPhase attemptedPhase = HostJoinPhase::CapturingWorld;
    int64_t       lastAttemptMs = 0;
    // The log's state: what actually went out, so a line never claims a note the link refused.
    bool          everSent = false;
    HostJoinPhase sentPhase = HostJoinPhase::CapturingWorld;
};
SlotNote g_notes[coop::net::kMaxPeers];

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* PhaseName(HostJoinPhase p) {
    switch (p) {
    case HostJoinPhase::CapturingWorld:    return "capturing the world";
    case HostJoinPhase::StreamingWorld:    return "streaming the world";
    case HostJoinPhase::SnapshotDeferred:  return "holding the bracket";
    case HostJoinPhase::StreamingSnapshot: return "streaming the bracket";
    }
    return "?";
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void NotePhase(int slot, HostJoinPhase phase, uint32_t done, uint32_t total) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    SlotNote& n = g_notes[slot];
    n.noted = true;
    n.phase = phase;
    n.done = done;
    n.total = total;
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    const int64_t now = NowMs();
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        SlotNote& n = g_notes[slot];
        // Consumed whether or not it is sent: the flag means "an owner worked this slot in the pass
        // just gone", and a slot nobody works falls silent on the next pass, which is the contract.
        const bool noted = n.noted;
        n.noted = false;
        if (!noted) continue;
        // A phase this slot has not tried to report yet goes out at once; everything else waits out
        // the interval, whether or not the last try landed.
        const bool newPhase = !n.everAttempted || n.phase != n.attemptedPhase;
        if (!newPhase && now - n.lastAttemptMs < kBeaconIntervalMs) continue;
        n.everAttempted = true;
        n.attemptedPhase = n.phase;
        n.lastAttemptMs = now;
        coop::net::JoinPhaseNotePayload p{};
        p.phase = static_cast<uint8_t>(n.phase);
        p.done = n.done;
        p.total = n.total;
        // The probe's path, for the probe's reason: a beacon is only worth what it says about NOW,
        // so it goes straight at the connection under the headroom reserve's exemption rather than
        // into the backlog, where a full send buffer -- the state a world transfer holds it in for
        // its whole length -- would park it behind the very stream it is reporting on. A refusal
        // here is the transport's own buffer being full, and the next second's note is a better
        // message than the one that was turned back, so a refusal is dropped rather than retried.
        if (!s->TrySendReliableToSlot(slot, coop::net::ReliableKind::JoinPhaseNote, &p,
                                      sizeof(p))) {
            continue;
        }
        if (!n.everSent || n.phase != n.sentPhase) {
            UE_LOGI("join_beacon: slot %d -- %s (%u/%u)", slot, PhaseName(n.phase), n.done,
                    n.total);
        }
        n.everSent = true;
        n.sentPhase = n.phase;
    }
}

void CancelForSlot(int slot) {
    if (slot < 1 || slot >= coop::net::kMaxPeers) return;
    g_notes[slot] = SlotNote{};
}

void OnDisconnect() {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) g_notes[slot] = SlotNote{};
}

}  // namespace coop::join_beacon
