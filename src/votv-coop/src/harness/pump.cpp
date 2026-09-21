// harness/pump.cpp -- see harness/pump.h.

#include "harness/pump.h"

#include "harness/session_runtime.h"   // Session(): the process's one session object

#include "coop/comms/chat_feed.h"
#include "coop/dev/object_overlay.h"
#include "coop/dev/ragdoll_bone_overlay.h"
#include "coop/player/death_revive.h"
#include "coop/player/nameplate.h"
#include "coop/player/run_end_travel.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/shutdown.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace harness::pump {
namespace {

namespace GT = ue_wrap::game_thread;

std::atomic<bool> g_queued{false};
uint64_t g_lastDrain = 0;   // game thread

}  // namespace

void PostComposite(GT::Task body) {
    bool idle = false;
    if (!g_queued.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return;
    GT::Post([body = std::move(body)] {
        struct Clear { ~Clear() { g_queued.store(false, std::memory_order_release); } } clear;
        const uint64_t drain = GT::DrainSerial();
        if (drain == g_lastDrain) return;
        g_lastDrain = drain;
        body();
    });
}

void PostMenuTick() {
    PostComposite([] {
        coop::net_pump::Tick(harness::session_runtime::Session());
        coop::nameplate::Update();
        coop::dev::object_overlay::Update(); coop::dev::ragdoll_bone_overlay::Update();
        coop::chat_feed::Tick();
        TickShutdownHooks();
    });
}

void TickShutdownHooks() {
    coop::net::Session* s = &harness::session_runtime::Session();
    coop::shutdown::Install(s);
    coop::shutdown::UpdateWindowTitle();
    coop::death_revive::Install(s);
    coop::player::run_end_travel::Install(s);
}

void TickWatchdogs() {
    // The join watchdog lives HERE, not on the render thread. Every phase budget in join_progress
    // used to be reachable only through ui::loading_screen::Render, i.e. through the ImGui
    // overlay's Present hook -- and harness.cpp treats a failed overlay install as non-fatal and
    // boots on, because the multiplayer entry point is a native UMG button, not ours. In a session
    // like that every join budget was dead. This runs on the thread that drives the join.
    coop::join_progress::MaybeTimeout();
    coop::death_revive::Watchdog();
}

}  // namespace harness::pump
