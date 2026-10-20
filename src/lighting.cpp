#include "patch.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Deliberately not including d3d9.h: only the classic IDirect3DDevice9 COM ABI
// (vtbl[57]/vtbl[58]) is used. DX11 renderers are not covered.
namespace pk {
namespace {
using SetState = HRESULT (WINAPI*)(void*, DWORD, DWORD);
using GetState = HRESULT (WINAPI*)(void*, DWORD, DWORD*);
constexpr DWORD kAlphaBlend = 27, kSrcBlend = 19, kDestBlend = 20, kZWrite = 14;
constexpr DWORD kOne = 2;
static SetState origSetState = nullptr;
static GetState origGetState = nullptr;
static void* hookedVtable = nullptr;
static std::mutex stateMutex;
thread_local bool tlsAdditive = false;

HRESULT WINAPI hookedSetRenderState(void* dev, DWORD state, DWORD value) {
    auto orig = origSetState;
    if (!orig) return E_FAIL;
    // While drawing an extra light pass, the engine must not turn additive
    // blending off or re-enable depth writes during its own shader setup.
    if (tlsAdditive && ((state == kAlphaBlend && value == 0) ||
                        (state == kZWrite && value == 1))) return S_OK;
    return orig(dev, state, value);
}

bool moduleIsD3D9(void* function) {
    if (!function) return false;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(function), &m)) return false;
    char filename[MAX_PATH] = {};
    GetModuleFileNameA(m, filename, MAX_PATH);
    const char* last = strrchr(filename, '\\');
    const char* base = last ? last+1 : filename;
    return _stricmp(base, "d3d9.dll") == 0;
}

struct Device {
    void* ptr = nullptr;
    SetState set = nullptr;
    GetState get = nullptr;
    explicit operator bool() const { return ptr && set && get; }
};
// Detect the backend once per RenderDevice instance; use a single pointer check
// afterward. A failed initial scan is retried infrequently for late D3D startup.
Device findDevice(void* renderDevice) {
    if (!renderDevice) return {};
    struct Cache {
        void* owner = nullptr;
        Device dev{};
        unsigned offset = 0;
        unsigned misses = 0;
    };
    thread_local Cache cache{};
    if (cache.owner == renderDevice) {
        if (cache.dev) {
            // RenderDevice is a live engine-owned object for the duration of Draw.
            // Its device pointer may change during a device reset.
            void* current = nullptr;
            memcpy(&current, static_cast<const unsigned char*>(renderDevice) + cache.offset,
                   sizeof current);
            if (current == cache.dev.ptr) {
                diagAdd(diagnostics.deviceCacheHits, 1, std::memory_order_relaxed);
                return cache.dev;
            }
            cache.dev = {};
        } else if (++cache.misses < 16384) {
            diagAdd(diagnostics.d3dMissCached, 1, std::memory_order_relaxed);
            return {};
        }
    }
    cache.owner = renderDevice;
    cache.dev = {};
    cache.misses = 0;
    diagAdd(diagnostics.d3dScans, 1, std::memory_order_relaxed);
    for (unsigned off = 128; off <= 192; off += 4) {
        const auto* p = static_cast<const unsigned char*>(renderDevice) + off;
        if (!readable(p, sizeof(void*))) continue;
        void* device = nullptr;
        memcpy(&device, p, sizeof device);
        if (!device || !readable(device, sizeof(void*))) continue;
        void** vt = nullptr;
        memcpy(&vt, device, sizeof vt);
        if (!vt || !readable(vt + 58, sizeof(void*))) continue;
        if (!moduleIsD3D9(vt[57]) &&
            vt[57] != reinterpret_cast<void*>(&hookedSetRenderState)) continue;
        cache.offset = off;
        cache.dev = {device, reinterpret_cast<SetState>(vt[57]),
                           reinterpret_cast<GetState>(vt[58])};
        return cache.dev;
    }
    return {};
}

// Used only if explicitly enabled. Patches the shared D3D9 vtable entry,
// matching the way the supplied module intercepts SetRenderState.
// Must remain installed until the process exits.
void ensureStateHook(Device& device) {
    if (!device || !g.config.hookD3DState) return;
    std::lock_guard<std::mutex> guard(stateMutex);
    auto* vt = *reinterpret_cast<void***>(device.ptr);
    if (vt[57] == reinterpret_cast<void*>(&hookedSetRenderState)) {
        device.set = &hookedSetRenderState;
        device.get = reinterpret_cast<GetState>(vt[58]);
        return;
    }
    if (hookedVtable) return; // Never hook multiple unrelated D3D device types.
    DWORD oldProt = 0;
    if (!VirtualProtect(vt + 57, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) return;
    origSetState = reinterpret_cast<SetState>(vt[57]);
    origGetState = reinterpret_cast<GetState>(vt[58]);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(vt+57),
                               reinterpret_cast<void*>(&hookedSetRenderState));
    DWORD ignored;
    VirtualProtect(vt+57, sizeof(void*), oldProt, &ignored);
    hookedVtable = vt;
    diagAdd(diagnostics.deviceStateHooks, 1, std::memory_order_relaxed);
    device.set = &hookedSetRenderState;
    device.get = origGetState;
    log("D3D9 SetRenderState hook installed on vtable=%p", vt);
}

