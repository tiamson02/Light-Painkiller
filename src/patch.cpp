#include "patch.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <vector>

namespace pk {
EnginePatch g;
LightDiagnostics diagnostics;
static std::mutex initMutex;
thread_local bool tlsSuppress = false;

static const char* kAdd      = "?AddLight@Entity@@QAEXPAVLight@@@Z";
static const char* kReset    = "?ResetLights@Entity@@QAEXXZ";
static const char* kDraw     = "?Draw@WorldMesh@@UAEXPAVRenderDevice@@H@Z";
static const char* kUber     = "?RenderUberLightPass@WorldMesh@@AAEXQAPBVLight@@H@Z";
static const char* kSingleA  = "?RenderLightPass@WorldMesh@@AAEXPBVLight@@@Z";
static const char* kSingleB  = "?RenderLightPass@WorldMesh@@AAEXPAVLight@@@Z";
static const char* kTime     = "?GetCurrentTimeMS@SystemDriver@@QBEKXZ";

void __fastcall hookAdd(void* entity, void*, void* light) {
    diagAdd(diagnostics.addCalls, 1, std::memory_order_relaxed);
    if (g.config.logLights) {
        diagnostics.lastEntity.store(reinterpret_cast<uintptr_t>(entity), std::memory_order_relaxed);
        diagnostics.lastLight.store(reinterpret_cast<uintptr_t>(light), std::memory_order_relaxed);
    }
    if (g.origAdd) g.origAdd(entity, light);
    if (g.lightingReady && entity && light && g.lights) {
        light::LightRegistry::LightRecord record{};
        const bool valid = classifyLight(light, record);
        diagAdd(diagnostics.classifiedOnAdd, 1, std::memory_order_relaxed);
        g.lights->addClassified(entity, light, record.flags, record.special, record.type, valid);
    }
}
void __fastcall hookReset(void* entity, void*) {
    diagAdd(diagnostics.resetCalls, 1, std::memory_order_relaxed);
    if (g.lightingReady && g.lights) g.lights->reset(entity);
    if (g.origReset) g.origReset(entity);
}
void __fastcall hookLightPass(void* mesh, void*, const void* light) {
    if (tlsSuppress) diagAdd(diagnostics.suppressedSingle, 1, std::memory_order_relaxed);
    else {
        diagAdd(diagnostics.nativeSingle, 1, std::memory_order_relaxed);
        if (g.origLightPass) g.origLightPass(mesh, light);
    }
}
void __fastcall hookUber(void* mesh, void*, const void* const* lights, int count) {
    if (tlsSuppress) diagAdd(diagnostics.suppressedUber, 1, std::memory_order_relaxed);
    else {
        diagAdd(diagnostics.nativeUber, 1, std::memory_order_relaxed);
        if (g.origUberPass) g.origUberPass(mesh, lights, count);
    }
}
void __fastcall hookDraw(void* mesh, void*, void* renderDevice, int flags) {
    if (!g.origDraw) return;
    diagAdd(diagnostics.drawCalls, 1, std::memory_order_relaxed);
    if (g.config.logLights) {
        diagnostics.lastDrawMesh.store(reinterpret_cast<uintptr_t>(mesh), std::memory_order_relaxed);
        diagnostics.lastDrawFlags.store(static_cast<unsigned>(flags), std::memory_order_relaxed);
    }
    const bool lightingFlag = (static_cast<uint32_t>(flags) & 0x40000000u) != 0;
    if (lightingFlag) diagAdd(diagnostics.lightFlagDraws, 1, std::memory_order_relaxed);
    else diagAdd(diagnostics.noLightFlag, 1, std::memory_order_relaxed);
    if (!g.lightingReady || !lightingFlag || !g.config.renderExtraLights) {
        if (!g.config.renderExtraLights) diagAdd(diagnostics.passthroughDraws, 1, std::memory_order_relaxed);
        g.origDraw(mesh, renderDevice, flags);
        if (g.config.logLights) maybeLogDiagnostics();
        return;
    }

    // Most WorldMesh objects have no tracked lights: leave stock rendering alone.
    // Avoid a mutex/hash lookup and scratch initialization for these meshes.
    if (!g.lights || !g.lights->hasAny()) {
        diagAdd(diagnostics.registryEmptyFastPaths, 1, std::memory_order_relaxed);
        diagAdd(diagnostics.nativeFallback, 1, std::memory_order_relaxed);
        g.origDraw(mesh, renderDevice, flags);
        if (g.config.logLights) maybeLogDiagnostics();
        return;
    }

    // Snapshot only once: the previous implementation copied the light list
    // twice, and locked the registry twice for every drawn mesh.
    // Stack snapshot: no allocation/copy-construction of std::vector per mesh.
    // Default initialization does not zero 512 entries on each draw.
    std::array<light::LightRegistry::LightRecord, 512> lights;
    const std::size_t lightCount = g.lights->snapshotRecordsInto(mesh, lights.data(), lights.size());
    const bool missingLights = lightCount == 0;
    if (missingLights) diagAdd(diagnostics.nativeFallback, 1, std::memory_order_relaxed);
    const bool preserve = g.config.preserveNativePasses || missingLights;
    const bool old = tlsSuppress;
    tlsSuppress = !preserve;
    g.origDraw(mesh, renderDevice, flags);
    tlsSuppress = old;
    if (!old && !missingLights)
        drawAdditionalLights(mesh, renderDevice, flags, lights.data(), lightCount);
    if (g.config.logLights) maybeLogDiagnostics();
}

void maybeLogDiagnostics() {
    if (!g.config.logLights) return;
    static std::atomic<DWORD> nextTime{0};
    const DWORD now = GetTickCount();
    DWORD prev = nextTime.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(now-prev) < 0) return;
    if (!nextTime.compare_exchange_strong(prev, now + 5000, std::memory_order_relaxed)) return;
    auto get = [](const std::atomic<unsigned long long>& v) -> unsigned long long {
        return v.load(std::memory_order_relaxed);
    };
    log("DIAG hookCalls Add=%llu Reset=%llu Draw=%llu DrawFlag=%llu SkipFlag=%llu NativeSingle=%llu NativeUber=%llu SuppressedSingle=%llu SuppressedUber=%llu",
        get(diagnostics.addCalls), get(diagnostics.resetCalls), get(diagnostics.drawCalls),
        get(diagnostics.lightFlagDraws), get(diagnostics.noLightFlag),
        get(diagnostics.nativeSingle), get(diagnostics.nativeUber),
        get(diagnostics.suppressedSingle), get(diagnostics.suppressedUber));
    log("DIAG lightRegistry tracked=%u snapshots=%llu nonempty=%llu totalSeen=%llu maxSnapshot=%u accepted=%llu rejected=%llu nativeFallback=%llu lastAddEntity=%p lastAddLight=%p lastDrawMesh=%p lastFlags=0x%08X",
        g.lights ? static_cast<unsigned>(g.lights->trackedEntities()) : 0,
        get(diagnostics.snapshots), get(diagnostics.nonEmpty),
        get(diagnostics.totalRegistered), diagnostics.maxSnapshot.load(),
        get(diagnostics.accepted), get(diagnostics.rejectLight), get(diagnostics.nativeFallback),
        reinterpret_cast<void*>(diagnostics.lastEntity.load()),
        reinterpret_cast<void*>(diagnostics.lastLight.load()),
        reinterpret_cast<void*>(diagnostics.lastDrawMesh.load()),
        diagnostics.lastDrawFlags.load());
    log("DIAG render singleCalls=%llu uberCalls=%llu deviceFound=%llu deviceMissing=%llu invalidState=%llu stateHookInstalled=%llu fallbackSingles=%llu",
        get(diagnostics.singleCalls), get(diagnostics.uberCalls), get(diagnostics.d3dFound),
        get(diagnostics.d3dMissing), get(diagnostics.d3dStateInvalid),
        get(diagnostics.deviceStateHooks), get(diagnostics.d3dFallback));
    log("DIAG perf passthroughDraws=%llu cappedLights=%llu cappedPasses=%llu RenderExtraLights=%d MaxRenderLightsPerMesh=%u MaxExtraPassesPerMesh=%u",
        get(diagnostics.passthroughDraws), get(diagnostics.limitedLights), get(diagnostics.limitedPasses),
        g.config.renderExtraLights, g.config.maxRenderLightsPerMesh, g.config.maxExtraPassesPerMesh);
    log("DIAG hotpath d3dScans=%llu d3dNegativeCacheSkips=%llu d3dCacheHits=%llu singleUberFastPath=%llu classOnAdd=%llu emptyRegistry=%llu slowValidation=%llu",
        get(diagnostics.d3dScans), get(diagnostics.d3dMissCached),
        get(diagnostics.deviceCacheHits), get(diagnostics.singleUberFastPaths),
        get(diagnostics.classifiedOnAdd), get(diagnostics.registryEmptyFastPaths),
        get(diagnostics.lightSlowValidation));
}
uint32_t __fastcall hookTime(void* driver, void*) {
    if (!g.origTime || !driver || !g.config.timerSizeFix) return g.origTime ? g.origTime(driver) : 0;
    // Engine's SystemDriver timebase double is at this + 80 bytes in examined variant.
    const auto* scalePtr = reinterpret_cast<const unsigned char*>(driver) + 80;
    if (!readable(scalePtr, sizeof(double))) return g.origTime(driver);
    double scale = 0.0;
    memcpy(&scale, scalePtr, sizeof scale);
    if (!std::isfinite(scale) || scale <= 0.) return g.origTime(driver);
    const double div = 0.001 / scale;
    if (!std::isfinite(div) || div < 0.5 || div > 2147483647.) return g.origTime(driver);
    const uint32_t divisor = static_cast<uint32_t>(div + 0.5);
    if (!divisor) return g.origTime(driver);
    const uint64_t ticks = __rdtsc();
    return static_cast<uint32_t>(ticks / divisor); // Engine API remains DWORD.
}

