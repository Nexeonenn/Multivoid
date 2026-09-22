// coop/props/prop_spawn_authoring.h -- did a PLAYER author this prop birth, or did the game?
//
// ONE concept, and it is the fact the client's birth seam was missing. A client's world produces
// keyed props constantly without anyone asking: mushroom and birch spawners, a dirthole, a
// lightning strike, an impact component breaking something, lib_C::replaceProp morphing one prop
// into another -- every one of them resolves the same catalog and finishes the same
// GameplayStatics spawn as the sandbox spawn menu does. They must NOT cross to the host: the other
// peers' games produce their own, and an intent for each would double the world.
//
// So the client's drop-intent lane could not admit a birth by its CLASS -- it kept a four-lineage
// whitelist instead, which the spawn menu, an arbitrary catalog, can never be on. The
// discriminator is not what was born but WHO ASKED -- docs/coop-entity-expression-map.md, "Who
// asked, for a client's fresh birth". This module answers that, and only that; the lane decides
// what to do with the answer.
//
// The two player spawn verbs in the cook, both read from bytecode on the shipped pak:
//
//   * the spawn menu. A click on a catalog slot runs ui_spawnmenu_C::spawn(FName), which traces
//     from the camera and calls gamemode->spawnPropThroughGamemode(row, transform, 1, out) --
//     ui_spawnmenu::spawn[13]. That gamemode verb is where the deferred spawn and the finish live,
//     but it is ALSO what replaceProp and the physics-impact component call, so the verb alone
//     does not name a player. The caller does. A script-body gate watch on the gamemode verb
//     reads the calling Blueprint frame the VM already built and opens a bracket only for the
//     menu's own spawn().
//
//   * the toolgun. tool_spawn_C::init(toolgun) enters ExecuteUbergraph_tool_spawn, which carries
//     its OWN copy of the three-branch spawn and finishes the actor itself -- it never goes
//     through the gamemode verb. Its ubergraph holds no other FinishSpawningActor, so a prop
//     finished while that body is the calling frame is a toolgun spawn by construction, and the
//     native seam's caller frame names it with no watch at all.
//
// Gameplay layer (principle 7): it reaches the engine only through ue_wrap. Game thread only --
// both the gate and the native seam hand their callbacks the VM's live frame.

#pragma once

namespace coop::prop_spawn_authoring {

// Install the script-body gate watch on the gamemode's spawn verb. A no-op on the host, which
// never asks the question: its own births are the shared world whoever asked for them. Idempotent
// and retry-throttled while mainGamemode_C is unresolved (it loads on gameplay entry). Safe to
// call every subsystem-install tick. Game thread.
void Install(bool isClient);

// Is the birth the caller is looking at RIGHT NOW authored by a player's own spawn verb? Call it
// from inside a FinishSpawningActor post callback and nowhere else: it reads that callback's
// calling frame (ue_wrap::ufunction_hook::CurrentCallerFrame) and this thread's open menu
// bracket, both of which mean nothing outside one. False when neither verb is on the stack, which
// is every ambient spawn the world makes for itself. Game thread.
bool BirthIsPlayerAuthored();

// Session teardown: report both columns of the caller tally -- the menu's calls, which crossed as
// intents, and every other caller's, which stayed local -- and zero it. The watch and the resolved
// UFunctions stay: they name the game's own classes, not this session's world. Game thread.
void Reset();

}  // namespace coop::prop_spawn_authoring
