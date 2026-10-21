#include "patch.hpp"
#include "light_memory_cache.hpp"
#include <cstring>

namespace pk {
namespace {
bool validateLightFields(const void* light) {
    const auto* p = static_cast<const unsigned char*>(light);
    return readable(p + 24, 2037 - 24) ||
        (readable(p + 24, sizeof(std::uint32_t)) &&
         readable(p + 2016, sizeof(std::uint8_t)) &&
         readable(p + 2036, sizeof(std::uint8_t)));
}
}
bool classifyLight(const void* light, light::LightRegistry::LightRecord& out) {
    out = {};
    out.light = light;
    if (!light) return false;
    const unsigned char* p = static_cast<const unsigned char*>(light);
    // Repeated AddLight() calls often register the same Light* on many meshes.
    // Cache only the expensive VirtualQuery result for <=16ms, NEVER the
    // values of flags/special/type; those are refreshed on every AddLight.
    static thread_local light::LightMemoryCache cache;
    bool cacheHit = false;
    if (!cache.readable(light, GetTickCount(), validateLightFields, &cacheHit)) return false;
    if (cacheHit) bump(stats.lightCacheHits);
    else bump(stats.lightCacheMisses);
    memcpy(&out.flags, p + 24, sizeof(out.flags));
    memcpy(&out.special, p + 2016, sizeof(out.special));
    memcpy(&out.type, p + 2036, sizeof(out.type));
    out.valid = true;
    return true;
}
}
