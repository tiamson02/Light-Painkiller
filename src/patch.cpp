#include "patch.hpp"
#include "light_replay.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace pk {
EnginePatch g;
Stats stats;
thread_local const void* tlsSuppressedMesh = nullptr;
static std::mutex initMutex;

// CPU-only sampling. Sampling is disabled completely unless LogLights=1;
// no high-resolution clock calls occur in the normal release hot path.
class DrawCpuSample {
    using Clock = std::chrono::steady_clock;
    using Point = Clock::time_point;
    enum class Phase { Preparation, NativeDraw, Replay, Done };
    bool enabled_;
    bool replay_ = false;
    Phase phase_ = Phase::Preparation;
    Point last_{};
    unsigned long long prepareNs_ = 0, nativeNs_ = 0, replayNs_ = 0;

    void transition(Phase next) noexcept {
        if (!enabled_) return;
        const Point now = Clock::now();
        const auto ns = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now-last_).count());
        switch (phase_) {
            case Phase::Preparation: prepareNs_ += ns; break;
            case Phase::NativeDraw: nativeNs_ += ns; break;
            case Phase::Replay: replayNs_ += ns; break;
            case Phase::Done: break;
        }
        last_ = now;
        phase_ = next;
    }
public:
    explicit DrawCpuSample(bool enabled) : enabled_(enabled) {
        if (enabled_) last_ = Clock::now();
    }
    DrawCpuSample(const DrawCpuSample&) = delete;
    DrawCpuSample& operator=(const DrawCpuSample&) = delete;
    void nativeDraw() noexcept { transition(Phase::NativeDraw); }
    void replay() noexcept { replay_ = true; transition(Phase::Replay); }
    ~DrawCpuSample() {
        if (!enabled_) return;
        transition(Phase::Done);
        if (replay_) {
            stats.profReplaySamples.fetch_add(1, std::memory_order_relaxed);
            stats.profReplayPrepareNs.fetch_add(prepareNs_, std::memory_order_relaxed);
            stats.profReplayNativeNs.fetch_add(nativeNs_, std::memory_order_relaxed);
            stats.profReplayPassNs.fetch_add(replayNs_, std::memory_order_relaxed);
        } else {
            stats.profStockSamples.fetch_add(1, std::memory_order_relaxed);
            stats.profStockNativeNs.fetch_add(nativeNs_, std::memory_order_relaxed);
        }
    }
};
static void* gEngineGlobal = nullptr;

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
    // Sample diagnostics in debug mode; do not query system time on every AddLight.
    if (g.config.logLights) {
        static thread_local unsigned calls = 0;
        if ((++calls & 1023u) == 0) maybeLogDiagnostics();
    }
}
void __fastcall hookReset(void* entity, void*) {
    bump(stats.reset);
    if (g.lightingReady && g.lights) g.lights->reset(entity);
    if (g.origReset) g.origReset(entity);
}

static unsigned readNativeSlots(void* mesh, const void** out) {
    if (!mesh || g.variant == EngineVariant::Unknown) return 0;
    // erychpatch sub_10004FD0: native count is 0..4, not necessarily 4.
    const size_t slotsOffset = g.variant == EngineVariant::Proto2003 ? 1316 : 1320;
    const size_t countOffset = g.variant == EngineVariant::Proto2003 ? 1540 : 1544;
    const unsigned char* p = static_cast<const unsigned char*>(mesh);
    // Both fields share one committed WorldMesh allocation: one region probe.
    if (!readable(p + slotsOffset, countOffset + sizeof(uint32_t) - slotsOffset)) return 0;
    uint32_t count = 0;
    memcpy(&count, p + countOffset, sizeof count);
    if (count > 4) return 0;
    memcpy(out, p + slotsOffset, sizeof(void*) * count);
    return count;
}

