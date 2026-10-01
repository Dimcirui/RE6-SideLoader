// Mod folders, the loose-file index and path redirection.
//
// <root> (nativePC_mod\) holds mod folders; every mod folder is laid out like nativePC itself:
//   <mod>\data\...\pl0600.mod      loose resource: the inner arc path, no arc name (also effect\, soft\, sound\ ...)
//   <mod>\arc\DX9\MenuAda.arc      side-loaded arc: replaces nativePC\arc\DX9\MenuAda.arc (or nativePC_dlc\...)
//   <mod>\arc\DX9\MenuAda\...      entries for that one arc only (replace or add)
// Folders of root named like a nativePC folder (data, arc, ...) make root itself the lowest-priority mod.
// Mods load in alphabetical order (or [Loader] Order=), a later mod wins over an earlier one.
//
// Loose resources reach the game in one of two ways ([Loader] Mode=):
//   engine  BH6.exe's rArchive has an (unused, dev-only) entry filter hook. We install it: arcs skip every entry
//           that has a loose resource, the resource manager then loads that path as a loose file
//           (<game>\nativePC\<path>.<ext>), and the CreateFile hook serves the mod file.
//   patch   every arc that contains the resource is rebuilt with the loose file (cached), entries never go missing.
// Files that no arc contains (new textures etc.) are served through the CreateFile hook in both modes.
#include "loose.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ext_table.h"

namespace loose {

Config g_cfg;

namespace {

CRITICAL_SECTION g_logLock;
CRITICAL_SECTION g_resolveLock;
SRWLOCK g_indexLock = SRWLOCK_INIT;
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_watch = INVALID_HANDLE_VALUE;
DWORD g_lastWatchTry = 0;
bool g_dirty = true;
std::set<std::wstring> g_unknownExts;

const wchar_t kManifestVersion[] = L"re6_sideloader 2";
// written next to BH6.exe when there is no ini (keep in sync with the defaults in LoadConfig and re6_sideloader.ini)
const char kDefaultIni[] =
    "; RE6 Sideloader settings (all keys optional; delete this file to get the defaults back)\r\n"
    "[Loader]\r\n"
    "; 0 = load nothing, the DLL only forwards DirectInput\r\n"
    "Enable=1\r\n"
    "; engine = arcs skip entries that have loose files, the game loads those as loose files (real loose loading)\r\n"
    "; patch  = arcs that contain a loose file are rebuilt with it (cached), the game always sees complete arcs\r\n"
    "Mode=engine\r\n"
    "; folder holding the mod folders, relative to the game folder or absolute\r\n"
    "Root=nativePC_mod\r\n"
    "; rebuilt arcs (patch mode only, safe to delete at any time)\r\n"
    "CacheDir=nativePC_mod_cache\r\n"
    "; mods to load, comma separated, later ones win; empty = every folder of Root in alphabetical order\r\n"
    "Order=\r\n"
    "; write re6_sideloader.log next to BH6.exe\r\n"
    "LogFile=1\r\n"
    "; log EVERY file the game opens (find out what the game reads); slow, debug only\r\n"
    "LogOpens=0\r\n";
// top-level folders of nativePC / inner arc paths
const wchar_t* const kNativeDirs[] = {L"arc",    L"sa",  L"data",       L"effect", L"effect_dlc", L"soft", L"sound",
                                      L"sound_dlc", L"system", L"sc", L"etc", L"movie", L"update"};
// game folders whose files mods can replace; "resource" is the engine's loose root for resources without the
// nativePC flag (empty in retail)
const wchar_t* const kGameRoots[] = {L"nativePC", L"nativePC_dlc", L"resource"};

struct FileRec {
    std::wstring file;
    std::wstring mod;
    uint64_t size = 0;
    FILETIME mtime{};
};

struct Index {
    std::map<std::wstring, FileRec> files;                   // lower-case path inside nativePC -> winning file
    std::unordered_map<std::string, Override> resources;     // "path|type" -> loose resource (not under arc\, sa\)
    std::unordered_map<std::string, std::vector<std::string>> byStem;  // lower path without extension -> keys
};
Index g_index;

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) ==
                                       CSTR_EQUAL;
}

bool IStartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

bool IEndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
}

