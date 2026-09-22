// ue_wrap/world/game_mode.h -- the game's mode enum, and the mode the running world is under.
//
// VOTV names its campaign flavours in one blueprint enum, enum_gamemode: eight members, story
// through solar, whose ordinals are not the order the mode menu shows them in (sandbox is 4, not
// 1). The cook strips the enumerators' display names, so the ordinal-to-name map below comes from
// the enum asset's own name table, cross-read against the save-name prefixes the game's slot menu
// derives per mode. The mode a world runs under is one byte on the GameInstance, outside the rules
// struct: read by the rules snapshot and the save browser, written by the two boots in
// ue_wrap/engine/engine_save.cpp, and relayed to a joining peer in the save-transfer header. One
// owner for all of it, so the bound, the names and the byte's address are each stated once.
// Principle 7: the engine read and write only -- ui/ owns the render. Game thread.
#pragma once

#include <string>

namespace ue_wrap::game_mode {

// enum_gamemode's member count (its enum_MAX): an ordinal at or above it names no mode.
constexpr int kCount = 8;

// Story, the mode a peer falls back to when no other answer is representable.
constexpr int kStory = 0;

constexpr bool IsValid(int ord) { return ord >= 0 && ord < kCount; }

// The game's own name for an ordinal ("Sandbox"), or null when the ordinal names no mode.
const wchar_t* Name(int ord);

// The same for a narrow-string caller, rendering an ordinal that names no mode as "#N".
std::string NameOrOrdinal(int ord);

// The mode `gameInstance` is under, -1 when the member does not resolve. Game thread.
int ReadFrom(void* gameInstance);

// The local peer's mode, -1 before the GameInstance boots. Game thread.
int ReadLocal();

// Put `mode` in force on `gameInstance`; returns the ordinal it replaced, or -1 when the member
// does not resolve or `mode` names no mode (nothing written). Game thread.
int WriteTo(void* gameInstance, int mode);

}  // namespace ue_wrap::game_mode
