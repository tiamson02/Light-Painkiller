#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pk {
struct Config {
    bool unlockLights = true;
    unsigned maxLights = 128;
    bool timerSizeFix = true;
    bool hookD3DState = true;
    bool trustEnginePointers = true; // fast: skip VirtualQuery for live engine objects
    bool fastSingleUberPass = true; // skip D3D state backup when one native pass suffices

    bool logLights = false;
    bool preserveNativePasses = false; // diagnostic ONLY: do not suppress vanilla light passes
    bool safeNativeFallback = true;   // preserve vanilla passes if this mesh has no collected lights
    bool renderExtraLights = true;     // diagnostic A/B: keep hooks but perform no extra passes
    unsigned maxRenderLightsPerMesh = 12; // CPU/GPU budget, independent of queue capacity
    unsigned maxExtraPassesPerMesh = 4;   // total extra native render calls per draw
};
Config loadConfig(HMODULE self);
void log(const char* format, ...);
void initLog(HMODULE self);
void* exported(HMODULE module, const char* name);
uint32_t read32(const void* p);
uint8_t read8(const void* p);
bool readable(const void* p, size_t n);
}