std::wstring LowerW(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

std::string LowerA(const char* s) {
    std::string out(s ? s : "");
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    return out;
}

std::string MapKey(const std::string& key, uint32_t type) {
    char buf[16];
    wsprintfA(buf, "|%08x", type);
    return key + buf;
}

bool IsNativeDir(const std::wstring& name) {
    for (auto d : kNativeDirs)
        if (IEquals(name, d)) return true;
    return false;
}

std::wstring FullPath(const wchar_t* path) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(path, (DWORD)(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return L"";
    return std::wstring(buf, n);
}

bool FileInfo(const std::wstring& path, uint64_t* size, FILETIME* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    if (size) *size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    if (mtime) *mtime = fa.ftLastWriteTime;
    return true;
}

bool IsDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool TypeOfExt(const std::wstring& ext, uint32_t* type) {
    for (auto& e : kExtTable)
        if (IEquals(ext, e.ext)) return *type = e.hash, true;
    if (ext.size() == 8) {  // unknown classes are extracted with the hash as extension
        uint32_t v = 0;
        for (wchar_t c : ext) {
            int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10
                                                      : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | (uint32_t)d;
        }
        return *type = v, true;
    }
    return false;
}

// "data\chara\x.mod" -> entry name "data\chara\x" + type; false (logged once per extension) when not a resource
bool MakeOverride(const std::wstring& rel, const FileRec& rec, bool canAdd, Override* o) {
    size_t slash = rel.find_last_of(L'\\');
    size_t dot = rel.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) || dot == 0) return false;
    std::wstring ext = rel.substr(dot + 1);
    if (!TypeOfExt(ext, &o->type)) {
        if (g_unknownExts.insert(LowerW(ext)).second)
            Log(L"  not a resource (unknown extension .%s): %s", ext.c_str(), rec.file.c_str());
        return false;
    }
    std::wstring stem = rel.substr(0, dot);
    o->name.clear();
    o->key.clear();
    for (wchar_t c : stem) {
        if (c <= 0 || c >= 0x80) {
            Log(L"  ignored, entry names must be ASCII: %s", rec.file.c_str());
            return false;
        }
        o->name.push_back((char)c);
        o->key.push_back((char)(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    }
    o->file = rec.file;
    o->canAdd = canAdd;
    o->size = rec.size;
    o->mtime = rec.mtime;
    return true;
}

void ScanTree(const std::wstring& dir, const std::wstring& rel, const std::wstring& mod, bool nativeOnly,
              std::map<std::wstring, FileRec>* files) {
    WIN32_FIND_DATAW fd;
    std::wstring here = rel.empty() ? dir : dir + L"\\" + rel;
    HANDLE h = FindFirstFileExW((here + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name.empty() || name[0] == L'.') continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (rel.empty() && nativeOnly && !(isDir && IsNativeDir(name))) continue;
        std::wstring sub = rel.empty() ? name : rel + L"\\" + name;
        if (isDir) {
            ScanTree(dir, sub, mod, false, files);
            continue;
        }
        FileRec r;
        r.file = here + L"\\" + name;
        r.mod = mod;
        r.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        r.mtime = fd.ftLastWriteTime;
        (*files)[LowerW(sub)] = r;  // later mods overwrite earlier ones
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void BuildIndex() {
    Index idx;
    std::vector<std::wstring> mods;  // load order, last wins; L"" = root itself
    bool rootIsMod = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((g_cfg.root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.empty() || name[0] == L'.' ||
                name[0] == L'_')
                continue;
            if (IsNativeDir(name))
                rootIsMod = true;
            else
                mods.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(mods.begin(), mods.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    if (!g_cfg.order.empty()) {
        std::vector<std::wstring> ordered;
        for (auto& o : g_cfg.order)
            for (auto& m : mods)
                if (IEquals(o, m)) ordered.push_back(m);
        mods.swap(ordered);
    }
    if (rootIsMod) ScanTree(g_cfg.root, L"", L"(root)", true, &idx.files);
    for (auto& m : mods) ScanTree(g_cfg.root + L"\\" + m, L"", m, false, &idx.files);

    for (auto& kv : idx.files) {
        const std::wstring& rel = kv.first;
        std::wstring top = rel.substr(0, rel.find(L'\\'));
        if (top == rel || IEquals(top, L"arc") || IEquals(top, L"sa")) continue;
        Override o;
        if (!MakeOverride(rel, kv.second, false, &o)) continue;
        std::string k = MapKey(o.key, o.type);
        idx.byStem[o.key].push_back(k);
        idx.resources.emplace(k, o);
    }
    AcquireSRWLockExclusive(&g_indexLock);
    g_index = std::move(idx);
    ReleaseSRWLockExclusive(&g_indexLock);

    std::wstring list = rootIsMod ? L"(root)" : L"";
    for (auto& m : mods) list += (list.empty() ? L"" : L", ") + m;
    Log(L"index: mods [%s], %u files, %u loose resources", list.c_str(), (unsigned)g_index.files.size(),
        (unsigned)g_index.resources.size());
}

// rebuild the index when anything under root changed (directory change notification)
void Refresh() {
    if (g_watch == INVALID_HANDLE_VALUE) {
        DWORD now = GetTickCount();
        if (now - g_lastWatchTry > 2000 || g_lastWatchTry == 0) {
            g_lastWatchTry = now ? now : 1;
            g_watch = FindFirstChangeNotificationW(
                g_cfg.root.c_str(), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (g_watch != INVALID_HANDLE_VALUE) g_dirty = true;
        }
    } else if (WaitForSingleObject(g_watch, 0) == WAIT_OBJECT_0) {
        if (!FindNextChangeNotification(g_watch)) {
            FindCloseChangeNotification(g_watch);
            g_watch = INVALID_HANDLE_VALUE;
        }
        g_dirty = true;
    }
    if (g_dirty) {
        g_dirty = false;
        BuildIndex();
    }
}

std::wstring Manifest(const std::wstring& src, const std::vector<Override>& ovs) {
    std::wstring m = kManifestVersion;
    wchar_t buf[96];
    uint64_t size = 0;
    FILETIME t{};
    FileInfo(src, &size, &t);
    swprintf(buf, 96, L"|%llu|%08x%08x\n", size, t.dwHighDateTime, t.dwLowDateTime);
    m += L"\n" + src + buf;
    for (auto& o : ovs) {
        swprintf(buf, 96, L"|%llu|%08x%08x|%d\n", o.size, o.mtime.dwHighDateTime, o.mtime.dwLowDateTime,
                 o.canAdd ? 1 : 0);
        m += o.file + buf;
    }
    return m;
}

bool ReadText(const std::wstring& path, std::wstring* text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::wstring s(size / sizeof(wchar_t), L'\0');
    bool ok = size != INVALID_FILE_SIZE && (size == 0 || ReadFile(h, &s[0], size, &got, nullptr)) && got == size;
    CloseHandle(h);
    if (ok) *text = s;
    return ok;
}

void WriteText(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &put, nullptr);
    CloseHandle(h);
}

// arc opened from <gameRoot>\<vrel>, its bytes at src -> patched copy, or empty when nothing applies
std::wstring PatchedArc(const std::wstring& gameRoot, const std::wstring& vrel, const std::wstring& src) {
    std::vector<Override> found;
    std::wstring prefix = LowerW(vrel.substr(0, vrel.size() - 4)) + L"\\";
    for (auto it = g_index.files.lower_bound(prefix); it != g_index.files.end() && IStartsWith(it->first, prefix);
         ++it) {
        // index keys are lower case: take the entry name (kept for added entries) from the file's real path
        const std::wstring& file = it->second.file;
        Override o;
        if (MakeOverride(file.substr(file.size() - (it->first.size() - prefix.size())), it->second, true, &o))
            found.push_back(o);
    }
    if (g_cfg.mode == Mode::Patch && !g_index.resources.empty()) {
        std::vector<std::pair<std::string, uint32_t>> keys;
        if (ReadArcKeys(src, &keys))
            for (auto& k : keys) {
                auto it = g_index.resources.find(MapKey(k.first, k.second));
                if (it != g_index.resources.end()) found.push_back(it->second);
            }
    }
    if (found.empty()) return L"";

    std::vector<Override> ovs;  // arc-specific files come first and win
    std::unordered_set<std::string> seen;
    for (auto& o : found)
        if (seen.insert(MapKey(o.key, o.type)).second) ovs.push_back(o);

    std::wstring dst = g_cfg.cacheDir + L"\\" + gameRoot + L"\\" + vrel;
    std::wstring manifest = Manifest(src, ovs), old;
    if (FileInfo(dst, nullptr, nullptr) && ReadText(dst + L".manifest", &old) && old == manifest) {
        Log(L"%s\\%s -> cached patch (%u loose files)", gameRoot.c_str(), vrel.c_str(), (unsigned)ovs.size());
        return dst;
    }
    DWORD t0 = GetTickCount();
    int replaced, added;
    DeleteFileW((dst + L".manifest").c_str());
    if (!PatchArc(src, ovs, dst, &replaced, &added)) {
        Log(L"%s\\%s -> patch FAILED, the game gets the unpatched arc", gameRoot.c_str(), vrel.c_str());
        return L"";
    }
    WriteText(dst + L".manifest", manifest);
    Log(L"%s\\%s -> patched: %d replaced, %d added, %d ignored (%u ms)", gameRoot.c_str(), vrel.c_str(), replaced,
        added, (int)ovs.size() - replaced - added, GetTickCount() - t0);
    for (auto& o : ovs) Log(L"    %s", o.file.c_str());
    return dst;
}

// a loose resource requested with an extension we name differently (e.g. extracted as .276de8b7)
const Override* ResourceByStem(const std::wstring& vrel) {
    size_t slash = vrel.find_last_of(L'\\'), dot = vrel.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return nullptr;
    std::string stem;
    for (wchar_t c : vrel.substr(0, dot)) {
        if (c <= 0 || c >= 0x80) return nullptr;
        stem.push_back((char)(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c));
    }
    auto it = g_index.byStem.find(stem);
    if (it == g_index.byStem.end()) return nullptr;
    uint32_t type;
    if (TypeOfExt(vrel.substr(dot + 1), &type)) {
        auto r = g_index.resources.find(MapKey(stem, type));
        return r == g_index.resources.end() ? nullptr : &r->second;
    }
    if (it->second.size() != 1) {
        Log(L"  %s: %u loose files share this name, cannot tell which type .%s is", vrel.c_str(),
            (unsigned)it->second.size(), vrel.substr(dot + 1).c_str());
        return nullptr;
    }
    return &g_index.resources.find(it->second[0])->second;
}

}  // namespace

void Log(const wchar_t* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)wcslen(msg);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2100];
    int len = swprintf(line, _countof(line), L"[%02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute, st.wSecond,
                       st.wMilliseconds, msg);
    char utf8[6400];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, utf8, sizeof(utf8), nullptr, nullptr);
    EnterCriticalSection(&g_logLock);
    DWORD put;
    WriteFile(g_log, utf8, (DWORD)bytes, &put, nullptr);
    LeaveCriticalSection(&g_logLock);
}

void LoadConfig() {
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_resolveLock);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    g_cfg.gameDir = dir;

    std::wstring ini = dir + L"\\re6_sideloader.ini";
    bool iniCreated = false;
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {  // first run: write the defaults
        HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD put;
            WriteFile(h, kDefaultIni, (DWORD)(sizeof(kDefaultIni) - 1), &put, nullptr);
            CloseHandle(h);
            iniCreated = true;
        }
    }
    wchar_t buf[4096];
    auto path = [&](const wchar_t* key, const wchar_t* def) {
        GetPrivateProfileStringW(L"Loader", key, def, buf, MAX_PATH, ini.c_str());
        std::wstring p = buf;
        if (p.size() < 2 || p[1] != L':') p = dir + L"\\" + p;
        p = FullPath(p.c_str());
        while (!p.empty() && p.back() == L'\\') p.pop_back();
        return p;
    };
    g_cfg.enable = GetPrivateProfileIntW(L"Loader", L"Enable", 1, ini.c_str()) != 0;
    g_cfg.logOpens = GetPrivateProfileIntW(L"Loader", L"LogOpens", 0, ini.c_str()) != 0;
    bool logFile = GetPrivateProfileIntW(L"Loader", L"LogFile", 1, ini.c_str()) != 0;
    g_cfg.root = path(L"Root", L"nativePC_mod");
    g_cfg.cacheDir = path(L"CacheDir", L"nativePC_mod_cache");
    GetPrivateProfileStringW(L"Loader", L"Mode", L"engine", buf, 64, ini.c_str());
    g_cfg.mode = IEquals(buf, L"patch") ? Mode::Patch : Mode::Engine;
    GetPrivateProfileStringW(L"Loader", L"Order", L"", buf, _countof(buf), ini.c_str());
    {
        std::wstring s = buf, cur;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == L',' || s[i] == L';') {
                while (!cur.empty() && cur.back() == L' ') cur.pop_back();
                size_t b = cur.find_first_not_of(L' ');
                if (b != std::wstring::npos) g_cfg.order.push_back(cur.substr(b));
                cur.clear();
            } else {
                cur.push_back(s[i]);
            }
        }
    }

    if (logFile)
        g_log = CreateFileW((dir + L"\\re6_sideloader.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD put;
        WriteFile(g_log, "\xEF\xBB\xBF", 3, &put, nullptr);
    }
    Log(L"RE6 Sideloader 2.0  game=%s  root=%s  cache=%s  mode=%s  enable=%d  logOpens=%d", g_cfg.gameDir.c_str(),
        g_cfg.root.c_str(), g_cfg.cacheDir.c_str(), g_cfg.mode == Mode::Engine ? L"engine" : L"patch", g_cfg.enable,
        g_cfg.logOpens);
    if (iniCreated) Log(L"no re6_sideloader.ini found, wrote one with the default settings");
    if (!IsDir(g_cfg.root)) Log(L"loose root does not exist yet: %s", g_cfg.root.c_str());
}

