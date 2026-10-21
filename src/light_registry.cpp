#include "light_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace light {

namespace {
std::uintptr_t mixPointer(const void* p) noexcept {
    std::uintptr_t x = reinterpret_cast<std::uintptr_t>(p) >> 4;
    // Both 32 and 64-bit versions; keep hash entropy for large aligned lights.
    x ^= x >> 15;
    x *= static_cast<std::uintptr_t>(0x7feb352du);
    x ^= x >> 13;
    x *= static_cast<std::uintptr_t>(0x846ca68bu);
    return x ^ (x >> 16);
}
}

LightRegistry::LightRegistry(std::size_t maxLights)
    : maxLights_(maxLights) {
    if (maxLights_ == 0) throw std::invalid_argument("maxLights must be positive");
}

bool LightRegistry::add(Object entity, Object light) {
    return addClassified(entity, light, 0, 0, 0, false);
}

bool LightRegistry::addClassified(Object entity, Object light, std::uint32_t flags,
                                  std::uint8_t special, std::uint8_t type, bool valid) {
    if (!entity || !light) return false;
    std::lock_guard<std::mutex> guard(mutex_);
    const auto inserted = lights_.try_emplace(entity);
    auto& bucket = inserted.first->second;
    auto& entries = bucket.records;
    const auto hash = mixPointer(light);
    const auto slot = hash & 3u;
    const std::uint64_t bit = std::uint64_t{1} << ((hash >> 2) & 63u);
    if (bucket.bloom[slot] & bit) {
        // A Bloom hit might be a collision; confirm by pointer equality.
        for (auto& entry : entries) {
            if (entry.light == light) {
                // Re-registration updates attributes rather than accumulating
                // duplicate work or retaining an old light classification.
                entry = {light, flags, special, type, valid};
                return false;
            }
        }
    }
    if (entries.size() >= maxLights_) return false;
    if (entries.empty()) {
        if (!inserted.second && cachedInactive_ > 0) --cachedInactive_;
        activeEntities_.fetch_add(1, std::memory_order_relaxed);
    }
    entries.push_back({light, flags, special, type, valid});
    bucket.bloom[slot] |= bit;
    return true;
}
void LightRegistry::reset(Object entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end() || it->second.records.empty()) return;
    activeEntities_.fetch_sub(1, std::memory_order_relaxed);
    if (cachedInactive_ >= kInactiveCacheLimit) {
        lights_.erase(it); // bound retained inactive mesh entries
    } else {
        it->second.records.clear(); // retains capacity for reuse next frame
        it->second.bloom.fill(0);
        ++cachedInactive_;
    }
}

std::vector<LightRegistry::Object> LightRegistry::snapshot(Object entity) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return {};
    std::vector<Object> result;
    result.reserve(it->second.records.size());
    for (const auto& entry : it->second.records) result.push_back(entry.light);
    return result;
}

std::size_t LightRegistry::snapshotInto(Object entity, Object* out, std::size_t capacity) const {
    if (!out || capacity == 0) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return 0;
    const auto count = std::min(it->second.records.size(), capacity);
    for (std::size_t i = 0; i < count; ++i) out[i] = it->second.records[i].light;
    return count;
}

std::size_t LightRegistry::snapshotRecordsInto(Object entity, LightRecord* out,
                                                std::size_t capacity) const {
    if (!out || capacity == 0) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return 0;
    const auto count = std::min(it->second.records.size(), capacity);
    std::copy_n(it->second.records.begin(), count, out);
    return count;
}

std::size_t LightRegistry::snapshotSafeOverflowInto(Object entity, LightRecord* out,
                                                     std::size_t capacity, bool& unsafe) const {
    unsafe = false;
    if (!entity || !out || capacity < 5) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end() || it->second.records.size() <= 4) return 0;
    const auto& records = it->second.records;
    // A partial snapshot could drop the engine's real first four sources.
    if (records.size() > capacity) { unsafe = true; return 0; }
    // Do not throw away the ENTIRE mesh when one source is special/type 2.
    // The renderer chooses supported overflow candidates individually.
    std::copy(records.begin(), records.end(), out);
    return records.size();
}

bool LightRegistry::safePointLight(const LightRecord& light) noexcept {
    if (!light.valid || !light.light || light.special != 0 || light.type == 2)
        return false;
    return (light.flags & 0x00400000u) != 0 || light.type == 3;
}

bool LightRegistry::safeUberLight(const LightRecord& light) noexcept {
    // Original erychpatch routes type == 2 to UberLightPass.
    // Keep special/projective lights on the native renderer.
    return light.valid && light.light && light.special == 0 && light.type == 2 &&
           (light.flags & 0x00400000u) != 0;
}

bool LightRegistry::allNativeObserved(const Object* native, std::size_t count,
                                      const Object* observed, std::size_t observedCount) noexcept {
    if (!native || !observed || count != 4 || observedCount < count) return false;
    for (std::size_t i=0; i<count; ++i) {
        if (!native[i] || !contains(observed, observedCount, native[i])) return false;
    }
    return true;
}

bool LightRegistry::contains(const Object* lights, std::size_t count, Object light) noexcept {
    if (!lights) return false;
    for (std::size_t i = 0; i < count; ++i) if (lights[i] == light) return true;
    return false;
}

bool LightRegistry::containsNativeRecord(const LightRecord* records, std::size_t count, Object light) noexcept {
    if (!records || !light) return false;
    for (std::size_t i = 0; i<count; ++i)
        if (records[i].light == light && safePointLight(records[i])) return true;
    return false;
}

std::size_t LightRegistry::selectNotNative(const LightRecord* all, std::size_t count,
                                          const Object* native, std::size_t nativeCount,
                                          LightRecord* result, std::size_t capacity) noexcept {
    if (!all || !native || !nativeCount || !result || !capacity) return 0;
    std::size_t used = 0;
    for (std::size_t i = 0; i < count && used < capacity; ++i) {
        if (!all[i].light || contains(native, nativeCount, all[i].light)) continue;
        bool duplicate = false;
        for (std::size_t j = 0; j < used; ++j)
            if (result[j].light == all[i].light) { duplicate = true; break; }
        if (!duplicate) result[used++] = all[i];
    }
    return used;
}

std::size_t LightRegistry::trackedEntities() const {
    return activeEntities_.load(std::memory_order_relaxed);
}

std::vector<std::vector<LightRegistry::Object>>
planLightPasses(const std::vector<LightRegistry::Object>& lights, LightPassMode mode) {
    std::size_t width = 1;
    if (mode == LightPassMode::UberBatchesOfThree) width = 3;
    if (mode == LightPassMode::UberBatchesOfFour) width = 4;

    std::vector<std::vector<LightRegistry::Object>> result;
    for (std::size_t pos = 0; pos < lights.size(); pos += width) {
        const auto end = std::min(pos + width, lights.size());
        result.emplace_back(lights.begin() + pos, lights.begin() + end);
    }
    return result;
}

} // namespace light
