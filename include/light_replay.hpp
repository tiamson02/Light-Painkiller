#pragma once
#include "light_registry.hpp"
#include <array>
#include <cstddef>

namespace light {
// First replay the native sources, then up to extraPasses new lighting passes.
// No renderer/WinAPI dependency: the selection algorithm is unit-testable.
struct ReplayPlan {
    // Capacity must cover the entire registry, not an unrelated 16-light cap.
    // MaxLights is configurable to 512.
    std::array<LightRegistry::Object, 512> singles{};
    std::array<LightRegistry::Object, 512> uber{};
    std::size_t singleCount = 0, uberCount = 0;
    std::size_t nativeCount = 0, extraCount = 0, skipped = 0;
    unsigned extraPassesUsed = 0;
    bool valid = false;
};
ReplayPlan makeReplayPlan(const LightRegistry::LightRecord* records,
                          std::size_t count,
                          const LightRegistry::Object* native,
                          std::size_t nativeCount,
                          unsigned extraPasses,
                          bool uberSupported,
                          unsigned uberBatchWidth = 3) noexcept;
}