struct RenderStates {
    Device dev;
    std::array<DWORD, 4> before{};
    bool valid = false;
    explicit RenderStates(Device d): dev(d) {
        if (!dev) return;
        const DWORD keys[] = {kAlphaBlend,kSrcBlend,kDestBlend,kZWrite};
        valid = true;
        for (size_t i=0;i<4;i++) if (FAILED(dev.get(dev.ptr,keys[i],&before[i]))) valid = false;
    }
    void additive() {
        if (!valid) return;
        dev.set(dev.ptr,kAlphaBlend,1);
        dev.set(dev.ptr,kSrcBlend,kOne);
        dev.set(dev.ptr,kDestBlend,kOne);
        dev.set(dev.ptr,kZWrite,0);
    }
    ~RenderStates() {
        tlsAdditive = false;
        if (!valid) return;
        const DWORD keys[] = {kAlphaBlend,kSrcBlend,kDestBlend,kZWrite};
        for (size_t i=0;i<4;i++) dev.set(dev.ptr,keys[i],before[i]);
    }
};

// Use one committed-region check for fields at +0x18, +0x7E0 and +0x7F4.
// Fall back to individual range validation for unusual allocation boundaries.
struct LightFields {
    uint32_t flags = 0;
    uint8_t special = 0;
    uint8_t type = 0;
};
bool lightFields(const void* light, LightFields& out) {
    if (!light) return false;
    const auto* data = static_cast<const unsigned char*>(light);
    if (!readable(data, 2037)) {
        diagAdd(diagnostics.lightSlowValidation, 1, std::memory_order_relaxed);
        if (!readable(data + 24, 4) || !readable(data + 2016, 1) || !readable(data + 2036, 1)) return false;
    }
    memcpy(&out.flags, data+24, sizeof out.flags);
    memcpy(&out.special, data+2016, sizeof out.special);
    memcpy(&out.type, data+2036, sizeof out.type);
    return true;
}
} // namespace

bool classifyLight(const void* light, light::LightRegistry::LightRecord& out) {
    out = {};
    out.light = light;
    if (!light) return false;
    LightFields fields{};
    if (g.config.trustEnginePointers) {
        // AddLight receives an engine-owned Light*; the original engine already
        // accesses this pointer. The optional safe mode retains VirtualQuery.
        const auto* data = static_cast<const unsigned char*>(light);
        memcpy(&fields.flags, data + 24, sizeof fields.flags);
        memcpy(&fields.special, data + 2016, sizeof fields.special);
        memcpy(&fields.type, data + 2036, sizeof fields.type);
    } else if (!lightFields(light, fields)) {
        return false;
    }
    out.flags = fields.flags;
    out.special = fields.special;
    out.type = fields.type;
    out.valid = true;
    return true;
}

