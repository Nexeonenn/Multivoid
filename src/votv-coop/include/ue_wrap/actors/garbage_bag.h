// ue_wrap/actors/garbage_bag.h -- the bagging family: the two tools a player packs a trash pile
// with, and the filled bag the pack produces.
//
// Its own file rather than a corner of ue_wrap/actors/prop: bagging is one feature, and prop.cpp
// is at the size where the next feature to land in it would be looking for a home anyway.
//
// A folded bag (prop_garbBagFold_C) is spent whole on one pack; a roll (prop_garbBagRoll_C) holds
// a count in `bags` and is spent one at a time; both spawn prop_garbageBag_C at the target's
// transform with the target's chipType. The gameplay lane that carries a client's pack across is
// coop/props/pack_trash_intent.

#pragma once

#include <cstdint>

namespace ue_wrap::garbage_bag {

// The two tools, each answering for its class or a subclass. A class that is not loaded costs a
// full object walk per call (a FindClass hit is memoised, a miss is not), so these are written for
// once-per-press call sites, not for a per-frame poll. Game thread.
bool IsFold(void* obj);
bool IsRoll(void* obj);

// The filled bag's class, for a spawn; null until it loads. Game thread.
void* FilledClass();

// The roll's remaining count, the number its own hint prints. A peer that spends a bag writes this
// field, and the roll's own body reads the same one. False for anything but a roll. Game thread.
bool ReadRollCount(void* roll, int32_t& bags);
bool WriteRollCount(void* roll, int32_t bags);

}  // namespace ue_wrap::garbage_bag
