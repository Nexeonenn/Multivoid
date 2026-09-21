// coop/dev/hotbar_icon_probe.h -- dev-only, read-only observability for the quick-slot bar's
// icons at world load (ini hotbar_icon_probe=1 / env VOTVCOOP_HOTBAR_ICON_PROBE, off by default).
//
// WHY IT EXISTS (F-121): after a load the quick-slot bar shows its items with NO icons until the
// item is interacted with. The game builds those icons in ONE verb, `ui_UI_C::updateSlotInv`
// (`research/bp_reflection/cpp/ui_UI.cpp:1676`), which:
//   1. reads the records from `saveSlot.GObjStack[playerContainer.propInventory.index].obj`,
//   2. clears EVERY `slotInv[i]` image's dynamic material to Engine's Black texture,
//   3. for each name calls `lib_C::propToIcon` -- `texs_GAMEINST[Array_Find(propRenderer.names, nm)]`
//      (`lib.cpp:3306`) -- and writes it into that image's `tex` parameter.
// So a missing icon has exactly three possible causes, and they are distinguishable by state:
//   A. `slotInv` / `slotTexts` were EMPTY when the verb ran -- both are built in the widget's own
//      construct chain (`ui_UI.cpp:750`, behind a Delay), so the images carry no `tex` parameter
//      at all and the bar was never touched;
//   B. `propRenderer.names` or `texs_GAMEINST` were empty -- `Array_Find` returns -1, the item's
//      image carries a `tex` parameter whose value is Black;
//   C. the records themselves were absent -- carried count 0 at the moment of the verb.
// This probe prints exactly those four quantities plus, per slot, whether a `tex` parameter exists
// and which texture it holds, so the failing moment names its own cause instead of being inferred.
//
// Read-only by construction: field reads only (no UFunction dispatch, no GetDynamicMaterial --
// which would CREATE a material instance and destroy the very evidence in cause A). Logs on
// CHANGE, so a settled bar is silent.
//
// ONE EXCEPTION, behind its own flag: `hotbar_icon_probe_poke=1` calls the game's own
// `updateSlotInv` ONCE, after the bar has held still, and logs the bindings either side of it.
// That is the experiment the first measurement earned -- every input the icon path needs read
// READY while all ten slots read Black, which a count cannot explain and one call can.

#pragma once

namespace coop::dev::hotbar_icon_probe {

// Poll the quick-slot bar's readiness and its per-slot icon binding; log on change (no-op unless
// the flag is set). Game thread.
void Tick();

}  // namespace coop::dev::hotbar_icon_probe
