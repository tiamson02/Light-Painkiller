#pragma once
#include "platform.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>
#include "light_registry.hpp"

namespace pk {
enum class EngineVariant { Unknown, Black, Overdose, Proto2003 };
using AddLight = void (__thiscall*)(void* entity, void* light);
using ResetLights = void (__thiscall*)(void* entity);
using WorldDraw = void (__thiscall*)(void* mesh, void* renderDevice, int flags);
using LightPass = void (__thiscall*)(void* mesh, const void* light);
using UberPass = void (__thiscall*)(void* mesh, const void* const* lights, int count);
using GetTime = uint32_t (__thiscall*)(void* driver);

struct EnginePatch {
    Config config{};
    EngineVariant variant = EngineVariant::Unknown;
    HMODULE module = nullptr;
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
// Counters deliberately record actual hook execution rather than successful hook installation.
// The snapshots measure registration/selection, not physical light contribution.
struct LightDiagnostics {
    std::atomic<unsigned long long> addCalls{0}, resetCalls{0}, drawCalls{0}, lightFlagDraws{0};
    std::atomic<unsigned long long> nativeSingle{0}, nativeUber{0}, suppressedSingle{0}, suppressedUber{0};
    std::atomic<unsigned long long> nativeFallback{0}, snapshots{0}, nonEmpty{0}, totalRegistered{0};
    std::atomic<unsigned long long> accepted{0}, rejectLight{0}, singleCalls{0}, uberCalls{0};
    std::atomic<unsigned long long> d3dFound{0}, d3dMissing{0}, d3dStateInvalid{0}, d3dFallback{0};
    std::atomic<unsigned long long> noLightFlag{0}, deviceStateHooks{0};
    std::atomic<unsigned long long> limitedLights{0}, limitedPasses{0}, passthroughDraws{0};
    std::atomic<unsigned long long> d3dScans{0}, d3dMissCached{0}, lightSlowValidation{0};
    std::atomic<unsigned long long> deviceCacheHits{0}, singleUberFastPaths{0};
    std::atomic<unsigned long long> classifiedOnAdd{0}, registryEmptyFastPaths{0};
    std::atomic<uintptr_t> lastEntity{0}, lastLight{0}, lastDrawMesh{0};
    std::atomic<unsigned> lastDrawFlags{0}, maxSnapshot{0};
};
extern LightDiagnostics diagnostics;
// Diagnostic atomics are intentionally disabled in release/performance mode.
template<typename T, typename V>
inline void diagAdd(std::atomic<T>& counter, V value, std::memory_order order) noexcept {
    if (g.config.logLights) counter.fetch_add(static_cast<T>(value), order);
}
void maybeLogDiagnostics();
bool initialize(HMODULE self);
void installLighting(HMODULE engine);
void drawAdditionalLights(void* mesh, void* renderDevice, int flags,
                          const light::LightRegistry::LightRecord* lights, std::size_t count);
bool classifyLight(const void* light, light::LightRegistry::LightRecord& out);
void __fastcall hookAdd(void* entity, void* edx, void* light);
void __fastcall hookReset(void* entity, void* edx);
void __fastcall hookDraw(void* mesh, void* edx, void* renderDevice, int flags);
void __fastcall hookLightPass(void* mesh, void* edx, const void* light);
void __fastcall hookUber(void* mesh, void* edx, const void* const* lights, int count);
uint32_t __fastcall hookTime(void* driver, void* edx);
extern thread_local bool tlsSuppress;
}
