// Stand-in for BH6.exe: loads dinput8.dll from its own folder (like the game does), then
//   test_host.exe open <path> <out file> [A|W]     opens a file through its kernel32 imports, copies it to <out>
//   test_host.exe filter <arc> <type hex> <path>   calls a byte-identical copy of BH6.exe's rArchive entry filter
#include <windows.h>
#include <cstdio>
#include <cstdlib>

extern "C" {
void* g_filterCtx = nullptr;
void* g_filterFn = nullptr;
}

// same bytes as BH6.exe 0x118d200 (8B 0D ctx 85 C9 74 1B A1 fn 85 C0 74 12 ...), so the DLL's signature scan finds it
__declspec(naked) bool __cdecl EngineFilter(const char*, const void*, const char*) {
    __asm {
        mov ecx, dword ptr [g_filterCtx]
        test ecx, ecx
        jz keep
        mov eax, dword ptr [g_filterFn]
        test eax, eax
        jz keep
        mov edx, [esp + 0Ch]
        push edx
        mov edx, [esp + 0Ch]
        push edx
        mov edx, [esp + 0Ch]
        push edx
        call eax
        ret
    keep:
        mov al, 1
        ret
    }
}

struct FakeDTI {
    void* vtable;
    const char* name;
    unsigned char pad[0x14];
    unsigned id;
};

int OpenCopy(const wchar_t* path, const wchar_t* outPath, bool ansi) {
    HANDLE h;
    if (ansi) {
        char p[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, path, -1, p, MAX_PATH, nullptr, nullptr);
        h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA(p, &fd);
        if (f != INVALID_HANDLE_VALUE) printf("FindFirstFileA size %lu\n", fd.nFileSizeLow), FindClose(f);
    } else {
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(path, &fd);
        if (f != INVALID_HANDLE_VALUE) printf("FindFirstFileW size %lu\n", fd.nFileSizeLow), FindClose(f);
    }
    if (h == INVALID_HANDLE_VALUE) return printf("open failed %lu\n", GetLastError()), 5;
    printf("GetFileSize %lu\n", GetFileSize(h, nullptr));
    HANDLE o = CreateFileW(outPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    static char buf[1 << 20];
    DWORD got = 0, put;
    LARGE_INTEGER pos{};
    for (;;) {  // overlapped handles need an explicit offset, like the game's ReadFileEx reads
        OVERLAPPED ov{};
        ov.Offset = pos.LowPart;
        ov.OffsetHigh = (DWORD)pos.HighPart;
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = ReadFile(h, buf, sizeof(buf), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && GetOverlappedResult(h, &ov, &got, TRUE);
        CloseHandle(ov.hEvent);
        if (!ok || !got) break;
        WriteFile(o, buf, got, &put, nullptr);
        pos.QuadPart += got;
    }
    CloseHandle(o);
    CloseHandle(h);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    *wcsrchr(dll, L'\\') = 0;
    lstrcatW(dll, L"\\dinput8.dll");
    HMODULE m = LoadLibraryW(dll);
    if (!m) return printf("cannot load %ls\n", dll), 3;
    typedef HRESULT(WINAPI * DI8)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    DI8 create = (DI8)GetProcAddress(m, "DirectInput8Create");
    if (!create) return printf("no DirectInput8Create export\n"), 4;
    static const GUID IID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    void* di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8W, &di, nullptr);
    printf("DirectInput8Create -> %08lx %s\n", (unsigned long)hr, di ? "(object)" : "");
    if (di) (*(ULONG(WINAPI***)(void*))di)[2](di);  // Release
    printf("filter installed: %s\n", g_filterFn ? "yes" : "no");

    if (!lstrcmpW(argv[1], L"filter") && argc >= 5) {
        char arcPath[260], path[260];
        WideCharToMultiByte(CP_ACP, 0, argv[2], -1, arcPath, 260, nullptr, nullptr);
        WideCharToMultiByte(CP_ACP, 0, argv[4], -1, path, 260, nullptr, nullptr);
        FakeDTI dti{};
        dti.name = "rTest";
        dti.id = wcstoul(argv[3], nullptr, 16);
        bool keep = EngineFilter(arcPath, &dti, path);
        printf("keep %d\n", keep ? 1 : 0);
        return keep ? 0 : 1;
    }
    if (!lstrcmpW(argv[1], L"open") && argc >= 4)
        return OpenCopy(argv[2], argv[3], argc > 4 && argv[4][0] == L'A');
    // reopen <path> <out1> <file to rewrite> <out2>: change a mod file while the "game" keeps running
    if (!lstrcmpW(argv[1], L"reopen") && argc >= 6) {
        int r = OpenCopy(argv[2], argv[3], false);
        HANDLE t = CreateFileW(argv[4], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD put;
        WriteFile(t, "HOT RELOAD", 10, &put, nullptr);
        CloseHandle(t);
        Sleep(300);
        return r | OpenCopy(argv[2], argv[5], false);
    }
    return 2;
}
