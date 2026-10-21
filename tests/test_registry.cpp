#include "light_registry.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cstdint>
#include <iostream>

static const void* ptr(std::uintptr_t n) { return reinterpret_cast<const void*>(n); }
int main() {
    using Registry = light::LightRegistry;
    using Record = Registry::LightRecord;
    Registry registry(128);
    const void* entity = ptr(0x10000);
    for (unsigned i=0; i<140; ++i)
        assert(registry.addClassified(entity, ptr(0x20000+16*i), 0x400000, 0, 1, true) == (i<128));
    assert(registry.hasAny());
    std::array<Record, 128> records{};
    assert(registry.snapshotRecordsInto(entity, records.data(), 128) == 128);
    const void* native[4] = {records[0].light,records[1].light,records[2].light,records[3].light};
    const void* incomplete[3] = {native[0],native[1],native[3]};
    const void* completed[4] = {native[3],native[0],native[2],native[1]};
    assert(!Registry::allNativeObserved(native,4,incomplete,3));
    assert( Registry::allNativeObserved(native,4,completed,4));
    assert(!Registry::allNativeObserved(native,3,completed,4));
    Record extra[128]{};
    assert(Registry::selectNotNative(records.data(),128,native,4,extra,128) == 124);
    assert(extra[0].light == records[4].light);
    for (const auto& r : records) assert(Registry::safePointLight(r));
    for (auto* p : native) assert(Registry::containsNativeRecord(records.data(),128,p));
    assert(!Registry::containsNativeRecord(records.data(),128,ptr(0x90000)));
    assert(!Registry::containsNativeRecord(nullptr,0,native[0]));
    Record projected{ptr(0x30000),0x400000,0,2,true};
    assert(!Registry::safePointLight(projected));
    Record special{ptr(0x30000),0x400000,1,1,true};
    assert(!Registry::safePointLight(special));
    Record unknown{ptr(0x30000),0x400000,0,1,false};
    assert(!Registry::safePointLight(unknown));
    Record nondynamic{ptr(0x30000),0,0,1,true};
    assert(!Registry::safePointLight(nondynamic));
    Record type3{ptr(0x30000),0,0,3,true};
    assert(Registry::safePointLight(type3));
    registry.reset(entity);
    assert(!registry.hasAny());
    assert(registry.snapshot(entity).empty());
    std::cout << "Light: safe in-Draw selection tests passed\n";
}
