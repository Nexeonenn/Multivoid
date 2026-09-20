// ui/thanks_roll.cpp -- see ui/thanks_roll.h.
//
// The tree, built detached and attached with one AddChild (a slot's raw fields are read once,
// when Slate builds it, so every slot below is written before the root joins the live menu):
//
//   the game's rows container (a vertical box: our version line, "Alpha", "Build")
//     root (vertical box)
//       title
//       window (size box, the fixed frame) > panel (overlay): a dark backing, then
//         columns (horizontal box) > column (vertical box)
//           header, pinned, when the column holds one section
//           clip (size box, ClipToBounds) > roll (vertical box, the part that moves)
//             first copy, second copy (collapsed until the column is known to scroll)

#include "ui/thanks_roll.h"

#include "coop/text/utf8_codec.h"
#include "coop/thanks/thanks_list.h"
#include "ui/native_screen.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <string>

namespace ui::thanks_roll {

namespace {

namespace NS = ui::native_screen;
namespace U = ue_wrap::umg;
namespace E = ue_wrap::engine;
namespace P = ue_wrap::profile;
using coop::thanks_list::List;
using coop::thanks_list::Section;

// The frame, in Slate units. The version lines end near y 210 and the band behind the game's
// title starts near 412; this height ends 12 units above it. The panel is as wide as its columns
// ask, up to a width that stops short of where the menu's sub-windows open; past it the panel
// clips, so one very long name costs its own tail and not the layout.
constexpr float kWindowMaxW = 440.f;
constexpr float kWindowH = 112.f;
// The backing: the dark band the game puts behind its own title, so the names hold against a
// bright sky. The columns sit inside it by this much.
constexpr ue_wrap::FLinearColor kBacking{0.f, 0.f, 0.f, 0.45f};
constexpr float kPanelPadH = 8.f;
constexpr float kPanelPadV = 5.f;
constexpr float kGapAboveTitle = 14.f;  // the space under the game's "Build" line
constexpr float kGapUnderTitle = 4.f;
constexpr float kGapUnderHeader = 2.f;  // between a pinned header and its roll
constexpr float kSectionGap = 8.f;      // above a section header, and at the loop's seam
constexpr float kGutter = 16.f;         // between the columns
constexpr int32_t kTitlePt = 16;        // the version lines' own size
constexpr int32_t kHeaderPt = 12;
constexpr int32_t kNamePt = 10;
// The game rolls its list at 3 lines a second past a window 45 lines tall; past one 6 lines tall
// that speed leaves a name on screen for 2 seconds. Half of it keeps a name readable.
constexpr float kLinesPerSecond = 1.5f;

// ESlateVisibility.
constexpr uint8_t kCollapsed = 1, kHitTestInvisible = 3;

struct Column {
    void* header = nullptr;  // the pinned header, when the column holds one section
    void* roll = nullptr;    // the part that moves
    void* first = nullptr;   // the copy that is measured
    void* second = nullptr;  // the copy that closes the loop
    int   lines = 0;         // text lines in one copy
    float period = 0.f;      // one copy's height plus the seam; the offset wraps here
    bool  measured = false;
    int   measureTries = 0;  // ticks spent waiting for a layout
    bool  scrolling = false;
    int   lastUnits = -1;    // the offset last pushed to the engine
};

void* g_menu = nullptr;            // the menu instance built into (compared, never dereferenced)
ue_wrap::CachedObjRef g_root;      // our root; its children are read only while it is alive
Column g_columns[2];
uint64_t g_builtGeneration = 0;    // the list generation the tree was built from
uint64_t g_lastAttemptMs = 0;      // one build attempt a second while one is failing
int  g_failures = 0;               // failed builds for this menu and list
bool g_settled = false;            // nothing more to do for this menu and list
constexpr int kMaxBuildAttempts = 5;
constexpr int kMaxMeasureTries = 600;  // about five seconds of menu ticks
std::chrono::steady_clock::time_point g_rollStart;

std::wstring Wide(const std::string& utf8) {
    return coop::text::FromUtf8Lossy(utf8.data(), utf8.size());  // the list's parser refused ill-formed text
}

ue_wrap::FLinearColor Colour(uint32_t rgb) {
    return NS::Srgb(static_cast<int>((rgb >> 16) & 0xFF), static_cast<int>((rgb >> 8) & 0xFF),
                    static_cast<int>(rgb & 0xFF));
}

void PadSlot(void* slot, size_t padOff, float l, float t, float r, float b) {
    if (slot) NS::SetSlotPadding(slot, padOff, l, t, r, b);
}

// The rows container the game's version label sits in: the label's row, then the row's parent.
void* RowsContainer(void* versionText) {
    const int32_t parentOff = static_cast<int32_t>(P::off::UPanelSlot_Parent);
    void* row = NS::ReadPtr(NS::SlotOf(versionText), parentOff);
    return row ? NS::ReadPtr(NS::SlotOf(row), parentOff) : nullptr;
}

// One copy of a column's sections: per section one multi-line block of names, under its header
// when the headers roll with the names.
void* BuildCopy(void* roll, const List& list, bool rightColumn, bool withHeaders, int* outLines) {
    void* copy = NS::Spawn(P::name::VerticalBoxClass, roll);
    if (!copy) return nullptr;
    int lines = 0;
    bool firstSection = true;
    for (const Section& s : list.sections) {
        if (s.rightColumn != rightColumn) continue;
        const ue_wrap::FLinearColor col = Colour(s.rgb);
        if (withHeaders) {
            void* header = NS::AddText(copy, Wide(s.title).c_str(), kHeaderPt, col, NS::kJustLeft, 0.f);
            if (!header) return nullptr;
            if (!firstSection)
                PadSlot(NS::SlotOf(header), P::off::UVerticalBoxSlot_Padding, 0.f, kSectionGap, 0.f, 0.f);
            ++lines;
        }
        firstSection = false;
        // Each name behind a dash, as the game lists its own supporters. The dash is the
        // roll's, not the list's: the data file holds names and nothing else.
        std::wstring names;
        for (const std::string& n : s.names) {
            if (!names.empty()) names.push_back(L'\n');
            names += L"- ";
            names += Wide(n);
        }
        if (!NS::AddText(copy, names.c_str(), kNamePt, col, NS::kJustLeft, 0.f)) return nullptr;
        lines += static_cast<int>(s.names.size());
    }
    if (outLines) *outLines = lines;
    return copy;
}

// A column holding one section pins that section's header above the roll, so it is on screen
// for the whole loop; with several, the headers are what separates them and roll with the names.
bool BuildColumn(void* columns, const List& list, bool rightColumn, float padLeft, Column& out) {
    out = Column{};
    const Section* only = nullptr;
    int sections = 0;
    for (const Section& s : list.sections)
        if (s.rightColumn == rightColumn) { only = &s; ++sections; }
    const bool pinned = sections == 1;

    void* column = NS::Spawn(P::name::VerticalBoxClass, columns);
    void* clip = column ? NS::Spawn(L"SizeBox", column) : nullptr;
    void* roll = clip ? NS::Spawn(P::name::VerticalBoxClass, clip) : nullptr;
    if (!roll) return false;
    if (pinned) {
        out.header = NS::AddText(column, Wide(only->title).c_str(), kHeaderPt, Colour(only->rgb),
                                 NS::kJustLeft, 0.f);
        if (!out.header) return false;
        PadSlot(NS::SlotOf(out.header), P::off::UVerticalBoxSlot_Padding, 0.f, 0.f, 0.f, kGapUnderHeader);
    }
    U::SetClipping(clip, 1);  // ClipToBounds: the window the roll passes behind
    out.first  = BuildCopy(roll, list, rightColumn, !pinned, &out.lines);
    out.second = BuildCopy(roll, list, rightColumn, !pinned, nullptr);
    if (!out.first || !out.second) return false;
    U::AddChild(roll, out.first);
    PadSlot(U::AddChild(roll, out.second), P::off::UVerticalBoxSlot_Padding, 0.f, kSectionGap, 0.f, 0.f);
    E::SetWidgetVisibility(out.second, kCollapsed);  // shown once the column is known to scroll
    U::SetContent(clip, roll);
    NS::AddVFill(column, clip, 1.f, NS::kFill, NS::kFill);  // what is left of the frame under the header
    // As wide as its widest line, so the backing ends where the last column does.
    PadSlot(NS::AddHFill(columns, column, 0.f, NS::kFill, NS::kFill),
            P::off::UHorizontalBoxSlot_Padding, padLeft, 0.f, 0.f, 0.f);
    out.roll = roll;
    return true;
}

bool HasColumn(const List& list, bool rightColumn) {
    for (const Section& s : list.sections)
        if (s.rightColumn == rightColumn) return true;
    return false;
}

// Build the whole tree detached, then attach it as the container's last row. False leaves
// nothing attached; the unattached widgets are collected with the next GC.
bool Build(void* container, const List& list) {
    void* root = NS::Spawn(P::name::VerticalBoxClass, container);
    if (!root) return false;
    E::SetWidgetVisibility(root, kHitTestInvisible);  // drawn, never a click target

    const std::wstring title = Wide(list.title);
    if (!title.empty()) {
        // The amber of the game's own "Patrons Tier III" header across the screen.
        void* t = NS::AddText(root, title.c_str(), kTitlePt, NS::Amber(), NS::kJustLeft, 0.f);
        PadSlot(NS::SlotOf(t), P::off::UVerticalBoxSlot_Padding, 0.f, kGapAboveTitle, 0.f, 0.f);
    }

    void* window = NS::Spawn(L"SizeBox", root);
    void* panel = window ? NS::Spawn(L"Overlay", window) : nullptr;
    void* backing = panel ? NS::Spawn(P::name::ImageClass, panel) : nullptr;
    void* columns = backing ? NS::Spawn(L"HorizontalBox", panel) : nullptr;
    if (!columns) return false;
    U::SetSizeBoxMaxWidth(window, kWindowMaxW);
    U::SetSizeBoxHeight(window, kWindowH);
    U::SetClipping(window, 1);  // ClipToBounds
    U::SetImageTintRaw(backing, kBacking);  // an image with no resource draws its tint as a solid rect
    if (void* s = U::AddChild(panel, backing))
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, NS::kFill, NS::kFill);