std::wstring Resolve(const wchar_t* path) {
    if (!g_cfg.enable || !path || !*path) return L"";
    std::wstring full = FullPath(path);
    if (full.empty()) return L"";
    std::wstring gameRoot, vrel;
    for (auto r : kGameRoots) {
        std::wstring prefix = g_cfg.gameDir + L"\\" + r + L"\\";
        if (IStartsWith(full, prefix)) {
            gameRoot = r;
            vrel = full.substr(prefix.size());
            break;
        }
    }
    if (vrel.empty()) return L"";

    EnterCriticalSection(&g_resolveLock);
    Refresh();
    std::wstring result;
    auto it = g_index.files.find(LowerW(vrel));
    const FileRec* overlay = it == g_index.files.end() ? nullptr : &it->second;
    if (IEndsWith(vrel, L".arc")) {
        std::wstring src = overlay ? overlay->file : full;
        if (FileInfo(src, nullptr, nullptr)) result = PatchedArc(gameRoot, vrel, src);
    } else if (!overlay) {
        if (const Override* o = ResourceByStem(vrel)) {
            result = o->file;
            Log(L"%s\\%s -> loose %s", gameRoot.c_str(), vrel.c_str(), result.c_str());
        }
    }
    if (result.empty() && overlay) {
        result = overlay->file;
        Log(L"%s\\%s -> %s [%s]", gameRoot.c_str(), vrel.c_str(), result.c_str(), overlay->mod.c_str());
    }
    LeaveCriticalSection(&g_resolveLock);
    return result;
}

