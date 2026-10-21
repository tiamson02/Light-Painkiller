#include "platform.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace pk {
static std::mutex logMutex;
static char logPath[MAX_PATH] = "Light.log";
static std::once_flag logSessionOnce;

void initLog(HMODULE self) {
    // One log file per process session. Re-entrant attempts to initialize
    // Light (proxy thread + explicit Custom EXE entry point) must NOT wipe it.
    std::call_once(logSessionOnce, [self]() {
        char path[MAX_PATH] = {};
        if (self && GetModuleFileNameA(self, path, MAX_PATH)) {
            char* slash = strrchr(path, '\\');
            if (slash) {
                *(slash + 1) = '\0';
                snprintf(logPath, sizeof(logPath), "%sLight.log", path);
            }
        }
        std::lock_guard<std::mutex> guard(logMutex);
        HANDLE file = CreateFileA(logPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        else OutputDebugStringA("[Light] Failed to reset Light.log for this session\n");
    });
}
void log(const char* fmt, ...) {
    char msg[1024] = {};
    va_list v;
    va_start(v, fmt);
    vsnprintf(msg, sizeof(msg)-1, fmt, v);
    va_end(v);
    OutputDebugStringA("[Light] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    std::lock_guard<std::mutex> guard(logMutex);
    HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const char prefix[] = "[Light] ";
        WriteFile(file, prefix, (DWORD)strlen(prefix), &written, nullptr);
        WriteFile(file, msg, (DWORD)strlen(msg), &written, nullptr);
        WriteFile(file, "\r\n", 2, &written, nullptr);
        CloseHandle(file);
    }
}
Config loadConfig(HMODULE self) {
    Config c;
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, sizeof(path) - (slash + 1 - path), "Light.ini");
    else strcpy_s(path, "Light.ini");
    auto get = [&](const char* key, int def) -> int {
        return static_cast<int>(GetPrivateProfileIntA("Settings", key, def, path));
    };
    c.unlockLights = get("UnlockLights", 1) != 0;
    c.maxLights = static_cast<unsigned>(std::clamp(get("MaxLights", 128), 4, 512));
    c.extraPassesPerMesh = static_cast<unsigned>(std::clamp(get("ExtraPassesPerMesh", 2), 0, 16));
    c.timerSizeFix = get("TimerSizeFix", 1) != 0;
    c.logLights = get("LogLights", 0) != 0;
    log("Config path: %s; UnlockLights=%d MaxLights=%u ExtraPassesPerMesh=%u TimerSizeFix=%d LogLights=%d",
        path, c.unlockLights, c.maxLights, c.extraPassesPerMesh,
        c.timerSizeFix, c.logLights);
    return c;
}
void* exported(HMODULE mod, const char* name) {
    return mod ? reinterpret_cast<void*>(GetProcAddress(mod, name)) : nullptr;
}
bool readable(const void* p, size_t n) {
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(p, &m, sizeof m) != sizeof m || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    DWORD prot = m.Protect & 0xFF;
    bool good = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress);
    uintptr_t start = reinterpret_cast<uintptr_t>(p);
    return good && start >= base && (start - base) <= m.RegionSize && n <= m.RegionSize - (start - base);
}
}
