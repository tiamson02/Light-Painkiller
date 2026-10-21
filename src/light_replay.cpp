#include "light_replay.hpp"

namespace light {
namespace {
// Number of actual UberLightPass calls, not number of projected lights.
constexpr std::size_t batchCount(std::size_t sources, unsigned width) noexcept {
    return (sources + width - 1) / width;
}
}

ReplayPlan makeReplayPlan(const LightRegistry::LightRecord* records,
                          std::size_t count, const LightRegistry::Object* native,
                          std::size_t nativeCount, unsigned extraPasses,
                          bool uberSupported, unsigned uberBatchWidth) noexcept {
    ReplayPlan result;
    if (!records || count <= 4 || !extraPasses || count > 512 || nativeCount > 4 ||
        (nativeCount && !native)) return result;
    if (uberBatchWidth < 1 || uberBatchWidth > 4) return result;

    // Never suppress stock passes unless the entire native light set can be
    // replayed using valid registered objects. A failed plan is stock Draw.
    LightRegistry::Object base[4]{};
    std::size_t baseCount = 0;
    if (nativeCount) {
        for (std::size_t i=0; i<nativeCount; ++i) {
            if (!native[i]) return result;
            bool seen=false, duplicate=false;
            for (std::size_t j=0;j<baseCount;++j) if (base[j]==native[i]) duplicate=true;
            for (std::size_t j=0;j<count;++j)
                if (records[j].light==native[i] && records[j].valid) seen=true;
            if (!seen) return result;
            if (!duplicate) base[baseCount++]=native[i];
        }
    } else {
        for (std::size_t i=0;i<count && baseCount<4;++i) {
            if (!records[i].light || !records[i].valid) continue;
            bool duplicate=false;
            for (std::size_t j=0;j<baseCount;++j) if (base[j]==records[i].light) duplicate=true;
            if (!duplicate) base[baseCount++]=records[i].light;
        }
    }
    if (!baseCount) return result;

    auto append = [&](const LightRegistry::LightRecord& r) -> bool {
        if (!r.valid || !r.light) return false;
        if (uberSupported && r.type==2) {
            if (result.uberCount==result.uber.size()) return false;
            result.uber[result.uberCount++]=r.light;
        } else {
            if (result.singleCount==result.singles.size()) return false;
            result.singles[result.singleCount++]=r.light;
        }
        return true;
    };

    // Exact native sources first. Even in mixed modes, replay never excludes
    // a native light just because extra-light budget is exhausted.
    for (std::size_t i=0;i<baseCount;++i) {
        bool copied=false;
        for (std::size_t j=0;j<count;++j) if (records[j].light==base[i] && records[j].valid) {
            copied=append(records[j]); break;
        }
        if (!copied) return ReplayPlan{};
    }
    result.nativeCount=baseCount;
    const std::size_t baselinePasses = result.singleCount +
        batchCount(result.uberCount,uberBatchWidth);

    // Important: first four native type-2 lights may leave spare slots in
    // their last Uber batch. Additional type-2 sources can occupy these slots
    // at ZERO additional pass cost. Account for all passes together.
    for (std::size_t i=0;i<count;++i) {
        const auto& r=records[i];
        bool inBase=false;
        for (std::size_t j=0;j<baseCount;++j) if (base[j]==r.light) {inBase=true;break;}
        if (inBase) continue;
        if (!r.valid || !r.light) {++result.skipped;continue;}
        const bool uber=uberSupported && r.type==2;
        const std::size_t futureSingles=result.singleCount + (!uber ? 1u : 0u);
        const std::size_t futureUber=result.uberCount + (uber ? 1u : 0u);
        const auto futurePasses=futureSingles+batchCount(futureUber,uberBatchWidth);
        if (futurePasses-baselinePasses>extraPasses || !append(r)) {
            ++result.skipped;continue;
        }
        result.extraPassesUsed=static_cast<unsigned>(futurePasses-baselinePasses);
        ++result.extraCount;
    }
    result.valid=result.extraCount!=0;
    return result;
}
} // namespace light
