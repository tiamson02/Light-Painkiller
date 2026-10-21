#include "patch.hpp"
#include <MinHook.h>
#include <array>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace pk {
EnginePatch g;
Stats stats;
thread_local DrawContext* tlsDraw = nullptr;
static std::mutex initMutex;

static const char* kAdd     = "?AddLight@Entity@@QAEXPAVLight@@@Z";
static const char* kReset   = "?ResetLights@Entity@@QAEXXZ";
static const char* kDraw    = "?Draw@WorldMesh@@UAEXPAVRenderDevice@@H@Z";
static const char* kSingleA = "?RenderLightPass@WorldMesh@@AAEXPBVLight@@@Z";
static const char* kSingleB = "?RenderLightPass@WorldMesh@@AAEXPAVLight@@@Z";
static const char* kUber    = "?RenderUberLightPass@WorldMesh@@AAEXQAPBVLight@@H@Z";
static const char* kTime    = "?GetCurrentTimeMS@SystemDriver@@QBEKXZ";

void __fastcall hookAdd(void* entity, void*, void* light) {
    // Engine always registers its first four lights as before.
    if (g.origAdd) g.origAdd(entity, light);
    bump(stats.add);
    if (!g.lightingReady || !g.lights || !entity || !light) return;
    light::LightRegistry::LightRecord r{};
    classifyLight(light, r);
    g.lights->addClassified(entity, light, r.flags, r.special, r.type, r.valid);
}
void __fastcall hookReset(void* entity, void*) {
    bump(stats.reset);
    if (g.lightingReady && g.lights) g.lights->reset(entity);
    if (g.origReset) g.origReset(entity);
}

static bool readNativeSlots(void* mesh, const void** out) {
    if (!mesh || g.variant == EngineVariant::Unknown) return false;
    // Confirmed by sub_10004FD0 in the supplied erychpatch decompilation.
    const size_t slotsOffset = g.variant == EngineVariant::Proto2003 ? 1316 : 1320;
    const size_t countOffset = g.variant == EngineVariant::Proto2003 ? 1540 : 1544;
    const unsigned char* p = static_cast<const unsigned char*>(mesh);
    if (!readable(p + countOffset, sizeof(uint32_t)) ||
        !readable(p + slotsOffset, sizeof(void*) * 4)) return false;
    uint32_t count = 0;
    memcpy(&count, p + countOffset, sizeof count);
    if (count != 4) return false;
    memcpy(out, p + slotsOffset, sizeof(void*) * 4);
    return true;
}

static bool safeNative(const light::LightRegistry::LightRecord* queue, size_t count,
                       const void* const* native) {
    // Only extend when ALL original lights and extra lights are ordinary,
    // known, non-projected light types. If a torch or flashlight participates,
    // defer wholly to the original renderer (works in vanilla and mods).
    if (!queue || !native || count <= 4) return false;
    for (size_t i=0; i<count; ++i)
        if (!light::LightRegistry::safePointLight(queue[i])) return false;
    for (unsigned i=0; i<4; ++i)
        if (!light::LightRegistry::containsNativeRecord(queue, count, native[i])) return false;
    return true;
}

static void tryExtraWithinDraw(DrawContext* ctx) {
    if (!ctx || ctx->dispatched || !g.origLightPass || !g.lightingReady ||
        g.config.extraPassesPerMesh == 0 || ctx->seenCount < 4) return;

    const void* native[4] = {};
    if (!readNativeSlots(const_cast<void*>(ctx->mesh), native) ||
        !light::LightRegistry::allNativeObserved(native, 4, ctx->seen, ctx->seenCount))
        return; // Native pass sequence unfinished or unsupported.

    // The original light sequence is complete. The engine still owns the
    // active material, projection state and shader while we are in Draw.
    ctx->dispatched = true;
    if (!safeNative(ctx->queued, ctx->queuedCount, native)) {
        bump(stats.unsafe);
        return;
    }

    std::array<light::LightRegistry::LightRecord, 512> extras{};
    const size_t count = light::LightRegistry::selectNotNative(
        ctx->queued, ctx->queuedCount, native, 4, extras.data(), extras.size());
    const size_t budget = (count < g.config.extraPassesPerMesh)
                            ? count : g.config.extraPassesPerMesh;
    for (size_t i=0; i<budget; ++i) {
        // Call trampoline directly, not the hook, to avoid nested tracking.
        g.origLightPass(const_cast<void*>(ctx->mesh), extras[i].light);
        bump(stats.extra);
    }
}

