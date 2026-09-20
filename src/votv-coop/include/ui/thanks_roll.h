// ui/thanks_roll.h -- the thanks roll: the main menu's list of people the mod thanks, built as
// native UMG under the game's version lines and rolled like the game's own supporter list on the
// other side of the screen (a vertical credit roll, two columns, the content printed twice so the
// loop has no seam). The names come from coop/thanks/thanks_list.h; nothing here knows who they
// are. A child of the menu, so it shows, hides and dies with it and costs nothing in a world.
//
// A column that fits its window stands still. A column that does not is moved by a render
// translation, only when its offset has changed by a whole unit, so the cost is a handful of
// engine calls a second on the title screen and none anywhere else.

#pragma once

namespace ui::thanks_roll {

// Builds the roll once per menu instance (again when the list is replaced), then animates it.
// `versionText` is the game's own version label, the anchor whose rows container the roll joins
// as its last row; null disables it for the tick. Called from the main menu's tick observer, main
// menu only. Game thread.
void OnMenuTick(void* menu, void* versionText);

}  // namespace ui::thanks_roll
