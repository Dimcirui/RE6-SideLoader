#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace loose {

enum class Mode { Engine, Patch };

struct Config {
    bool enable = true;
    bool logOpens = false;      // log every file the game opens (like QuestLoader's outputEveryPath)
    Mode mode = Mode::Engine;   // how loose resources (data\..., effect\...) reach the game, see loose.cpp
    std::wstring gameDir;       // directory of BH6.exe, no trailing slash
    std::wstring root;          // loose root (holds the mod folders), absolute, no trailing slash
    std::wstring cacheDir;      // patched arcs, absolute
    std::vector<std::wstring> order;  // [Loader] Order=: mods to load, last wins (empty = all, alphabetical)
};

extern Config g_cfg;

void Log(const wchar_t* fmt, ...);
void LoadConfig();

// Returns the path the game should open instead of `path`, or an empty string to keep it.
std::wstring Resolve(const wchar_t* path);

// rArchive entry filter (engine mode): false = the arc must not load this entry, a loose file replaces it.
bool KeepArcEntry(const char* arcPath, uint32_t type, const char* path);

// One loose file that replaces (or adds) an arc entry.
struct Override {
    std::string key;            // entry name, lower case, backslashes, no extension
    std::string name;           // entry name as written into a new entry (original case)
    uint32_t type = 0;          // ARC type hash from the extension
    std::wstring file;          // absolute path of the loose file
    bool canAdd = false;        // false for global resources: only replaces entries the arc already has
    uint64_t size = 0;
    FILETIME mtime{};
};

// Rebuilds `src` into `dst` with the overrides applied (arc_patch.cpp). Writes `dst` atomically.
bool PatchArc(const std::wstring& src, const std::vector<Override>& ovs, const std::wstring& dst,
              int* replaced, int* added);

// Entry keys (lower-case name + type) of an arc, used to filter global overrides.
bool ReadArcKeys(const std::wstring& src, std::vector<std::pair<std::string, uint32_t>>* keys);

// hooks.cpp: kernel32 import hooks of the game module, and the rArchive entry filter of BH6.exe.
void InstallHooks();
bool InstallArcFilter();

}  // namespace loose
