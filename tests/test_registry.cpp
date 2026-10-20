#include "light_registry.hpp"
#include <cassert>
#include <array>
#include <cstdint>
#include <iostream>

int main() {
    light::LightRegistry registry(128);
    const auto e = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x10000));
    for (std::uintptr_t i = 1; i <= 150; ++i) {
        const auto light = reinterpret_cast<const void*>(0x20000 + i * 16);
        const bool inserted = registry.add(e, light);
        assert(inserted == (i <= 128));
    }
    assert(registry.snapshot(e).size() == 128);
    std::array<const void*, 128> testBuffer{};
    assert(registry.snapshotInto(e,testBuffer.data(),testBuffer.size()) == 128);
    assert(testBuffer[0] == reinterpret_cast<const void*>(0x20000+16));
    std::array<const void*, 4> shortBuffer{};
    assert(registry.snapshotInto(e,shortBuffer.data(),shortBuffer.size()) == 4);
    assert(registry.snapshotInto(e,nullptr,0) == 0);
    assert(!registry.add(e, registry.snapshot(e).front()));
    auto batches3 = light::planLightPasses(registry.snapshot(e), light::LightPassMode::UberBatchesOfThree);
    auto batches4 = light::planLightPasses(registry.snapshot(e), light::LightPassMode::UberBatchesOfFour);
    assert(batches3.size() == 43 && batches3.back().size() == 2);
    assert(batches4.size() == 32 && batches4.back().size() == 4);
    assert(registry.hasAny());
    registry.reset(e);
    assert(!registry.hasAny());
    assert(registry.snapshot(e).empty());
    assert(registry.snapshotInto(e,testBuffer.data(),testBuffer.size()) == 0);
    // Classification belongs to registration, not to each mesh draw.
    const auto l = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x40000));
    assert(registry.addClassified(e, l, 0x00400000, 0, 3, true));
    std::array<light::LightRegistry::LightRecord, 2> records{};
    assert(registry.snapshotRecordsInto(e, records.data(), records.size()) == 1);
    assert(records[0].light == l && records[0].flags == 0x00400000);
    assert(records[0].type == 3 && records[0].valid);
    assert(!registry.addClassified(e, l, 0, 1, 2, true));
    assert(registry.snapshotRecordsInto(e, records.data(), records.size()) == 1);
    assert(records[0].flags == 0 && records[0].special == 1 && records[0].type == 2);
    registry.reset(e);
    assert(!registry.hasAny());
    std::cout << "LightRegistry: all tests passed\n";
}
