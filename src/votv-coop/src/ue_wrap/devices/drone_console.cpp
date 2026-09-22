// ue_wrap/devices/drone_console.cpp -- see ue_wrap/devices/drone_console.h.

#include "ue_wrap/devices/drone_console.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>

namespace ue_wrap::drone_console {

namespace R = ue_wrap::reflection;

namespace {

// The console's class and the three fields its own verb reads, resolved on first sight.
ue_wrap::CachedObjRef g_consoleCls;
int32_t g_conOpenedOff   = -1;  // droneConsole_C::opened          (the lid)
int32_t g_conKeyboardOff = -1;  // droneConsole_C::lookatKeyboard  (the presser's own cursor)
int32_t g_conDroneOff    = -1;  // droneConsole_C::drone           (a level reference)
void*   g_triggerFlyFn   = nullptr;  // drone_C::triggerFly(console)
constexpr int32_t kMaxConsoles = 8;  // the base has one; the room is for a level that adds more

void* ConsoleClass() {
    if (g_consoleCls.Alive()) return g_consoleCls.Raw();
    g_consoleCls.Set(R::FindClass(L"droneConsole_C"));
    return g_consoleCls.Raw();
}

// A console field by name, resolved once off the live instance's own class.
bool ConsoleBool(void* console, int32_t& off, const wchar_t* name, bool& out) {
    if (!IsConsole(console)) return false;
    if (off < 0) off = R::FindPropertyOffset(R::ClassOf(console), name);
    if (off < 0) return false;
    out = *reinterpret_cast<bool*>(reinterpret_cast<char*>(console) + off);
    return true;
}

}  // namespace

int32_t LiveConsoles(void** out, int32_t cap) {
    if (!out || cap <= 0 || !ConsoleClass()) return 0;
    // The consoles are level geometry: they are born with the world and never respawn, so the walk
    // that finds them runs once and the answer is held by slot and serial. Without this the host
    // paid a full object-array walk and a vector allocation per press it performed. A slot that
    // dies drops out and the next call re-scans, throttled by the same second the drone's own
    // singleton lookup uses.
    static std::chrono::steady_clock::time_point s_lastScan{};
    static void*   s_cache[kMaxConsoles] = {};
    static int32_t s_cacheIdx[kMaxConsoles] = {};
    static int32_t s_cacheN = 0;
    int32_t live = 0;
    for (int32_t i = 0; i < s_cacheN; ++i)
        if (s_cache[i] && R::IsLiveByIndex(s_cache[i], s_cacheIdx[i])) ++live;
    const auto now = std::chrono::steady_clock::now();
    if (live != s_cacheN && now - s_lastScan >= std::chrono::seconds(1)) {
        s_lastScan = now;
        s_cacheN = 0;
        for (void* c : R::FindObjectsByClass(L"droneConsole_C")) {
            if (s_cacheN >= kMaxConsoles) break;
            if (!c || !R::IsLive(c)) continue;
            s_cache[s_cacheN] = c;
            s_cacheIdx[s_cacheN] = R::InternalIndexOf(c);
            ++s_cacheN;
        }
    }
    int32_t n = 0;
    for (int32_t i = 0; i < s_cacheN && n < cap; ++i)
        if (s_cache[i] && R::IsLiveByIndex(s_cache[i], s_cacheIdx[i])) out[n++] = s_cache[i];
    return n;
}

void* ClassPtr() { return ConsoleClass(); }

bool IsConsole(void* obj) {
    void* cls = ConsoleClass();
    if (!obj || !cls) return false;
    void* objCls = R::ClassOf(obj);
    void* bases[1] = {cls};
    return objCls && R::IsDescendantOfAny(objCls, bases, 1);
}

bool IsLidOpen(void* console) {
    bool open = false;
    return ConsoleBool(console, g_conOpenedOff, L"opened", open) && open;
}

bool IsCursorOnKeyboard(void* console) {
    bool on = false;
    return ConsoleBool(console, g_conKeyboardOff, L"lookatKeyboard", on) && on;
}

bool TriggerFly(void* console) {
    if (!IsConsole(console)) return false;
    if (g_conDroneOff < 0) g_conDroneOff = R::FindPropertyOffset(R::ClassOf(console), L"drone");
    if (g_conDroneOff < 0) return false;
    void* drone = *reinterpret_cast<void**>(reinterpret_cast<char*>(console) + g_conDroneOff);
    if (!drone || !R::IsLive(drone)) return false;
    if (!g_triggerFlyFn) g_triggerFlyFn = R::FindFunction(R::ClassOf(drone), L"triggerFly");
    if (!g_triggerFlyFn) return false;
    ue_wrap::ParamFrame f(g_triggerFlyFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"console", console);
    return ue_wrap::Call(drone, f);
}

}  // namespace ue_wrap::drone_console
