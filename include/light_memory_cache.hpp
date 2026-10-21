#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace light {

// Short-lived, thread-local cache of the *readability check*, not light attributes.
// The flags/type fields are still read from the live engine object on every AddLight,
// so animated lights and runtime type changes never reuse stale classifications.
//
// Only affirmative validations are cached. A later invalid pointer from the engine
// cannot be made safe by any cache: callers must supply live Engine light objects.
class LightMemoryCache {
public:
    using Validator = bool (*)(const void*);
    static constexpr std::uint32_t kReuseMs = 16;
    static constexpr std::size_t kSlots = 512; // direct-mapped, no locks or allocations

    bool readable(const void* object, std::uint32_t tick, Validator validate,
                  bool* cacheHit = nullptr) noexcept {
        if (cacheHit) *cacheHit = false;
        if (!object || !validate) return false;
        const auto key = reinterpret_cast<std::uintptr_t>(object);
        // Same aligned Light* tends to be registered on multiple meshes.
        auto hash = key >> 4;
        hash ^= hash >> 15;
        hash *= static_cast<std::uintptr_t>(0x7feb352du);
        hash ^= hash >> 13;
        hash *= static_cast<std::uintptr_t>(0x846ca68bu);
        const auto index = (hash ^ (hash >> 16)) & (kSlots - 1);
        auto& slot = slots_[index];
        if (slot.object == object && static_cast<std::uint32_t>(tick - slot.tick) <= kReuseMs) {
            if (cacheHit) *cacheHit = true;
            return true;
        }
        const bool valid = validate(object);
        if (valid) {
            slot.object = object;
            slot.tick = tick;
        } else {
            // Never cache a failed probe (the address may become valid later).
            if (slot.object == object) slot.object = nullptr;
        }
        return valid;
    }

private:
    struct Slot { const void* object = nullptr; std::uint32_t tick = 0; };
    std::array<Slot, kSlots> slots_{};
};

} // namespace light