bool KeepArcEntry(const char* arcPath, uint32_t type, const char* path) {
    if (!g_cfg.enable || g_cfg.mode != Mode::Engine || !path) return true;
    std::string key = MapKey(LowerA(path), type);
    EnterCriticalSection(&g_resolveLock);  // normally the arc was just opened (index fresh), this is cheap
    Refresh();
    LeaveCriticalSection(&g_resolveLock);
    // an arc-specific file (arc\DX9\<arc>\<path>.<ext>) is patched into the arc, so that entry must stay
    std::wstring arcPrefix;
    for (const char* p = arcPath ? arcPath : ""; *p; p++)
        arcPrefix.push_back((wchar_t)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p == '/' ? '\\' : (uint8_t)*p));
    std::string lp = LowerA(path);
    arcPrefix += L"\\" + std::wstring(lp.begin(), lp.end()) + L".";
    AcquireSRWLockShared(&g_indexLock);
    auto it = g_index.resources.find(key);
    std::wstring file = it == g_index.resources.end() ? L"" : it->second.file;
    if (!file.empty() && arcPath) {
        auto f = g_index.files.lower_bound(arcPrefix);
        for (; f != g_index.files.end() && IStartsWith(f->first, arcPrefix); ++f) {
            uint32_t t;
            std::wstring ext = f->first.substr(arcPrefix.size());
            if (ext.find(L'\\') == std::wstring::npos && TypeOfExt(ext, &t) && t == type) {
                file.clear();
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_indexLock);
    if (file.empty()) return true;
    Log(L"arc %S: skip %S (type %08x), loose %s", arcPath ? arcPath : "?", path, type, file.c_str());
    return false;
}

}  // namespace loose