void __fastcall hookLightPass(void* mesh, void*, const void* light) {
    bump(stats.nativeSingle);
    DrawContext* ctx = tlsDraw;
    if (ctx && ctx->mesh == mesh) ctx->note(light);
    if (g.origLightPass) g.origLightPass(mesh, light);
    if (ctx && ctx->mesh == mesh) tryExtraWithinDraw(ctx);
}
void __fastcall hookUber(void* mesh, void*, const void* const* lights, int count) {
    bump(stats.nativeUber);
    DrawContext* ctx = tlsDraw;
    if (ctx && ctx->mesh == mesh && lights && count > 0 && count <= 4)
        for (int i=0; i<count; ++i) ctx->note(lights[i]);
    if (g.origUberPass) g.origUberPass(mesh, lights, count);
    if (ctx && ctx->mesh == mesh) tryExtraWithinDraw(ctx);
}

void __fastcall hookDraw(void* mesh, void*, void* renderDevice, int flags) {
    if (!g.origDraw) return;
    bump(stats.draw);
    if (!g.lightingReady || !g.lights || !g.lights->hasAny() ||
        g.config.extraPassesPerMesh == 0 ||
        (static_cast<uint32_t>(flags) & 0x40000000u) == 0) {
        g.origDraw(mesh, renderDevice, flags);
        return;
    }
    std::array<light::LightRegistry::LightRecord, 512> queued{};
    const size_t count = g.lights->snapshotRecordsInto(mesh, queued.data(), queued.size());
    if (count <= 4) {
        bump(stats.noOverflow);
        g.origDraw(mesh, renderDevice, flags);
        return;
    }
    // Cheap safety check before calling Draw; the full native set is only
    // verified inside the engine's own light-pass callbacks.
    for (size_t i=0; i<count; ++i) {
        if (!light::LightRegistry::safePointLight(queued[i])) {
            bump(stats.unsafe);
            g.origDraw(mesh, renderDevice, flags);
            return;
        }
    }
    DrawContext ctx{};
    ctx.mesh = mesh;
    ctx.queued = queued.data();
    ctx.queuedCount = count;
    ctx.previous = tlsDraw;
    tlsDraw = &ctx;
    struct RestoreScope {
        DrawContext* previous;
        ~RestoreScope() { tlsDraw = previous; }
    } restore{ctx.previous};
    g.origDraw(mesh, renderDevice, flags);
    if (!ctx.dispatched) bump(stats.incomplete);
    if (g.config.logLights) maybeLogDiagnostics();
    // IMPORTANT: absolutely no additional rendering after origDraw returns.
}

void maybeLogDiagnostics() {
    if (!g.config.logLights) return;
    static std::atomic<DWORD> next{0};
    DWORD now = GetTickCount(), old = next.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(now - old) < 0 ||
        !next.compare_exchange_strong(old, now + 5000, std::memory_order_relaxed)) return;
    const auto read = [](const std::atomic<unsigned long long>& a) {return a.load();};
    log("DIAG add=%llu reset=%llu draw=%llu nativeSingle=%llu nativeUber=%llu extraInDraw=%llu unsafeSkipped=%llu incompleteNative=%llu stockOnly=%llu",
        read(stats.add), read(stats.reset), read(stats.draw), read(stats.nativeSingle),
        read(stats.nativeUber), read(stats.extra), read(stats.unsafe),
        read(stats.incomplete), read(stats.noOverflow));
}

uint32_t __fastcall hookTime(void* driver, void*) {
    if (!g.origTime || !driver) return g.origTime ? g.origTime(driver) : 0;
    const unsigned char* p = static_cast<const unsigned char*>(driver) + 80;
    if (!readable(p, sizeof(double))) return g.origTime(driver);
    double scale{};
    memcpy(&scale, p, sizeof scale);
    if (!std::isfinite(scale) || scale <= 0.0) return g.origTime(driver);
    const double div = 0.001 / scale;
    if (!std::isfinite(div) || div < 0.5 || div > 2147483647.0) return g.origTime(driver);
    const uint32_t divisor = static_cast<uint32_t>(div + 0.5);
    if (!divisor) return g.origTime(driver);
    return static_cast<uint32_t>(__rdtsc() / divisor);
}

static bool addHook(void* address, void* callback, void** trampoline,
                    const char* name, std::vector<void*>& made) {
    if (!address) {log("Missing export: %s", name); return false;}
    const MH_STATUS status = MH_CreateHook(address, callback, trampoline);
    if (status != MH_OK) {log("MH_CreateHook %s failed: %s", name, MH_StatusToString(status)); return false;}
    made.push_back(address);
    return true;
}

