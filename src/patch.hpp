#pragma once
#include "platform.hpp"
#include "light_registry.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pk {
enum class EngineVariant { Unknown, Black, Overdose, Proto2003 };
using AddLight = void (__thiscall*)(void*, void*);
using ResetLights = void (__thiscall*)(void*);
using WorldDraw = void (__thiscall*)(void*, void*, int);
using LightPass = void (__thiscall*)(void*, const void*);
using UberPass = void (__thiscall*)(void*, const void* const*, int);
using GetTime = uint32_t (__thiscall*)(void*);

struct EnginePatch {
    Config config{};
    EngineVariant variant = EngineVariant::Unknown;
    HMODULE self = nullptr;
    std::unique_ptr<light::LightRegistry> lights;
    AddLight origAdd = nullptr;
    ResetLights origReset = nullptr;
    WorldDraw origDraw = nullptr;
    LightPass origLightPass = nullptr;
    UberPass origUberPass = nullptr;
    GetTime origTime = nullptr;
    std::atomic<bool> lightingReady{false};
    std::atomic<bool> installed{false};
};
extern EnginePatch g;

struct Stats {
    std::atomic<unsigned long long> add{0}, reset{0}, draw{0};
    std::atomic<unsigned long long> nativeSingle{0}, nativeUber{0};
    std::atomic<unsigned long long> extra{0}, unsafe{0}, incomplete{0};
    std::atomic<unsigned long long> noOverflow{0};
};
extern Stats stats;

template<typename T>
inline void bump(std::atomic<T>& value, T amount = 1) noexcept {
    if (g.config.logLights) value.fetch_add(amount, std::memory_order_relaxed);
}
void maybeLogDiagnostics();
bool initialize(HMODULE self);
bool classifyLight(const void* light, light::LightRegistry::LightRecord& out);
void __fastcall hookAdd(void* entity, void*, void* light);
void __fastcall hookReset(void* entity, void*);
void __fastcall hookDraw(void* mesh, void*, void* renderDevice, int flags);
void __fastcall hookLightPass(void* mesh, void*, const void* light);
void __fastcall hookUber(void* mesh, void*, const void* const* lights, int count);
uint32_t __fastcall hookTime(void* driver, void*);

// This scope lives ONLY while original WorldMesh::Draw is running.
// Additional passes are never emitted after WorldMesh::Draw returns.
struct DrawContext {
    const void* mesh = nullptr;
    const light::LightRegistry::LightRecord* queued = nullptr;
    std::size_t queuedCount = 0;
    const void* seen[16]{};
    unsigned seenCount = 0;
    bool dispatched = false;
    DrawContext* previous = nullptr;
    void note(const void* light) noexcept {
        if (!light) return;
        for (unsigned i = 0; i < seenCount; ++i) if (seen[i] == light) return;
        if (seenCount < 16) seen[seenCount++] = light;
    }
};
extern thread_local DrawContext* tlsDraw;
}