void drawAdditionalLights(void* mesh, void* renderDevice, int /*flags*/,
                          const light::LightRegistry::LightRecord* lights, std::size_t lightCount) {
    if (!mesh || !g.origLightPass) return;
    diagAdd(diagnostics.snapshots, 1, std::memory_order_relaxed);
    if (!lightCount || !lights) return;
    diagAdd(diagnostics.nonEmpty, 1, std::memory_order_relaxed);
    diagAdd(diagnostics.totalRegistered, lightCount, std::memory_order_relaxed);
    if (g.config.logLights) {
        unsigned oldMax = diagnostics.maxSnapshot.load(std::memory_order_relaxed);
        while (oldMax < lightCount && !diagnostics.maxSnapshot.compare_exchange_weak(
                oldMax, static_cast<unsigned>(lightCount), std::memory_order_relaxed)) {}
    }
    // No heap allocations in the hot renderer path.
    std::array<const void*, 512> single;
    std::array<const void*, 512> uber;
    std::size_t singleCount = 0, uberCount = 0;
    const auto* meshBytes = static_cast<const unsigned char*>(mesh);
    uint32_t exclude0 = 0, exclude1 = 0;
    const bool hasExclusions = g.config.trustEnginePointers || readable(meshBytes + 2024, 8);
    if (hasExclusions) {
        memcpy(&exclude0, meshBytes + 2024, sizeof exclude0);
        memcpy(&exclude1, meshBytes + 2028, sizeof exclude1);
    }
    for (std::size_t i=0; i<lightCount; ++i) {
        const auto& entry = lights[i];
        const void* light = entry.light;
        if (singleCount + uberCount >= g.config.maxRenderLightsPerMesh) {
            diagAdd(diagnostics.limitedLights, 1, std::memory_order_relaxed);
            continue;
        }
        if (!light || g.variant == EngineVariant::Unknown ||
            (hasExclusions && (reinterpret_cast<uintptr_t>(light) == exclude0 ||
                               reinterpret_cast<uintptr_t>(light) == exclude1))) {
            diagAdd(diagnostics.rejectLight, 1, std::memory_order_relaxed);
            continue;
        }
        if (!entry.valid) { diagAdd(diagnostics.rejectLight, 1, std::memory_order_relaxed); continue; }
        const bool dynamic = (entry.flags & 0x00400000u) != 0;
        const unsigned type = entry.type;
        const bool special = entry.special != 0;
        if (!dynamic && !special && type != 3) { diagAdd(diagnostics.rejectLight, 1, std::memory_order_relaxed); continue; }
        diagAdd(diagnostics.accepted, 1, std::memory_order_relaxed);
        if (type == 2 || !g.origUberPass || g.variant == EngineVariant::Proto2003)
            single[singleCount++] = light;
        else uber[uberCount++] = light;
    }
    // Hard budget for native renderer calls. Per-light passes are expensive.
    unsigned passCount = 0;
    for (std::size_t i=0;i<singleCount;++i) {
        const void* l = single[i];
        if (passCount >= g.config.maxExtraPassesPerMesh) {
            diagAdd(diagnostics.limitedPasses, 1, std::memory_order_relaxed);
            continue;
        }
        g.origLightPass(mesh, l);
        ++passCount;
        diagAdd(diagnostics.singleCalls, 1, std::memory_order_relaxed);
    }
    if (!uberCount) return;
    if (passCount >= g.config.maxExtraPassesPerMesh) {
        diagAdd(diagnostics.limitedPasses, uberCount, std::memory_order_relaxed);
        return;
    }
    // A single UberLightPass does not need additive blending/state restoration.
    // Do not perform 17 probes or 8+ D3D state calls for this common case.
    const size_t width = g.variant == EngineVariant::Overdose ? 4 : 3;
    if (uberCount <= width && g.config.fastSingleUberPass) {
        g.origUberPass(mesh, uber.data(), static_cast<int>(uberCount));
        diagAdd(diagnostics.singleUberFastPaths, 1, std::memory_order_relaxed);
        diagAdd(diagnostics.uberCalls, 1, std::memory_order_relaxed);
        return;
    }
    Device dev = findDevice(renderDevice);
    if (dev) diagAdd(diagnostics.d3dFound, 1, std::memory_order_relaxed);
    else diagAdd(diagnostics.d3dMissing, 1, std::memory_order_relaxed);
    if (!dev) {
        // If the real D3D9 backend is unavailable, avoid touching unknown state.
        for (std::size_t i=0;i<uberCount;++i) {
            const void* l = uber[i];
            if (passCount >= g.config.maxExtraPassesPerMesh) {
                diagAdd(diagnostics.limitedPasses, 1, std::memory_order_relaxed); continue;
            }
            g.origLightPass(mesh, l);
            ++passCount;
            diagAdd(diagnostics.singleCalls, 1, std::memory_order_relaxed);
        }
        diagAdd(diagnostics.d3dFallback, 1, std::memory_order_relaxed);
        return;
    }
    ensureStateHook(dev);
    RenderStates states(dev);
    if (!states.valid) {
        diagAdd(diagnostics.d3dStateInvalid, 1, std::memory_order_relaxed);
        for (std::size_t i=0;i<uberCount;++i) {
            const void* l = uber[i];
            if (passCount >= g.config.maxExtraPassesPerMesh) {
                diagAdd(diagnostics.limitedPasses, 1, std::memory_order_relaxed); continue;
            }
            g.origLightPass(mesh,l);
            ++passCount;
            diagAdd(diagnostics.singleCalls, 1, std::memory_order_relaxed);
        }
        diagAdd(diagnostics.d3dFallback, 1, std::memory_order_relaxed);
        return;
    }
    for (size_t at=0; at<uberCount; at+=width) {
        if (passCount >= g.config.maxExtraPassesPerMesh) {
            diagAdd(diagnostics.limitedPasses, (uberCount-at+width-1)/width, std::memory_order_relaxed);
            break;
        }
        const auto count = static_cast<int>(std::min(width,uberCount-at));
        if (at) {
            states.additive();
            tlsAdditive = true;
        }
        g.origUberPass(mesh,uber.data()+at,count);
        ++passCount;
        diagAdd(diagnostics.uberCalls, 1, std::memory_order_relaxed);
    }
}
} // namespace pk
