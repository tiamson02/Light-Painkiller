#include "patch.hpp"
#include <cstring>

namespace pk {
bool classifyLight(const void* light, light::LightRegistry::LightRecord& out) {
    out = {};
    out.light = light;
    if (!light) return false;
    const unsigned char* p = static_cast<const unsigned char*>(light);
    // Known 32-bit PainEngine layout, from the supplied original decompilation.
    // Only read at registration; never scan memory or classify every Draw.
    if (!readable(p + 24, sizeof(out.flags)) ||
        !readable(p + 2016, sizeof(out.special)) ||
        !readable(p + 2036, sizeof(out.type))) return false;
    memcpy(&out.flags, p + 24, sizeof(out.flags));
    memcpy(&out.special, p + 2016, sizeof(out.special));
    memcpy(&out.type, p + 2036, sizeof(out.type));
    out.valid = true;
    return true;
}
}
