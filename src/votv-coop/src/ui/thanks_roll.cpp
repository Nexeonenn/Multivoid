// ui/thanks_roll.cpp -- see ui/thanks_roll.h.
//
// The tree, built detached and attached with one AddChild (a slot's raw fields are read once,
// when Slate builds it, so every slot below is written before the root joins the live menu):
//
//   the game's rows container (a vertical box: our version line, "Alpha", "Build")
//     root (vertical box)
//       sentence: the title, and under it a row of each section's name in its colour
//       window (size box, the fixed frame) > panel (overlay): a dark backing, then
//         columns (horizontal box)
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
#include <vector>

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
// ask, up to a bound; past it the panel clips, so one very long name costs its own tail and not
// the layout. The menu's sub-windows open over this corner of the screen behind a scrim, so the
// bound is the free sky to the right, not their edge.
constexpr float kWindowMaxW = 640.f;
constexpr float kWindowH = 112.f;
// The backing: the dark band the game puts behind its own title, so the names hold against a
// bright sky. The columns sit inside it by this much.
constexpr ue_wrap::FLinearColor kBacking{0.f, 0.f, 0.f, 0.45f};
constexpr float kPanelPadH = 8.f;
constexpr float kPanelPadV = 5.f;
constexpr float kGapAboveSentence = 14.f;  // the space under the game's "Build" line
constexpr float kGapUnderSentence = 4.f;
constexpr float kSeamGap = 8.f;            // between the two copies, where the loop closes
constexpr float kGutter = 16.f;            // between the columns
constexpr float kApartGutter = 28.f;       // before the column that stands apart
constexpr int32_t kNamePt = 10;
constexpr int32_t kSentencePt = kNamePt;   // the legend is read with the names, at their size
// A list this short stays in one column; a longer one is dealt into two of equal length, which
// roll as one and halve the time a name waits for its turn. Sections marked `apart` are not
// dealt: their names get a column of their own after the others.
constexpr size_t kOneColumnNames = 6;
constexpr int kMaxColumns = 3;
// The game rolls its list at 3 lines a second past a window 45 lines tall; past one 6 lines tall
// that speed leaves a name on screen for 2 seconds. Half of it keeps a name readable.
constexpr float kLinesPerSecond = 1.5f;

// ESlateVisibility.
constexpr uint8_t kCollapsed = 1, kHitTestInvisible = 3;

