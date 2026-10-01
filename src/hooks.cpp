// kernel32 import hooks of the game module (IAT patching, no code patching / signatures needed).
#include "loose.h"

namespace loose {
namespace {

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FindFirstFileW_t = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileA_t = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);

CreateFileW_t oCreateFileW = CreateFileW;
CreateFileA_t oCreateFileA = CreateFileA;
FindFirstFileW_t oFindFirstFileW = FindFirstFileW;
FindFirstFileA_t oFindFirstFileA = FindFirstFileA;

const DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC |
                           WRITE_OWNER;

std::wstring Widen(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

// read-only open of an existing file -> redirected path, else empty
std::wstring RedirectOpen(const wchar_t* path, DWORD access, DWORD disposition) {
    if (g_cfg.logOpens) Log(L"open %s (access %08x, disposition %u)", path, access, disposition);
    if ((access & kWriteAccess) || disposition != OPEN_EXISTING) return L"";
    return Resolve(path);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
    std::wstring r = path ? RedirectOpen(path, access, disp) : L"";
    if (!r.empty()) {
        HANDLE h = oCreateFileW(r.c_str(), access, share, sa, disp, flags, tmpl);
        if (h != INVALID_HANDLE_VALUE) return h;
        Log(L"  redirected open failed (error %u), using the original file", GetLastError());
    }
    return oCreateFileW(path, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI HookCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                              DWORD flags, HANDLE tmpl) {
    std::wstring r = path ? RedirectOpen(Widen(path).c_str(), access, disp) : L"";
    if (!r.empty()) {
        HANDLE h = oCreateFileW(r.c_str(), access, share, sa, disp, flags, tmpl);
        if (h != INVALID_HANDLE_VALUE) return h;
        Log(L"  redirected open failed (error %u), using the original file", GetLastError());
    }
    return oCreateFileA(path, access, share, sa, disp, flags, tmpl);
}

bool HasWildcard(const std::wstring& s) { return s.find_first_of(L"*?") != std::wstring::npos; }

// FindFirstFile on an exact path reports size/time: answer for the file the game will really get
HANDLE WINAPI HookFindFirstFileW(LPCWSTR name, LPWIN32_FIND_DATAW data) {
    if (name && !HasWildcard(name)) {
        std::wstring r = Resolve(name);
        if (!r.empty()) {
            HANDLE h = oFindFirstFileW(r.c_str(), data);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }
    return oFindFirstFileW(name, data);
}

HANDLE WINAPI HookFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data) {
    std::wstring w = Widen(name);
    if (name && !HasWildcard(w)) {
        std::wstring r = Resolve(w.c_str());
        WIN32_FIND_DATAW fw{};
        HANDLE h = r.empty() ? INVALID_HANDLE_VALUE : oFindFirstFileW(r.c_str(), &fw);
        if (h != INVALID_HANDLE_VALUE) {
            data->dwFileAttributes = fw.dwFileAttributes;
            data->ftCreationTime = fw.ftCreationTime;
            data->ftLastAccessTime = fw.ftLastAccessTime;
            data->ftLastWriteTime = fw.ftLastWriteTime;
            data->nFileSizeHigh = fw.nFileSizeHigh;
            data->nFileSizeLow = fw.nFileSizeLow;
            data->dwReserved0 = fw.dwReserved0;
            data->dwReserved1 = fw.dwReserved1;
            WideCharToMultiByte(CP_ACP, 0, fw.cFileName, -1, data->cFileName, MAX_PATH, nullptr, nullptr);
            WideCharToMultiByte(CP_ACP, 0, fw.cAlternateFileName, -1, data->cAlternateFileName, 14, nullptr,
                                nullptr);
            return h;
        }
    }
    return oFindFirstFileA(name, data);
}

struct Hook {
    const char* name;
    void* hook;
    void** original;
};

int PatchImports(HMODULE mod, const Hook* hooks, int count) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(base + imp->Name), "kernel32.dll") != 0) continue;
        if (!imp->OriginalFirstThunk) continue;  // no name table: cannot match by name
        auto* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto* slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            for (int i = 0; i < count; i++) {
                if (lstrcmpA((const char*)byName->Name, hooks[i].name) != 0) continue;
                DWORD old;
                if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) break;
                *hooks[i].original = (void*)slots->u1.Function;
                slots->u1.Function = (ULONG_PTR)hooks[i].hook;
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                patched++;
                break;
            }
        }
    }
    return patched;
}

