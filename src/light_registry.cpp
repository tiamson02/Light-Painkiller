#include "light_registry.hpp"

#include <algorithm>
#include <stdexcept>

namespace light {

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
    auto& entries = lights_[entity];
    for (auto& entry : entries) {
        if (entry.light == light) {
            // Repeated AddLight can carry updated flags/type on later frames.
            entry = {light, flags, special, type, valid};
            return false;
        }
    }
    if (entries.size() >= maxLights_) return false;
    if (entries.empty()) activeEntities_.fetch_add(1, std::memory_order_relaxed);
    entries.push_back({light, flags, special, type, valid});
    return true;
}

void LightRegistry::reset(Object entity) {
    if (!entity) return;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return;
    if (!it->second.empty()) activeEntities_.fetch_sub(1, std::memory_order_relaxed);
    lights_.erase(it);
}

std::vector<LightRegistry::Object> LightRegistry::snapshot(Object entity) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return {};
    std::vector<Object> result;
    result.reserve(it->second.size());
    for (const auto& entry : it->second) result.push_back(entry.light);
    return result;
}

std::size_t LightRegistry::snapshotInto(Object entity, Object* out, std::size_t capacity) const {
    if (!out || capacity == 0) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return 0;
    const auto count = std::min(it->second.size(), capacity);
    for (std::size_t i = 0; i < count; ++i) out[i] = it->second[i].light;
    return count;
}

std::size_t LightRegistry::snapshotRecordsInto(Object entity, LightRecord* out,
                                                std::size_t capacity) const {
    if (!out || capacity == 0) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = lights_.find(entity);
    if (it == lights_.end()) return 0;
    const auto count = std::min(it->second.size(), capacity);
    std::copy_n(it->second.begin(), count, out);
    return count;
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
