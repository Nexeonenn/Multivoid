// harness/pump.h -- the ONE task this harness posts to the game thread per tick, and the
// watchdogs that deliberately do not ride inside it. Every TimelineThread loop that must keep
// the mod alive -- the play loop and the world-boot waits -- drives the same calls from here,
// so a wait outside gameplay is never a loop that forgot half of them.

#pragma once

#include "ue_wrap/core/game_thread.h"

namespace harness::pump {

// Post `body` as this tick's composite. The TimelineThread posts at 60 Hz and the game thread
// runs posted tasks only at an outermost dispatch, so one script body -- a blocking world
// load's -- holds every task for its whole length. A composite is posted only while none is
// queued, and runs its body at most once in a drain: a drain runs every task posted while it
// runs, so a composite posted as a long tick returned would otherwise run a second tick straight
// after the first. A stall of any length therefore ends in one session tick, never a run of ticks
// back to back with no engine frame between them, each advancing every tick-counted window. The
// flag clears as the composite returns, a faulting body included (the image unwinds destructors
// on a structured exception), so one faulted composite cannot stop the next. Composites are
// idempotent per-tick logic, so a skipped post or body is not lost work.
void PostComposite(ue_wrap::game_thread::Task body);

// The composite every wait loop OUTSIDE gameplay posts: the session tick the save transfer lives
// in, the nameplates, the dev overlays, the chat feed and the shutdown hooks. The loop that waits
// for the transfer is the loop that posts the tick the transfer runs in, so without this a
// menu-mode join deadlocks at "Connecting".
void PostMenuTick();

// The shutdown hooks, run every tick regardless of possession and idempotent: the HWND subclass
// and the window title must work before the local player exists (a close on the splash). The
// run-ending seam is registered here unconditionally rather than lazily from the pump: registered
// only while a session runs, the single-player guarantee would rest on the watch's absence rather
// than on the seam's own session test, and a negative-control run would grade a watch that was
// never there. Safe off the game thread.
void TickShutdownHooks();

// The watchdogs that cover a failure of the PUMP itself, and therefore the one thing that must
// not ride in the pump's own composite: a stalled game thread stops the watchdog and the task it
// is supposed to be watching together. Called from the thread that POSTS the composite instead --
// safe there, since it touches atomics and posts its flee.
void TickWatchdogs();

}  // namespace harness::pump