bool initialize(HMODULE self) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (g.installed.load()) return true;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    if (!engine) return false;
    g.self = self;
    initLog(self);
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(self, modulePath, MAX_PATH);
    log("Light v0.2.1 initializing; PID=%lu; module=%s",
        (unsigned long)GetCurrentProcessId(), modulePath);
    g.config = loadConfig(self);
    log("Engine.dll base=%p", static_cast<void*>(engine));
    void* add = exported(engine, kAdd);
    void* reset = exported(engine, kReset);
    void* draw = exported(engine, kDraw);
    void* single = exported(engine, kSingleA);
    if (!single) single = exported(engine, kSingleB);
    void* uber = exported(engine, kUber);
    void* time = exported(engine, kTime);
    if (!add || !reset || !draw || !single) {
        log("Unsupported Engine.dll exports, no patches applied.");
        return false;
    }
    if (uber) g.variant = time ? EngineVariant::Black : EngineVariant::Overdose;
    else g.variant = EngineVariant::Proto2003;
    // If both independent patch families are disabled, leave Engine.dll code
    // untouched and do not even initialize the hook framework.
    const bool wantLighting = g.config.unlockLights && g.config.extraPassesPerMesh > 0;
    const bool wantTimer = g.config.timerSizeFix && time && g.variant == EngineVariant::Black;
    if (!wantLighting && !wantTimer) {
        log("NO HOOKS INSTALLED: lighting=0 timer=0; untouched native engine path");
        g.installed.store(true);
        return true;
    }
    if (wantLighting) g.lights = std::make_unique<light::LightRegistry>(g.config.maxLights);
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        log("MinHook init failed: %s", MH_StatusToString(init));
        return false;
    }
    std::vector<void*> made;
    bool lightOK = true;
    // A zero extra-pass budget is a true no-op for the entire lighting path.
    // This gives a decisive baseline, with zero AddLight/Draw/pass hooks.
    if (wantLighting) {
        lightOK = addHook(add, reinterpret_cast<void*>(&hookAdd), reinterpret_cast<void**>(&g.origAdd), "AddLight", made)
               && addHook(reset, reinterpret_cast<void*>(&hookReset), reinterpret_cast<void**>(&g.origReset), "ResetLights", made)
               && addHook(draw, reinterpret_cast<void*>(&hookDraw), reinterpret_cast<void**>(&g.origDraw), "WorldMesh::Draw", made)
               && addHook(single, reinterpret_cast<void*>(&hookLightPass), reinterpret_cast<void**>(&g.origLightPass), "RenderLightPass", made);
        if (lightOK && uber) lightOK = addHook(uber, reinterpret_cast<void*>(&hookUber),
            reinterpret_cast<void**>(&g.origUberPass), "RenderUberLightPass", made);
        if (!lightOK) {
            for (void* ptr : made) MH_RemoveHook(ptr);
            made.clear();
            g.origAdd = nullptr; g.origReset = nullptr; g.origDraw = nullptr;
            g.origLightPass = nullptr; g.origUberPass = nullptr;
            log("Lighting hooks rolled back; stock renderer preserved.");
        }
    }
    if (wantTimer)
        addHook(time, reinterpret_cast<void*>(&hookTime),
                reinterpret_cast<void**>(&g.origTime), "GetCurrentTimeMS", made);
    if (made.empty()) {
        log("NO HOOKS INSTALLED. lighting_requested=%d extra_budget=%u timer_requested=%d; original engine rendering is untouched.",
            g.config.unlockLights, g.config.extraPassesPerMesh, g.config.timerSizeFix);
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(MH_ALL_HOOKS);
    if (enabled != MH_OK) {
        for (void* ptr : made) {MH_DisableHook(ptr); MH_RemoveHook(ptr);}
        log("Hook activation failed: %s", MH_StatusToString(enabled));
        return false;
    }
    g.lightingReady.store(lightOK && wantLighting);
    g.installed.store(true);
    log("Installed variant=%s MaxLights=%u ExtraPassesPerMesh=%u lighting=%d timer=%d",
        g.variant == EngineVariant::Black ? "PK Black" :
        g.variant == EngineVariant::Overdose ? "Overdose" : "Proto2003",
        g.config.maxLights, g.config.extraPassesPerMesh,
        g.lightingReady.load(), g.origTime != nullptr);
    if (!g.lightingReady.load()) log("Lighting hooks INACTIVE; native lighting forwarded without modification.");
    return true;
}
}
