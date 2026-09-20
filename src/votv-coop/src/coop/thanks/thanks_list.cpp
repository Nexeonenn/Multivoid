// coop/thanks/thanks_list.cpp -- see coop/thanks/thanks_list.h.

#include "coop/thanks/thanks_list.h"

#include "coop/net/lobby_client.h"
#include "coop/session/session_manager.h"  // MasterUrl
#include "coop/session/shutdown.h"
#include "coop/text/utf8_codec.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include "../../../resources/thanks_resource_ids.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace coop::thanks_list {

namespace {

// Bounds on one copy of the file. The game's own list is about 600 names in 10 KB; these leave
// room to grow and keep a hostile master from handing the menu a wall of text.
constexpr size_t kMaxFileBytes  = 64 * 1024;
constexpr size_t kMaxSections   = 8;
constexpr size_t kMaxNames      = 2000;  // across all sections
constexpr size_t kMaxNameBytes  = 64;
constexpr size_t kMaxTitleBytes = 48;

const wchar_t* kCacheFileName = L"multivoid_thanks.txt";
constexpr uint64_t kFetchFloorMs = 8000;  // the same floor the version check keeps

std::mutex g_mu;
List g_list;                 // the winner so far
std::string g_cachedRaw;     // the bytes the cache file holds, so an unchanged fetch writes nothing
std::atomic<uint64_t> g_generation{0};
std::atomic<bool> g_fetchInFlight{false};
std::atomic<uint64_t> g_fetchStartMs{0};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// One displayable string, or empty: control characters out, capped on a character boundary,
// and refused whole when it is not well-formed UTF-8 (a repair would show a name nobody has).
std::string Clean(const std::string& raw, size_t maxBytes) {
    std::string s = Trim(coop::text::SanitizeUtf8(raw.data(), raw.size()));
    s = coop::text::CapUtf8Bytes(std::move(s), maxBytes);
    std::wstring probe;
    if (!coop::text::FromUtf8Strict(s.data(), s.size(), &probe)) return {};
    return s;
}

bool ParseHexColour(const std::string& tok, uint32_t& out) {
    if (tok.size() != 7 || tok[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = tok[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

// `[Title] #RRGGBB left|right`. Anything else after the bracket makes the line a name, so a
// player whose name opens with a bracketed clan tag is not read as a section.
bool ParseSectionHeader(const std::string& line, Section& out) {
    if (line.empty() || line[0] != '[') return false;
    const size_t close = line.find(']');
    if (close == std::string::npos) return false;
    Section s;
    s.title = Clean(line.substr(1, close - 1), kMaxTitleBytes);
    if (s.title.empty()) return false;
    size_t i = close + 1;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
        if (j == i) break;
        const std::string tok = line.substr(i, j - i);
        if (tok == "left") s.rightColumn = false;
        else if (tok == "right") s.rightColumn = true;
        else if (!ParseHexColour(tok, s.rgb)) return false;
        i = j;
    }
    out = std::move(s);
    return true;
}

size_t NameCount(const List& l) {
    size_t n = 0;
    for (const Section& s : l.sections) n += s.names.size();
    return n;
}

// Whether `candidate`, read from a later source than `current`, replaces it.
bool Wins(const List& candidate, const List& current) {
    return candidate.revision >= current.revision;
}

std::wstring CachePath() {
    const std::wstring dir = ue_wrap::paths::ExeDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kCacheFileName;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string buf(kMaxFileBytes + 1, '\0');
    const size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0 || n > kMaxFileBytes) return false;
    buf.resize(n);
    out = std::move(buf);
    return true;
}

// Written beside itself and moved into place, so a crash mid-write leaves the old cache whole.
bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) { ::DeleteFileW(tmp.c_str()); return false; }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool EmbeddedBytes(std::string& out) {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EmbeddedBytes), &self);
    if (!self) return false;
    HRSRC res = ::FindResourceW(self, MAKEINTRESOURCEW(IDR_THANKS_LIST),
                                reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!res) return false;
    HGLOBAL glob = ::LoadResource(self, res);
    const DWORD size = ::SizeofResource(self, res);
    const void* p = glob ? ::LockResource(glob) : nullptr;
    if (!p || size == 0) return false;
    out.assign(static_cast<const char*>(p), size);
    return true;
}