static bool supportsUberMode(void* renderDevice) {
    if (!renderDevice || !g.origUberPass || g.variant==EngineVariant::Proto2003)
        return false;
    // Renderer settings change infrequently. Avoid several VirtualQuery calls
    // for every lit mesh: refresh this thread-local cache once per second.
    struct Cache { void* rd=nullptr; DWORD at=0; bool value=false; bool initialized=false; };
    static thread_local Cache cached;
    const DWORD now=GetTickCount();
    if (cached.initialized && cached.rd==renderDevice &&
        static_cast<DWORD>(now-cached.at)<1000) return cached.value;
    cached={}; cached.rd=renderDevice; cached.at=now; cached.initialized=true;
    const unsigned char* p=static_cast<const unsigned char*>(renderDevice)+4;
    int hardware=0;
    if (!readable(p,sizeof hardware)) return false;
    memcpy(&hardware,p,sizeof hardware);
    if (hardware<5 || !gEngineGlobal || !readable(gEngineGlobal,sizeof(void*))) return false;
    void* system=nullptr;
    memcpy(&system,gEngineGlobal,sizeof system);
    if (!system) return false;
    const size_t offset=g.variant==EngineVariant::Black ? 232 : 100;
    if (!readable(static_cast<unsigned char*>(system)+offset,sizeof(void*))) return false;
    void* rendererCfg=nullptr;
    memcpy(&rendererCfg,static_cast<unsigned char*>(system)+offset,sizeof rendererCfg);
    if (!rendererCfg || !readable(static_cast<unsigned char*>(rendererCfg)+6396,1)) return false;
    unsigned char medium=0;
    memcpy(&medium,static_cast<unsigned char*>(rendererCfg)+6396,1);
    cached.value=medium!=0;
    return cached.value;
}

// Reverse-engineered from erychpatch's sub_10004680: the underlying D3D9
// interface is stored in one of the RenderDevice pointer slots +0x80..0xC0.
// The method at vtable +228 is IDirect3DDevice9::SetRenderState.
// Do NOT change the device vtable; use its COM methods only.
struct D3DStateDevice {
    void* ptr = nullptr;
    using GetStateFn = long (__stdcall *)(void*, unsigned long, unsigned long*);
    using SetStateFn = long (__stdcall *)(void*, unsigned long, unsigned long);
    GetStateFn get = nullptr;
    SetStateFn set = nullptr;
};

static D3DStateDevice locateD3D9(void* rd) {
    if (!rd) return {};
    // Cache is per thread and render device, never shared across threads.
    struct Cache { void* rd = nullptr; D3DStateDevice dev{}; DWORD lastProbe = 0; };
    static thread_local Cache cached;
    const DWORD now = GetTickCount();
    if (cached.rd == rd &&
        (cached.dev.ptr || static_cast<DWORD>(now - cached.lastProbe) < 1000))
        return cached.dev;
    cached = {};
    cached.rd = rd;
    cached.lastProbe = now;
    HMODULE engine = GetModuleHandleA("Engine.dll");
    for (unsigned offset = 128; offset <= 192; offset += 4) {
        auto* address = static_cast<unsigned char*>(rd) + offset;
        if (!readable(address, sizeof(void*))) continue;
        void* candidate = nullptr;
        memcpy(&candidate, address, sizeof candidate);
        if (!candidate || !readable(candidate, sizeof(void*))) continue;
        void** vt = nullptr;
        memcpy(&vt, candidate, sizeof vt);
        if (!vt || !readable(vt + 58, sizeof(void*) * 2)) continue;
        void* method = vt[57];
        if (!method) continue;
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(method), &module) || !module || module == engine) continue;
        cached.dev = {candidate,
            reinterpret_cast<D3DStateDevice::GetStateFn>(vt[58]),
            reinterpret_cast<D3DStateDevice::SetStateFn>(method)};
        break;
    }
    return cached.dev;
}

