// coop/save/join_window_baseline.h -- what the host's world was at the instant a joiner's blob was
// cut, and every correction that reconciles the host's live world against it until the window
// closes.
//
// The blob a joiner loads is a PHOTOGRAPH. The host keeps playing while that photograph travels,
// is written to disk and is loaded, which on a slow link is a minute: a pile is carried off, a
// keyed prop is picked up and destroyed, a kerfur is turned on. Everything the joiner then builds
// from the photograph is right for an instant that has passed. This module is the other half of
// the transfer: at the capture instant it records the world's save-authoritative state, and
// through the join window it sends the differences.
//
// Two kinds of difference, both per joiner, because each joiner's photograph was cut at its own
// instant:
//
//   - GONE. The keyed props the blob contains that the host no longer has. An explicit PropDestroy
//     per key at the connect edge, ahead of the snapshot bracket so the removes precede the adds
//     (MTA's Packet_EntityRemove). Without it the divergence sweep has to INFER the delete.
//   - MOVED. The save-time position of every chipPile, garbage clump, off-form kerfur and keyed
//     prop. A pile carries no position on the wire at all (both peers load it from the same save);
//     a keyed prop does carry one, but the joiner's own loadObjects re-creates it at the save
//     position afterwards and clobbers it. Both are answered the same way: re-assert the host's
//     current position once the joiner has quiesced, past the clobber.
//
// The window does not close at world-ready. A pile the host moves late in a long load tail would
// get no correction from a one-shot, so the flush is LATE-ARMED: it keeps running on a cadence for
// a window past the connect edge, deduped per (slot, eid) to actual movement. Its expiry is the
// join window's true close, and it is what retires the maps -- with "an active join" defined as a
// non-empty map, a map left behind made every steady-state pile grab stamp a save-time key and
// every landing carry one, and the client armed a hopeless pending twin per drop.
//
// A stale-fallback join (the live capture was unavailable and the canonical on-disk slot went out
// instead) captures no baseline at all: the maps stay empty, every entry point is a no-op, and the
// divergence sweep keeps full responsibility for that join.
//
// Host side, game thread throughout.

#pragma once

#include "coop/element/element.h"  // ElementId
#include "ue_wrap/core/types.h"    // ue_wrap::FVector

namespace coop::net { class Session; }

namespace coop::join_window_baseline {

// Remember the session for sends. Once at harness boot, beside the transfer's own install.
void Install(coop::net::Session* session);

// THE CAPTURE INSTANT. Record the host's keyed-prop key set and the save-time position of every
// chipPile, garbage clump, off-form kerfur and keyed prop. Called from the transfer's request
// handler on the live-capture success path only, in the same breath as the blob it describes.
void CaptureForSlot(int peerSlot);

// The connect edge: what this joiner's blob has and the host no longer does, as one explicit
// PropDestroy per key. Consumes the key set. A no-op without a live-capture baseline.
void SendDivergenceDeletes(int peerSlot);

// The connect edge, after the snapshot trigger: one position correction per save-authoritative
// entity the host moved since the capture. Arms the late window and resets its dedupe.
void FlushDivergedPositions(int peerSlot);

// The late-arm cadence: re-flush each armed joiner's diverged positions until its window expires,
// then retire that slot's maps. From the transfer's host tick.
void TickLateArm();

// The save-time position of keyless chipPile `eid` for `peerSlot`. False (out untouched) for a
// stale-fallback join, an unseeded or post-save pile, or an out-of-range slot. The connect-replay
// snapshot builder stamps it onto the pile's spawn so the client's twin destroy reconciles a pile
// the host moved in the join window.
bool TryGetPileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out);

// The same for an entity that was a garbage CLUMP in the save this joiner loaded: its own copy is
// a clump at this position.
bool TryGetClumpXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out);

// Like TryGetPileXform but across all active join slots: a convert broadcast is a single fan-out
// with no slot, and a pile eid is unique, so at most one slot holds it.
bool TryGetPileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// The save-time position of off-form kerfur `eid`, across every active slot for the same reason.
// False if no slot captured it: a stale-fallback join, a kerfur bought after the save, or one
// already active at every blob instant. The kerfur table reads it at the first conversion to
// record the off-prop's origin eid, which the connect snapshot carries to the joiner as the eid to
// retire.
bool TryGetKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// Record the pre-grab position of pile `eid` into every active slot's map, at the seam where a
// grabbed pile's clump is born and before the pile dies in place, so what is recorded is still its
// save position -- the key the joiner's native sits at. The landing convert then carries it and the
// client arms a pending save-time twin. A no-op outside a join; a re-grab overwrites.
void RecordGrabTimePileXform(coop::element::ElementId eid, const ue_wrap::FVector& preGrabLoc);

// A peer left, or its stream was cancelled: drop its baseline and disarm its late flush.
void ClearForSlot(int peerSlot);

}  // namespace coop::join_window_baseline