struct Column {
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

// One name and the section it came from: what a column is dealt.
struct Entry { const std::string* name; const Section* section; };

void* g_menu = nullptr;            // the menu instance built into (compared, never dereferenced)
ue_wrap::CachedObjRef g_root;      // our root; its children are read only while it is alive
Column g_columns[kMaxColumns];
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

// The sentence is the legend: the title in the amber of the game's own "Patrons Tier III"
// header across the screen, and on the line under it each section's name in that section's
// colour ("testers, bug reporters and Boosty supporters:"), which is what the colours below
// mean. Two lines, so the legend does not run across the whole sky. The names carry no headers.
bool AddSentence(void* root, const List& list) {
    const std::wstring title = Wide(list.title);
    void* head = title.empty() ? nullptr
                               : NS::AddText(root, title.c_str(), kSentencePt, NS::Amber(), NS::kJustLeft, 0.f);
    if (!title.empty() && !head) return false;
    PadSlot(NS::SlotOf(head), P::off::UVerticalBoxSlot_Padding, 0.f, kGapAboveSentence, 0.f, 0.f);
    void* row = NS::Spawn(L"HorizontalBox", root);
    if (!row) return false;
    auto word = [row](const std::wstring& text, const ue_wrap::FLinearColor& col) {
        return text.empty() || NS::AddText(row, text.c_str(), kSentencePt, col, NS::kJustLeft, 0.f);
    };
    const size_t n = list.sections.size();
    for (size_t i = 0; i < n; ++i) {
        if (!word(Wide(list.sections[i].title), Colour(list.sections[i].rgb))) return false;
        const wchar_t* joint = i + 1 == n ? L":" : (i + 2 == n ? L" and " : L", ");
        if (!word(joint, NS::Amber())) return false;
    }
    PadSlot(NS::AddVFill(root, row, 0.f, NS::kLeft, NS::kTop),
            P::off::UVerticalBoxSlot_Padding, 0.f, title.empty() ? kGapAboveSentence : 0.f, 0.f, 0.f);
    return true;
}

// One copy of a column: its names in order, a text block per run of one section, so a colour
// costs a widget only where it changes. Each name behind a dash, as the game lists its own
// supporters; the dash is the roll's, the data file holds names and nothing else. `padLines`
// blank lines close a column one name short of its neighbour, so both wrap at the same offset.
void* BuildCopy(void* roll, const Entry* entries, size_t count, int padLines) {
    void* copy = NS::Spawn(P::name::VerticalBoxClass, roll);
    if (!copy) return nullptr;
    for (size_t i = 0; i < count;) {
        const Section* section = entries[i].section;
        std::wstring names;
        for (; i < count && entries[i].section == section; ++i) {
            if (!names.empty()) names.push_back(L'\n');
            names += L"- ";
            names += Wide(*entries[i].name);
        }
        if (i == count) names.append(static_cast<size_t>(padLines), L'\n');
        if (!NS::AddText(copy, names.c_str(), kNamePt, Colour(section->rgb), NS::kJustLeft, 0.f))
            return nullptr;
    }
    return copy;
}

bool BuildColumn(void* columns, const Entry* entries, size_t count, int padLines, float padLeft,
                 Column& out) {
    out = Column{};
    void* clip = NS::Spawn(L"SizeBox", columns);
    void* roll = clip ? NS::Spawn(P::name::VerticalBoxClass, clip) : nullptr;
    if (!roll) return false;
    U::SetClipping(clip, 1);  // ClipToBounds: the window the roll passes behind
    out.first  = BuildCopy(roll, entries, count, padLines);
    out.second = BuildCopy(roll, entries, count, padLines);
    if (!out.first || !out.second) return false;
    U::AddChild(roll, out.first);
    PadSlot(U::AddChild(roll, out.second), P::off::UVerticalBoxSlot_Padding, 0.f, kSeamGap, 0.f, 0.f);
    E::SetWidgetVisibility(out.second, kCollapsed);  // shown once the column is known to scroll
    U::SetContent(clip, roll);
    // As wide as its widest line, so the backing ends where the last column does.
    PadSlot(NS::AddHFill(columns, clip, 0.f, NS::kFill, NS::kFill),
            P::off::UHorizontalBoxSlot_Padding, padLeft, 0.f, 0.f, 0.f);
    out.lines = static_cast<int>(count) + padLines;
    out.roll = roll;
    return true;
}

// Build the whole tree detached, then attach it as the container's last row. False leaves
// nothing attached; the unattached widgets are collected with the next GC.
bool Build(void* container, const List& list) {
    void* root = NS::Spawn(P::name::VerticalBoxClass, container);
    if (!root) return false;
    E::SetWidgetVisibility(root, kHitTestInvisible);  // drawn, never a click target
    if (!AddSentence(root, list)) return false;

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

    // Every name in the list's own order. The columns are not the sections: with the legend in
    // the sentence a column has no title to keep, so the common names are dealt by count down one
    // column and then the next, and the colours alone say who is who. The sections that stand
    // apart keep a column to themselves, after the others.
    std::vector<Entry> common, apart;
    for (const Section& s : list.sections)
        for (const std::string& n : s.names) (s.apart ? apart : common).push_back(Entry{&n, &s});
    for (Column& c : g_columns) c = Column{};
    int used = 0;
    if (!common.empty() && common.size() <= kOneColumnNames) {
        if (!BuildColumn(columns, common.data(), common.size(), 0, 0.f, g_columns[used++])) return false;
    } else if (!common.empty()) {
        const size_t firstCount = (common.size() + 1) / 2;
        const size_t secondCount = common.size() - firstCount;
        if (!BuildColumn(columns, common.data(), firstCount, 0, 0.f, g_columns[used++])) return false;
        if (!BuildColumn(columns, common.data() + firstCount, secondCount,
                         static_cast<int>(firstCount - secondCount), kGutter, g_columns[used++]))
            return false;
    }
    if (!apart.empty() &&
        !BuildColumn(columns, apart.data(), apart.size(), 0, used ? kApartGutter : 0.f, g_columns[used++]))
        return false;
    if (void* s = U::AddChild(panel, columns)) {
        U::SetSlotAlign(s, P::off::UOverlaySlot_HAlign, P::off::UOverlaySlot_VAlign, NS::kFill, NS::kFill);
        PadSlot(s, P::off::UOverlaySlot_Padding, kPanelPadH, kPanelPadV, kPanelPadH, kPanelPadV);
    }
    U::SetContent(window, panel);
    PadSlot(NS::AddVFill(root, window, 0.f, NS::kLeft, NS::kTop),
            P::off::UVerticalBoxSlot_Padding, 0.f, kGapUnderSentence, 0.f, 0.f);

    if (!U::AddChild(container, root)) return false;
    g_root.Set(root);
    g_rollStart = std::chrono::steady_clock::now();
    return true;
}

// Learn one copy's height once Slate has laid it out; a column taller than its window scrolls.
// The window is the frame less the panel's padding, in the units the desired size reports in.
void Measure(Column& c) {
    ue_wrap::FVector2D size{};
    if (!U::WidgetDesiredSize(c.first, size) || size.Y < 1.f) {
        // A layout normally lands within two ticks. One that never does must not cost an engine
        // call a tick for as long as the menu is up: the column then stays as built, standing still.
        if (++c.measureTries >= kMaxMeasureTries) {
            c.measured = true;
            UE_LOGW("thanks_roll: a column was never laid out -- it stays still");
        }
        return;
    }
    c.measured = true;
    c.period = size.Y + kSeamGap;
    c.scrolling = size.Y > kWindowH - 2.f * kPanelPadV;
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
        for (Column& c : g_columns) Animate(c, seconds);
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
    for (Column& c : g_columns) c = Column{};
    if (!current) { g_failures = 0; g_settled = false; }

    // The list outlives the build: a column's entries point into it until Build returns.
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
