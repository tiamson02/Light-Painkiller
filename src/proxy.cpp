#include "patch.hpp"
#include <unknwn.h>
#include <cstring>
#include <mutex>

static HMODULE s_self = nullptr;
static HMODULE s_dinput = nullptr;
static std::once_flag s_dinputOnce;
static HMODULE realDInput() {
    std::call_once(s_dinputOnce, [] {
        char dir[MAX_PATH] = {};
        const UINT len = GetSystemDirectoryA(dir, MAX_PATH);
        if (len && len < MAX_PATH - 14) {
            strcat_s(dir, "\\dinput8.dll");
            s_dinput = LoadLibraryA(dir); // explicit system path to avoid recursion
        }
    });
    return s_dinput;
}
static FARPROC forwarded(const char* name) {
    const auto h = realDInput();
    return h ? GetProcAddress(h, name) : nullptr;
}
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid,
                                               LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT (WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(forwarded("DirectInput8Create"));
    return fn ? fn(hinst, version, riid, out, outer) : E_FAIL;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forwarded("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out) {
    using Fn = HRESULT (WINAPI*)(REFCLSID,REFIID,LPVOID*);
    auto fn = reinterpret_cast<Fn>(forwarded("DllGetClassObject"));
    return fn ? fn(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI DllRegisterServer() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forwarded("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}
extern "C" HRESULT WINAPI DllUnregisterServer() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(forwarded("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}
extern "C" BOOL __cdecl Light_Initialize() {
    return pk::initialize(s_self) ? TRUE : FALSE;
}
// Backward-compatible entry point for Custom EXEs that already use v0.1.6.
extern "C" BOOL __cdecl PKPatch_Initialize() {
    return Light_Initialize();
}
static DWORD WINAPI patchThread(void*) {
    Sleep(1500);
    // Engine.dll may not yet be loaded by the game when this DLL is mapped.
    for (unsigned attempt=0; attempt<120; ++attempt) {
        if (GetModuleHandleA("Engine.dll")) {
            pk::initialize(s_self);
            return 0;
        }
        Sleep(500);
    }
    pk::initLog(s_self);
    pk::log("Engine.dll not found after 60 seconds; patch not installed.");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, patchThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    // Process-lifetime hooks: explicitly unloading the DLL while game runs is unsupported.
    return TRUE;
}