// Original erychpatch's additive pass uses render states 27,19,20,14.
// We save and restore their *actual* device values without a D3D9 vtable hook.
struct AdditiveState {
    D3DStateDevice dev;
    unsigned long old[4]{};
    bool ready = false;
    static constexpr unsigned long keys[4] = {27u, 19u, 20u, 14u};
    static constexpr unsigned long values[4] = {1u, 2u, 2u, 0u};
    explicit AdditiveState(D3DStateDevice d) : dev(d) {
        if (!dev.ptr || !dev.get || !dev.set) return;
        for (unsigned i=0;i<4;++i)
            if (dev.get(dev.ptr, keys[i], &old[i]) < 0) return;
        unsigned applied=0;
        for (;applied<4;++applied) {
            if (dev.set(dev.ptr, keys[applied], values[applied]) < 0) break;
        }
        if (applied!=4) {
            while (applied>0) { --applied; dev.set(dev.ptr, keys[applied], old[applied]); }
            return;
        }
        ready=true;
    }
    ~AdditiveState() {
        if (ready) for(unsigned i=0;i<4;++i) dev.set(dev.ptr, keys[i], old[i]);
    }
};

// Mirrors the proven erychpatch sequencing (sub_10002EB0):
//     suppress built-in light calls *during* WorldMesh::Draw,
//     then replay the engine's per-mesh light list after Draw.
// The v0.2.0-0.2.4 approach of waiting until exactly four native calls
// were observed never activated on some valid PainEditor rendering paths.
static void replayLightList(void* mesh, void* renderDevice,
                            const light::ReplayPlan& plan, bool uberMode) {
    for (std::size_t i=0;i<plan.singleCount;++i) {
        if (g.origLightPass) g.origLightPass(mesh, plan.singles[i]);
    }
    if (plan.singleCount) bump(stats.replaySingles, static_cast<unsigned long long>(plan.singleCount));
    if (!uberMode || !g.origUberPass) return;

    // Erychpatch batches type-2 sources into groups of three on PK Black.
    const unsigned width = g.variant == EngineVariant::Overdose ? 4 : 3;
    const auto device = plan.uberCount>width ? locateD3D9(renderDevice) : D3DStateDevice{};
    // Validated by hookDraw before suppression. No dangerous type-2 to
    // RenderLightPass fallback is permitted here.
    for (std::size_t i=0;i<plan.uberCount;i+=width) {
        const auto n=static_cast<int>((std::min)(std::size_t(width),plan.uberCount-i));
        if (i) {
            // The first Uber batch renders normally. Subsequent batches
            // accumulate additively, just as in sub_100045A0.
            AdditiveState state(device);
            if (!state.ready) { bump(stats.skippedNoDevice); break; }
            g.origUberPass(mesh, plan.uber.data()+i, n);
        } else {
            g.origUberPass(mesh, plan.uber.data()+i, n);
        }
        bump(stats.replayUberBatches);
        bump(stats.replayUberLights, static_cast<unsigned long long>(n));
    }
}

void __fastcall hookLightPass(void* mesh, void*, const void* light) {
    bump(stats.nativeSingle);
    if (tlsSuppressedMesh != mesh && g.origLightPass) g.origLightPass(mesh, light);
}
void __fastcall hookUber(void* mesh, void*, const void* const* lights, int count) {
    bump(stats.nativeUber);
    if (tlsSuppressedMesh != mesh && g.origUberPass) g.origUberPass(mesh, lights, count);
}

