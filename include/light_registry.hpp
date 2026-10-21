#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <array>
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
        // Keep the type trivially default-constructible. Hooks use a large
        // stack buffer but only read records that the registry actually fills.
        Object light;
        std::uint32_t flags;
        std::uint8_t special;
        std::uint8_t type;
        bool valid;
    };
    bool add(Object entity, Object light);  // engine-independent API
    bool addClassified(Object entity, Object light, std::uint32_t flags,
                       std::uint8_t special, std::uint8_t type, bool valid);
    void reset(Object entity);
    std::vector<Object> snapshot(Object entity) const;
    // Copy directly to a caller-owned buffer, avoiding per-mesh heap allocation.
    std::size_t snapshotInto(Object entity, Object* out, std::size_t capacity) const;
    std::size_t snapshotRecordsInto(Object entity, LightRecord* out, std::size_t capacity) const;
    // Single locked snapshot of all source records when 5+ are registered.
    // Unsupported sources must be filtered individually by the renderer.
    std::size_t snapshotSafeOverflowInto(Object entity, LightRecord* out,
                                         std::size_t capacity, bool& unsafe) const;
    bool hasAny() const noexcept { return activeEntities_.load(std::memory_order_relaxed) != 0; }
    std::size_t limit() const noexcept { return maxLights_; }
    std::size_t trackedEntities() const;
    // Only ordinary single-pass lights; type 2 requires UberLightPass.
    // Unsupported/projection lights are not rendered as overflow.
    static bool safePointLight(const LightRecord& light) noexcept;
    static bool safeUberLight(const LightRecord& light) noexcept;
    static bool allNativeObserved(const Object* native, std::size_t count,
                                  const Object* observed, std::size_t observedCount) noexcept;
    static bool contains(const Object* lights, std::size_t count, Object light) noexcept;
    static bool containsNativeRecord(const LightRecord* records, std::size_t count, Object light) noexcept;
    static std::size_t selectNotNative(const LightRecord* all, std::size_t count,
                                       const Object* native, std::size_t nativeCount,
                                       LightRecord* result, std::size_t capacity) noexcept;

private:
    std::size_t maxLights_;
    mutable std::mutex mutex_;
    struct Bucket {
        std::vector<LightRecord> records;
        // A small per-mesh Bloom filter rules out most duplicate searches.
        // False positives fall back to an exact linear lookup, never skipping
        // an existing light or dropping an additional source.
        std::array<std::uint64_t, 4> bloom{};
    };
    std::unordered_map<Object, Bucket> lights_;
    std::atomic<std::size_t> activeEntities_{0};
    // ResetLights is often called once per frame. Reuse vector capacity rather
    // than deallocate/reallocate on every frame; bound retention for level loads.
    std::size_t cachedInactive_ = 0;
    static constexpr std::size_t kInactiveCacheLimit = 2048;
};

// Reconstructed idea of multi-pass accumulation. Actual renderer selection,
// native-light exclusions and D3D render state changes remain engine-specific.
enum class LightPassMode { PerLight, UberBatchesOfThree, UberBatchesOfFour };
std::vector<std::vector<LightRegistry::Object>>
planLightPasses(const std::vector<LightRegistry::Object>& lights, LightPassMode mode);

} // namespace light