void Adopt(List&& l, const char* source) {
    UE_LOGI("thanks_list: revision %d from the %s copy -- %zu sections, %zu names", l.revision,
            source, l.sections.size(), NameCount(l));
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_list = std::move(l);
    }
    g_generation.fetch_add(1, std::memory_order_release);
}

}  // namespace

bool Parse(const char* text, size_t size, List& out) {
    if (!text || size == 0 || size > kMaxFileBytes) return false;
    size_t pos = 0;
    if (size >= 3 && static_cast<uint8_t>(text[0]) == 0xEF &&
        static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF)
        pos = 3;  // a byte-order mark an editor left
    List l;
    Section cur;
    bool inSection = false;
    size_t names = 0;
    auto closeSection = [&] {
        if (inSection && !cur.names.empty() && l.sections.size() < kMaxSections)
            l.sections.push_back(std::move(cur));
        cur = Section{};
    };
    while (pos < size) {
        size_t eol = pos;
        while (eol < size && text[eol] != '\n') ++eol;
        const std::string line = Trim(std::string(text + pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == ';') continue;
        Section header;
        if (ParseSectionHeader(line, header)) {
            closeSection();
            cur = std::move(header);
            inSection = true;
            continue;
        }
        if (!inSection) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq));
            const std::string val = Trim(line.substr(eq + 1));
            if (key == "revision") l.revision = std::atoi(val.c_str());
            else if (key == "title") l.title = Clean(val, kMaxTitleBytes);
            continue;
        }
        if (names >= kMaxNames) continue;
        std::string name = Clean(line, kMaxNameBytes);
        if (name.empty()) continue;
        cur.names.push_back(std::move(name));
        ++names;
    }
    closeSection();
    if (l.sections.empty()) return false;
    if (l.revision < 0) l.revision = 0;
    out = std::move(l);
    return true;
}

void Init() {
    std::string raw;
    List embedded;
    if (EmbeddedBytes(raw) && Parse(raw.data(), raw.size(), embedded)) {
        Adopt(std::move(embedded), "embedded");
    } else {
        UE_LOGE("thanks_list: the embedded list did not load -- the menu shows none until a "
                "download lands");
    }
    const std::wstring path = CachePath();
    List cached;
    if (path.empty() || !ReadFileBytes(path, raw) || !Parse(raw.data(), raw.size(), cached)) return;
    bool wins;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        wins = Wins(cached, g_list);
        g_cachedRaw = raw;
    }
    if (wins) Adopt(std::move(cached), "cached");
}

void RefreshFromMaster() {
    const uint64_t now = ::GetTickCount64();
    const uint64_t last = g_fetchStartMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kFetchFloorMs) return;
    if (g_fetchInFlight.exchange(true)) return;
    g_fetchStartMs.store(now, std::memory_order_relaxed);
    const std::string masterUrl = coop::session_manager::MasterUrl();
    std::thread([masterUrl] {
        try {
            std::string raw;
            List fetched;
            if (!coop::shutdown::IsShuttingDown() &&
                coop::net::lobby::LobbyClient::FetchThanks(masterUrl, 8000, raw) &&
                Parse(raw.data(), raw.size(), fetched)) {
                bool wins, changed;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    wins = Wins(fetched, g_list);
                    changed = raw != g_cachedRaw;
                }
                if (!wins) {
                    UE_LOGI("thanks_list: the master's copy is revision %d, older than ours -- kept",
                            fetched.revision);
                } else if (changed) {
                    const std::wstring path = CachePath();
                    if (path.empty() || !WriteFileBytes(path, raw))
                        UE_LOGW("thanks_list: the downloaded list could not be cached; it lasts "
                                "this run only");
                    {
                        std::lock_guard<std::mutex> lk(g_mu);
                        g_cachedRaw = raw;
                    }
                    Adopt(std::move(fetched), "master's");
                }
            }
        } catch (const std::exception& e) {
            UE_LOGW("thanks_list: fetch worker exception: %s", e.what());
        }
        g_fetchInFlight.store(false, std::memory_order_release);
    }).detach();
}

uint64_t Generation() { return g_generation.load(std::memory_order_acquire); }

uint64_t Copy(List& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    out = g_list;
    return g_generation.load(std::memory_order_acquire);
}

}  // namespace coop::thanks_list