void __fastcall hookDraw(void* mesh, void*, void* renderDevice, int flags) {
    if (!g.origDraw) return;
    bump(stats.draw);
    // Sample 1/128 calls only while diagnostics are explicitly enabled. The
    // profiler observes CPU submission time, never GPU completion time.
    static thread_local unsigned sampleOrdinal = 0;
    const bool sampling = g.config.logLights && ((++sampleOrdinal & 127u) == 0);
    DrawCpuSample cpu(sampling);
    if (g.config.logLights) maybeLogDiagnostics();
    if (!g.lightingReady || !g.lights || !g.lights->hasAny() ||
        g.config.extraPassesPerMesh == 0 ||
        (static_cast<uint32_t>(flags) & 0x40000000u) == 0 ||
        tlsSuppressedMesh == mesh) {
        cpu.nativeDraw();
        g.origDraw(mesh, renderDevice, flags);
        return;
    }
    // Most world meshes have 0..4 registered sources. Avoid copying their
    // records into a large temporary buffer and planning a replay.
    std::array<light::LightRegistry::LightRecord, 512> queued;
    bool tooManyForBuffer = false;
    const size_t count = g.lights->snapshotSafeOverflowInto(
        mesh, queued.data(), queued.size(), tooManyForBuffer);
    if (!count) {
        bump(tooManyForBuffer ? stats.incomplete : stats.noOverflow);
        cpu.nativeDraw();
        g.origDraw(mesh,renderDevice,flags);
        return;
    }
    bump(stats.overflowEligible);
    if (g.config.logLights) {
        if (count > 8) bump(stats.profOver8);
        if (count > 16) bump(stats.profOver16);
        if (count > 32) bump(stats.profOver32);
        auto old = stats.profMaxLightsOnMesh.load(std::memory_order_relaxed);
        while (count > old && !stats.profMaxLightsOnMesh.compare_exchange_weak(
                   old, count, std::memory_order_relaxed)) {}
    }
    const void* native[4]{};
    const unsigned nativeCount=readNativeSlots(mesh,native);
    const bool uberMode=supportsUberMode(renderDevice);
    const unsigned uberWidth = g.variant == EngineVariant::Overdose ? 4u : 3u;
    const auto plan=light::makeReplayPlan(queued.data(),count,native,nativeCount,
                                          g.config.extraPassesPerMesh,uberMode,uberWidth);
    if (!plan.valid) {
        bump(stats.incomplete);
        cpu.nativeDraw();
        g.origDraw(mesh,renderDevice,flags);
        return;
    }
    // A second Uber batch REQUIRES additive device state. If the interface
    // cannot be safely located, keep all native projector passes intact.
    // Never reinterpret projective lights as ordinary single-light passes.
    if (uberMode && plan.uberCount > uberWidth &&
        !locateD3D9(renderDevice).ptr) {
        bump(stats.skippedNoDevice);
        cpu.nativeDraw();
        g.origDraw(mesh,renderDevice,flags);
        return;
    }
    // Use the v0.2.5 replay sequencing; enlarge source capacity only.
    const void* previous=tlsSuppressedMesh;
    tlsSuppressedMesh=mesh;
    struct Restore { const void* prev; ~Restore() {tlsSuppressedMesh=prev;} } restore{previous};
    cpu.nativeDraw();
    g.origDraw(mesh,renderDevice,flags);
    tlsSuppressedMesh=previous;
    cpu.replay();
    replayLightList(mesh,renderDevice,plan,uberMode);
    bump(stats.replayedMeshes);
    bump(stats.replayedNative, static_cast<unsigned long long>(plan.nativeCount));
    bump(stats.replayedOverflow, static_cast<unsigned long long>(plan.extraCount));
    if (plan.skipped) bump(stats.skippedUnsupported,static_cast<unsigned long long>(plan.skipped));
}

