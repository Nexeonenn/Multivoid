// coop/thanks/thanks_list.h -- the thanks list: the people the main menu names under the version
// lines, grouped in sections. The names are data, never code: one text file
// (assets/thanks/thanks.txt) is embedded as the fallback and served by the master at /v1/thanks,
// so a name is added or taken out by publishing the file, with no release. The game keeps its
// supporter list the same way (downloaded, one name per line under a tier tag); what differs is
// that this one has a real fallback, is cached beside the executable, and is fetched only where
// the mod already talks to its master, never from the title screen alone.
//
// Three copies can exist: embedded, cached, just fetched. The highest `revision` wins, and at
// equal revisions the later source does (fetched over cached over embedded), so a master holding
// an old file cannot take names away from a newer build. The format is in the data file's header.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::thanks_list {

struct Section {
    std::string title;               // UTF-8, as written in the header line
    uint32_t    rgb = 0xFFFFFF;      // sRGB 0xRRGGBB
    bool        rightColumn = false;
    std::vector<std::string> names;  // UTF-8, one per line of the file
};

struct List {
    int         revision = 0;
    std::string title;
    std::vector<Section> sections;   // only sections that hold a name
};

// Parses one copy of the file. Text from the master is untrusted: the size, the section count
// and the name count are capped, a name that is not well-formed UTF-8 is dropped whole, control
// characters are stripped. False when nothing displayable came out, `out` then untouched. Pure.
bool Parse(const char* text, size_t size, List& out);

// Loads the embedded copy and the cached one and keeps the winner. Once, at boot, any thread.
void Init();

// Fetches the master's copy on a worker and, if it wins, adopts and caches it. Coalesced and
// rate-floored like the version check it rides with; silent when the master has no list.
void RefreshFromMaster();

// The current list, and a counter that moves when it is replaced, so a consumer rebuilds on a
// change and otherwise asks only this. Any thread.
uint64_t Generation();
uint64_t Copy(List& out);

}  // namespace coop::thanks_list
