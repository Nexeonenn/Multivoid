// ue_wrap/hotbar/icons.h -- the quick-slot bar (the game's "hotbar") and the icon tables it draws
// from, plus the one edge the game leaves out.
//
// THE GAME'S SHAPE, measured 2026-09-21 (F-121):
//   `ui_UI_C::updateSlotInv` (`research/bp_reflection/cpp/ui_UI.cpp:1676`) rebuilds the bar whole:
//   it lists `saveSlot.GObjStack[playerContainer.propInventory.index].obj`, clears every
//   `slotInv[i]` image's material to Engine's Black texture, then writes each item's icon through
//   `lib_C::propToIcon` (`lib.cpp:3306`), which is
//   `mainGameInstance.texs_GAMEINST[Array_Find(propRenderer.names, name)]`.
//   Those two tables are built by `ApropProcessor_C`, which renders every prop to a texture and
//   publishes the pair when it finishes (`propProcessor.cpp:276` writes `texs_GAMEINST`).
//
// THE DEFECT: the renderer finishes SECONDS after the world is up, and the game's own post-load
// refresh runs before it. Measured on this rig: the refresh ran 11 s after the world with
// `propNames=2471, texs_GAMEINST=0` on a host and `0/0` on a client, so every lookup returned
// null and every slot kept the Black the clear pass had just written. Nothing rebuilds the bar
// afterwards -- at the moment the renderer publishes the tables it refreshes the EQUIPMENT panel
// (`propProcessor.cpp:593`, `gamemode.propInventory.upd_equipment()`) and not the bar, no
// Blueprint in the corpus binds its `finished` delegate, and the gamemode's
// `intComs_propRenderer_finishProps` is an empty stub (`mainGamemode.cpp:9445` -> `Label_18320:
// return`). So the bar stays iconless until some gameplay verb rebuilds it, which is exactly the
// reported symptom: the icons appear the moment the item is touched.
//
// WHAT WE ADD: the missing half of that publish. When the icon tables are available and the bar
// was built without them, rebuild it once per world through the game's own verb. It is not a
// repair loop -- it fires at most once per world, on a condition that is false in every ordering
// the game gets right, and every later rebuild is the game's own.
//
// Engine-wrapper layer (principle 7): no network and no coop state. This is the game's own UI
// contract, read and re-issued; a client and a host hit the identical defect for the identical
// reason.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::hotbar {

// The bar and its icon source as they stand right now. Every field is a count read straight off
// the live objects, so a caller can say WHICH input was missing rather than that one was.
struct State {
    void*   ui             = nullptr;  // ui_UI_C (mainGamemode.playerInterface)
    void*   renderer       = nullptr;  // ApropProcessor_C (mainGamemode.propRenderer)
    int32_t slots          = 0;        // ui_UI_C.slotInv    -- the ten slot images
    int32_t slotTexts      = 0;        // ui_UI_C.slotTexts  -- their amount labels
    int32_t iconNames      = 0;        // propProcessor.names          (propToIcon's index side)
    int32_t iconTextures   = 0;        // mainGameInstance.texs_GAMEINST (propToIcon's value side)
    int32_t rendererPhases = -1;       // propProcessor.fins -- how far the render got
    int32_t carried        = -1;       // records in the live personal store

    // Both halves of the icon lookup are published. Either one empty makes every icon null.
    bool IconsReady() const { return iconNames > 0 && iconTextures > 0; }
};

// Read it. False (and `out` left alone) before the world is up or while the reflection has not
// resolved. Pure field reads, no UFunction dispatch -- safe from inside a Blueprint body. Game
// thread.
bool Read(State& out);

// The texture bound to slot `i`'s `tex` material parameter, by name. Empty when the slot index is
// out of range or the widget is gone; `no-material` when the image was never touched by a rebuild;
// `no-parameter` when a material is bound but carries no `tex` override. A real rebuild that found
// an icon leaves a texture name here, and one that did not leaves Engine's `Black`. Game thread.
std::wstring SlotTexture(const State& s, int i);

// Does the bar contradict the store -- items carried, icons available, and the first slot still
// holding no icon? That is the fingerprint of a bar built before the tables existed. Game thread.
bool BuiltWithoutIcons(const State& s);

// Rebuild the bar through the game's own `ui_UI_C::updateSlotInv`. Idempotent by construction (the
// verb rebuilds from the store every time). False if the verb or the widget is unavailable. Game
// thread.
bool Rebuild(const State& s);

// The missing edge: once per world, when the icons have arrived and the bar was built without
// them, rebuild it. A no-op in every other state and after it has fired. Game thread, per tick.
void Tick();

}  // namespace ue_wrap::hotbar
