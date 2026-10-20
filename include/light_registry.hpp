#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace light {

// Engine-independent reconstruction of the per-entity light overflow queue.
// Does not patch, inject, or render anything by itself.
class LightRegistry {
public:
    using Object = const void*;
    explicit LightRegistry(std::size_t maxLights = 128);

    // Engine light attributes are captured on AddLight, never probed on every Draw.
    struct LightRecord {
        Object light = nullptr;
        std::uint32_t flags = 0;
        std::uint8_t special = 0;
        std::uint8_t type = 0;
        bool valid = false;
    };
    bool add(Object entity, Object light);  // engine-independent API
    bool addClassified(Object entity, Object light, std::uint32_t flags,
                       std::uint8_t special, std::uint8_t type, bool valid);
    void reset(Object entity);
    std::vector<Object> snapshot(Object entity) const;
    // Copy directly to a caller-owned buffer, avoiding per-mesh heap allocation.
    std::size_t snapshotInto(Object entity, Object* out, std::size_t capacity) const;
    std::size_t snapshotRecordsInto(Object entity, LightRecord* out, std::size_t capacity) const;
    bool hasAny() const noexcept { return activeEntities_.load(std::memory_order_relaxed) != 0; }
    std::size_t limit() const noexcept { return maxLights_; }
    std::size_t trackedEntities() const;

private:
    std::size_t maxLights_;
    mutable std::mutex mutex_;
    std::unordered_map<Object, std::vector<LightRecord>> lights_;
    std::atomic<std::size_t> activeEntities_{0};
};

// Reconstructed idea of multi-pass accumulation. Actual renderer selection,
// native-light exclusions and D3D render state changes remain engine-specific.
enum class LightPassMode { PerLight, UberBatchesOfThree, UberBatchesOfFour };
std::vector<std::vector<LightRegistry::Object>>
planLightPasses(const std::vector<LightRegistry::Object>& lights, LightPassMode mode);

} // namespace light