void maybeLogDiagnostics() {
    if (!g.config.logLights) return;
    static std::atomic<DWORD> next{0};
    DWORD now = GetTickCount(), old = next.load(std::memory_order_relaxed);
    if (static_cast<int32_t>(now - old) < 0 ||
        !next.compare_exchange_strong(old, now + 5000, std::memory_order_relaxed)) return;
    const auto read = [](const std::atomic<unsigned long long>& a) {return a.load();};
    log("DIAG add=%llu reset=%llu draw=%llu nativeSingle=%llu nativeUber=%llu replaySingles=%llu replayUberLights=%llu replayUberBatches=%llu unsafeSkipped=%llu skippedUnsupported=%llu skippedNoDevice=%llu incompleteNative=%llu stockOnly=%llu overflowEligible=%llu lightCacheHit=%llu lightCacheMiss=%llu timerCalls=%llu trackedEntities=%zu",
        read(stats.add), read(stats.reset), read(stats.draw), read(stats.nativeSingle),
        read(stats.nativeUber), read(stats.replaySingles), read(stats.replayUberLights),
        read(stats.replayUberBatches), read(stats.unsafe),
        read(stats.skippedUnsupported), read(stats.skippedNoDevice),
        read(stats.incomplete), read(stats.noOverflow), read(stats.overflowEligible),
        read(stats.lightCacheHits), read(stats.lightCacheMisses),
        read(stats.timerCalls), g.lights ? g.lights->trackedEntities() : 0);
    log("DIAG replayedMeshes=%llu replayedNative=%llu replayedOverflow=%llu; original light passes suppressed only for reconstructed overflow meshes",
        read(stats.replayedMeshes),read(stats.replayedNative),read(stats.replayedOverflow));
    const auto stockSamples = read(stats.profStockSamples);
    const auto replaySamples = read(stats.profReplaySamples);
    // Cumulative, sampled averages. Long GPU stalls and async work are NOT
    // represented; compare these numbers with an A/B no-hook FPS baseline.
    log("DIAG CPU sampled stockDraw=%llu avgNativeUs=%llu replayDraw=%llu avgPrepUs=%llu avgNativeUs=%llu avgReplayUs=%llu (CPU only, 1/128 draws)",
        stockSamples,
        stockSamples ? read(stats.profStockNativeNs)/stockSamples/1000 : 0,
        replaySamples,
        replaySamples ? read(stats.profReplayPrepareNs)/replaySamples/1000 : 0,
        replaySamples ? read(stats.profReplayNativeNs)/replaySamples/1000 : 0,
        replaySamples ? read(stats.profReplayPassNs)/replaySamples/1000 : 0);
    log("DIAG LIGHT overlap>8=%llu overlap>16=%llu overlap>32=%llu maxSourcesOnMesh=%llu (cumulative draw observations)",
        read(stats.profOver8),read(stats.profOver16),read(stats.profOver32),
        read(stats.profMaxLightsOnMesh));
}

uint32_t __fastcall hookTime(void* driver, void*) {
    if (!g.origTime || !driver) return g.origTime ? g.origTime(driver) : 0;
    bump(stats.timerCalls);
    if (g.config.logLights) {
        static thread_local unsigned calls = 0;
        if ((++calls & 4095u) == 0) maybeLogDiagnostics();
    }
    struct TimerCache {
        const void* driver = nullptr;
        uint32_t divisor = 0;
        unsigned callsUntilRefresh = 0;
    };
    // The SystemDriver scale normally remains constant. Validate it on the
    // first call and periodically, without VirtualQuery on every timer read.
    static thread_local TimerCache cache;
    if (cache.driver != driver || cache.callsUntilRefresh == 0) {
        cache.driver = driver;
        cache.callsUntilRefresh = 256;
        cache.divisor = 0;
        const unsigned char* p = static_cast<const unsigned char*>(driver) + 80;
        if (readable(p, sizeof(double))) {
            double scale{};
            memcpy(&scale, p, sizeof scale);
            if (std::isfinite(scale) && scale > 0.0) {
                const double div = 0.001 / scale;
                if (std::isfinite(div) && div >= 0.5 && div <= 2147483647.0)
                    cache.divisor = static_cast<uint32_t>(div + 0.5);
            }
        }
    }
    --cache.callsUntilRefresh;
    if (!cache.divisor) return g.origTime(driver);
    return static_cast<uint32_t>(__rdtsc() / cache.divisor);
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
    gEngineGlobal = exported(engine, "?GEngine@@3PAVPCFSystem@@A");
    g.self = self;
    initLog(self);
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(self, modulePath, MAX_PATH);
    log("Light v0.2.9 initializing (EXE-synchronized addon handshake; v0.2.7 renderer); PID=%lu; module=%s",
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
    else log("Light budget: registered up to %u per mesh, extra-pass budget %u (ordinary lights: up to 4+budget; projected lights batched).",g.config.maxLights,g.config.extraPassesPerMesh);
    return true;
}
}
