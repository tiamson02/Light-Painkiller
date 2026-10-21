#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace pk {
// One shared configuration for original Painkiller and compatible mods.
// There are deliberately no renderer-state or mod-specific workarounds.
struct Config {
    bool unlockLights = true;
    unsigned maxLights = 128;            // registry capacity per entity
    unsigned extraPassesPerMesh = 124;   // full 128 point lights when MaxLights=128; costly
    bool timerSizeFix = true;
    bool logLights = false;
};
Config loadConfig(HMODULE self);
void log(const char* format, ...);
void initLog(HMODULE self);
void* exported(HMODULE module, const char* name);
bool readable(const void* p, size_t n);
}
