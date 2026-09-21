// coop/config/config_census.cpp -- what this launch is actually configured with, said out loud.
//
// A drill that varies a setting and never checks the setting took effect is not an experiment.
// A send-rate run measured a controller its own log said was off, and every conclusion drawn
// from it described a binary that never ran the code under test. So a peer publishes the rows a
// layer actually supplied -- the value as RESOLVED, and which layer won -- and a rig asserts its
// own independent variables against that before it measures anything.
//
// The two lines are a contract with tools outside the tree, like coop/session/rig_ready.h's:
//   config: EFFECTIVE <key>=<value> (<env|ini>)
//   config: EFFECTIVE end -- <n> row(s) configured
// An absent row is the claim "this peer took the row default", which is why the end line exists:
// it says the census finished rather than the log being read mid-write. A value whose key names
// a credential prints as <set>, so presence is assertable without the log carrying the secret,
// and a raw value the reader refuses says so beside the default it fell back to.

#include "coop/config/config.h"

#include "config_internal.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cstdio>
#include <string>

namespace coop::config {
namespace {

using config_registry::Kind;
using config_registry::Row;

// A row whose key names a credential is reported as present and never quoted: this log is pasted
// into bug reports and captured by CI, and a lobby password in it outlives the session.
bool Sensitive(const Row& r) {
    const std::string k = r.key;
    return r.kind == Kind::Identity || k.find("pass") != std::string::npos ||
           k.find("token") != std::string::npos || k.find("secret") != std::string::npos;
}

// The resolved value in the one spelling a reader and a rig both compare: a flag is 1 or 0, an
// integer its own digits, a float the catalog's C-locale emission, an enum its canonical token.
// Every kind goes through the same *FromRaw core the live Resolve functions use, so the census
// cannot report a value the product would not read.
std::string Resolved(const Row& r, const std::string& raw) {
    char buf[64];
    switch (r.kind) {
        case Kind::Flag:
            return internal::FlagFromRaw(&r, true, raw) ? "1" : "0";
        case Kind::Int:
            std::snprintf(buf, sizeof(buf), "%ld", internal::IntFromRaw(&r, true, raw));
            return buf;
        case Kind::Float:
            return internal::FormatFloat(internal::FloatFromRaw(&r, true, raw));
        case Kind::Enum:
            return internal::EnumFromRaw(&r, true, raw);
        case Kind::String:
        case Kind::Identity:
            break;
    }
    return raw;  // free strings are unvalidated, and an identity never reaches here
}

}  // namespace

void ReportEffectiveConfig() {
    size_t count = 0;
    const Row* rows = config_registry::Rows(count);
    int configured = 0;
    for (size_t i = 0; i < count; ++i) {
        const Row& r = rows[i];
        std::string raw;
        bool fromEnv = false;
        // Nobody configured it: the row default stands, and saying so for every untouched row
        // would bury the handful that were.
        if (!internal::PickRawLayered(&r, raw, &fromEnv)) continue;
        ++configured;
        // A raw value the reader refuses leaves the row at its default. That is the failure a rig
        // has to see stated rather than infer from a value that merely looks wrong, and it is the
        // same validator the writer and the boot sweep use, so there is one verdict per value.
        std::string why;
        const bool valid = ValueValidForKey(r.key, raw, &why);
        const std::string value = Sensitive(r) ? "<set>" : Resolved(r, raw);
        if (valid)
            UE_LOGI("config: EFFECTIVE %s=%s (%s)", r.key, value.c_str(),
                    fromEnv ? "env" : "ini");
        else
            UE_LOGW("config: EFFECTIVE %s=%s (%s, rejected: %s)", r.key, value.c_str(),
                    fromEnv ? "env" : "ini", why.c_str());
    }
    UE_LOGI("config: EFFECTIVE end -- %d row(s) configured", configured);
}

}  // namespace coop::config