static bool addHook(void* address, void* callback, void** trampoline, const char* name,
                    std::vector<void*>& made) {
    if (!address) { log("missing export: %s", name); return false; }
    const MH_STATUS status = MH_CreateHook(address, callback, trampoline);
    if (status != MH_OK) { log("MH_CreateHook %s failed: %s", name, MH_StatusToString(status)); return false; }
    made.push_back(address);
    return true;
}

bool initialize(HMODULE self) {
    std::lock_guard<std::mutex> lk(initMutex);
    if (g.installed.load()) return true;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    if (!engine) return false;
    g.self = self;
    g.module = engine;
    initLog(self);
    log("Light v0.1.7 initializing");
    g.config = loadConfig(self);
    void* add = exported(engine, kAdd);
    void* reset = exported(engine, kReset);
    void* draw = exported(engine, kDraw);
    void* single = exported(engine, kSingleA);
    if (!single) single = exported(engine, kSingleB);
    void* uber = exported(engine, kUber);
    void* time = exported(engine, kTime);
    if (!add || !reset || !draw || !single) {
        log("Engine.dll found, but it lacks required PainEngine exports. Lighting disabled; no patches applied.");
        return false;
    }
    if (uber) g.variant = time ? EngineVariant::Black : EngineVariant::Overdose;
    else g.variant = EngineVariant::Proto2003;
    log("Engine.dll=%p variant=%s, MaxLights=%u, lighting=%d timer=%d",
        engine, g.variant == EngineVariant::Black ? "PK Black" :
        g.variant == EngineVariant::Overdose ? "Overdose family" : "Proto2003",
        g.config.maxLights, g.config.unlockLights, g.config.timerSizeFix);
    g.lights = std::make_unique<light::LightRegistry>(g.config.maxLights);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log("MinHook init failed: %s", MH_StatusToString(status));
        return false;
    }
    std::vector<void*> made;
    bool lightOK = true;
    if (g.config.unlockLights) {
        // All-or-nothing: never suppress vanilla passes without a working draw hook.
        lightOK = addHook(add, reinterpret_cast<void*>(&hookAdd), reinterpret_cast<void**>(&g.origAdd), "Entity::AddLight", made)
                && addHook(reset, reinterpret_cast<void*>(&hookReset), reinterpret_cast<void**>(&g.origReset), "Entity::ResetLights", made)
                && addHook(draw, reinterpret_cast<void*>(&hookDraw), reinterpret_cast<void**>(&g.origDraw), "WorldMesh::Draw", made)
                && addHook(single, reinterpret_cast<void*>(&hookLightPass), reinterpret_cast<void**>(&g.origLightPass), "WorldMesh::RenderLightPass", made);
        if (lightOK && uber)
            lightOK = addHook(uber, reinterpret_cast<void*>(&hookUber), reinterpret_cast<void**>(&g.origUberPass), "WorldMesh::RenderUberLightPass", made);
        if (!lightOK) {
            for (void* p : made) MH_RemoveHook(p);
            made.clear();
            g.origAdd = nullptr;
            g.origReset = nullptr;
            g.origDraw = nullptr;
            g.origLightPass = nullptr;
            g.origUberPass = nullptr;
            log("Lighting hooks not installed; original rendering preserved.");
        }
    }
    if (g.config.timerSizeFix && time && g.variant == EngineVariant::Black) {
        addHook(time, reinterpret_cast<void*>(&hookTime), reinterpret_cast<void**>(&g.origTime),
                "SystemDriver::GetCurrentTimeMS", made);
    }
    if (made.empty()) { log("Nothing to hook (disabled or unsupported build)."); return false; }
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        for (void* p : made) { MH_DisableHook(p); MH_RemoveHook(p); }
        g.origAdd = nullptr; g.origReset = nullptr; g.origDraw = nullptr;
        g.origLightPass = nullptr; g.origUberPass = nullptr; g.origTime = nullptr;
        log("Enabling hooks failed: %s; unchanged game", MH_StatusToString(status));
        return false;
    }
    g.lightingReady = lightOK && g.config.unlockLights;
    log("Diagnostics LogLights=%d PreserveNativePasses=%d SafeNativeFallback=%d",
        g.config.logLights, g.config.preserveNativePasses, g.config.safeNativeFallback);
    g.installed.store(true);
    log("Installed; dynamic lighting %s, timer %s", g.lightingReady ? "ACTIVE" : "OFF",
        g.origTime ? "ACTIVE" : "OFF");
    return true;
}
} // namespace pk