// ---- rArchive entry filter -------------------------------------------------------------------------------------
// BH6.exe 0x118d200 (Steam build of 2025-08), called by rArchive::load for every entry:
//   mov ecx,[ctx]; test ecx,ecx; jz keep; mov eax,[fn]; test eax,eax; jz keep;
//   push entry name; push dti; push arc path; call eax (thiscall, this = ctx); ret;  keep: mov al,1; ret
// Nothing in the retail game sets ctx/fn, so every entry loads. Returning false skips the entry; the resource is
// then loaded by path as a loose file by sResource. The filter is also called after creating a resource on the
// other load path of rArchive (same arguments, path of the created resource).
const uint8_t kFilterSig[] = {0x8B, 0x0D, 0, 0, 0, 0, 0x85, 0xC9, 0x74, 0x1B, 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74,
                              0x12, 0x8B, 0x54, 0x24, 0x0C, 0x52, 0x8B, 0x54, 0x24, 0x0C, 0x52, 0x8B, 0x54, 0x24,
                              0x0C, 0x52, 0xFF, 0xD0, 0xC3, 0xB0, 0x01, 0xC3};
const bool kFilterWild[sizeof(kFilterSig)] = {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1};

struct MtDTI {  // MtDTI layout of RE6: +0x04 class name, +0x1C type hash (~crc32(name) & 0x7FFFFFFF)
    void* vtable;
    const char* name;
    uint8_t pad[0x14];
    uint32_t id;
};

int g_filterCtx = 1;

// thiscall emulated with fastcall: ecx = ctx, edx unused, three stack arguments popped by the callee
bool __fastcall ArcFilter(void*, void*, const char* arcPath, const MtDTI* dti, const char* path) {
    __try {
        return KeepArcEntry(arcPath, dti ? dti->id : 0, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

uint8_t* FindSig(HMODULE mod) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* hit = nullptr;
    int hits = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        for (size_t i = 0; i + sizeof(kFilterSig) <= n; i++) {
            size_t k = 0;
            while (k < sizeof(kFilterSig) && (kFilterWild[k] || p[i + k] == kFilterSig[k])) k++;
            if (k == sizeof(kFilterSig)) hit = p + i, hits++;
        }
    }
    return hits == 1 ? hit : nullptr;
}

}  // namespace

bool InstallArcFilter() {
    uint8_t* fn = FindSig(GetModuleHandleW(nullptr));
    if (!fn) {
        Log(L"engine mode: rArchive entry filter not found in this BH6.exe, falling back to patch mode");
        return false;
    }
    void** ctx = *(void***)(fn + 2);
    void** cb = *(void***)(fn + 11);
    if (*ctx || *cb) {
        Log(L"engine mode: the entry filter is already in use (%p / %p), falling back to patch mode", *ctx, *cb);
        return false;
    }
    *cb = (void*)ArcFilter;
    *ctx = &g_filterCtx;
    Log(L"engine mode: rArchive entry filter installed (function %p, globals %p / %p)", fn, ctx, cb);
    return true;
}

void InstallHooks() {
    const Hook hooks[] = {
        {"CreateFileW", (void*)HookCreateFileW, (void**)&oCreateFileW},
        {"CreateFileA", (void*)HookCreateFileA, (void**)&oCreateFileA},
        {"FindFirstFileW", (void*)HookFindFirstFileW, (void**)&oFindFirstFileW},
        {"FindFirstFileA", (void*)HookFindFirstFileA, (void**)&oFindFirstFileA},
    };
    int n = PatchImports(GetModuleHandleW(nullptr), hooks, (int)(sizeof(hooks) / sizeof(hooks[0])));
    Log(L"hooked %d kernel32 imports of the game module", n);
}

}  // namespace loose