    // Both columns start at their left edge, the first under the title's own. The game sets its
    // left column against a divider because its panel is centred on one; this one hangs off the
    // corner of the screen. A list that fills one side only is one column.
    const bool left = HasColumn(list, false), right = HasColumn(list, true);
    g_columns[0] = g_columns[1] = Column{};
    if (left && right) {
        if (!BuildColumn(columns, list, false, 0.f, g_columns[0])) return false;
        if (!BuildColumn(columns, list, true, kGutter, g_columns[1])) return false;
    } else if (!BuildColumn(columns, list, right, 0.f, g_columns[0])) {
        return false;
    }
    if (void* s = U::AddChild(panel, columns)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, NS::kFill, NS::kFill);
        PadSlot(s, P::off::UOverlaySlot_Padding, kPanelPadH, kPanelPadV, kPanelPadH, kPanelPadV);
    }
    U::SetContent(window, panel);
    PadSlot(NS::AddVFill(root, window, 0.f, NS::kLeft, NS::kTop),
            P::off::UVerticalBoxSlot_Padding, 0.f, title.empty() ? kGapAboveTitle : kGapUnderTitle,
            0.f, 0.f);

    if (!U::AddChild(container, root)) return false;
    g_root.Set(root);
    g_rollStart = std::chrono::steady_clock::now();
    return true;
}

