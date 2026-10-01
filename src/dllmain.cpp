// dinput8.dll proxy: BH6.exe imports DINPUT8.dll!DirectInput8Create, Windows looks for it in the game folder
// first, so this DLL is loaded before the game starts. It forwards the call to the system dinput8.dll and
// installs the loose-file hooks while being loaded (same trick as MHW-QuestLoader's DllLoader).
#include "loose.h"

namespace {

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
DirectInput8Create_t g_real = nullptr;

DirectInput8Create_t RealDirectInput8Create() {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit process through WOW64 redirection
        lstrcatW(path, L"\\dinput8.dll");
        if (HMODULE m = LoadLibraryW(path))
            g_real = (DirectInput8Create_t)GetProcAddress(m, "DirectInput8Create");
        if (!g_real) loose::Log(L"cannot load the system dinput8.dll (%s)", path);
    }
    return g_real;
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out,
                                                   void* outer) {
    auto real = RealDirectInput8Create();
    return real ? real(inst, version, riid, out, outer) : E_FAIL;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        loose::LoadConfig();
        if (loose::g_cfg.enable) {
            loose::InstallHooks();
            if (loose::g_cfg.mode == loose::Mode::Engine && !loose::InstallArcFilter())
                loose::g_cfg.mode = loose::Mode::Patch;
        }
    }
    return TRUE;
}