// Learn one copy's height once Slate has laid it out; a column taller than its window scrolls.
// The window is the frame less the panel's padding and a pinned header, all in the units the
// desired size reports in.
void Measure(Column& c) {
    ue_wrap::FVector2D size{}, header{};
    const bool laidOut = U::WidgetDesiredSize(c.first, size) && size.Y >= 1.f &&
                         (!c.header || (U::WidgetDesiredSize(c.header, header) && header.Y >= 1.f));
    if (!laidOut) {
        // A layout normally lands within two ticks. One that never does must not cost two engine
        // calls a tick for as long as the menu is up: the column then stays as built, standing still.
        if (++c.measureTries >= kMaxMeasureTries) {
            c.measured = true;
            UE_LOGW("thanks_roll: a column was never laid out -- it stays still");
        }
        return;
    }
    c.measured = true;
    c.period = size.Y + kSectionGap;
    c.scrolling = size.Y > kWindowH - 2.f * kPanelPadV - (c.header ? header.Y + kGapUnderHeader : 0.f);
    if (c.scrolling) E::SetWidgetVisibility(c.second, kHitTestInvisible);
}

void Animate(Column& c, float seconds) {
    if (!c.roll) return;
    if (!c.measured) { Measure(c); return; }
    if (!c.scrolling || c.lines <= 0) return;
    const float unitsPerSecond = kLinesPerSecond * (c.period / static_cast<float>(c.lines));
    const int units = static_cast<int>(std::fmod(seconds * unitsPerSecond, c.period));
    if (units == c.lastUnits) return;  // text snaps to whole units; a smaller move draws nothing new
    c.lastUnits = units;
    U::SetRenderTranslation(c.roll, ue_wrap::FVector2D{0.f, -static_cast<float>(units)});
}

}  // namespace

void OnMenuTick(void* menu, void* versionText) {
    if (!menu || !versionText) return;
    const uint64_t generation = coop::thanks_list::Generation();
    const bool current = menu == g_menu && generation == g_builtGeneration;
    if (current && g_settled) return;  // an empty list, or a build that kept failing: nothing to drive
    const bool alive = g_root.Alive();
    if (current && alive) {
        const float seconds =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - g_rollStart).count();
        Animate(g_columns[0], seconds);
        Animate(g_columns[1], seconds);
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastAttemptMs < 1000) return;
    g_lastAttemptMs = now;

    void* container = RowsContainer(versionText);
    if (!container) return;
    // A replaced list rebuilds the roll: the old root leaves the container first.
    if (alive && menu == g_menu) U::RemoveChild(container, g_root.Raw());
    g_root.Reset();
    g_columns[0] = g_columns[1] = Column{};
    if (!current) { g_failures = 0; g_settled = false; }

    List list;
    g_builtGeneration = coop::thanks_list::Copy(list);
    g_menu = menu;
    if (list.sections.empty()) { g_settled = true; return; }  // no roll until the list moves
    if (Build(container, list)) {
        UE_LOGI("thanks_roll: built under the version lines (revision %d, %zu sections)",
                list.revision, list.sections.size());
    } else if (++g_failures >= kMaxBuildAttempts) {
        g_settled = true;
        UE_LOGE("thanks_roll: build failed %d times -- no roll on this menu", g_failures);
    }
}

}  // namespace ui::thanks_roll
