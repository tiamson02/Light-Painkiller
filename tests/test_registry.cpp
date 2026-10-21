#include "light_registry.hpp"
#include "light_memory_cache.hpp"
#include "light_replay.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cstdint>
#include <iostream>

static const void* ptr(std::uintptr_t n) { return reinterpret_cast<const void*>(n); }
static int validationCount = 0;
static bool validateTest(const void* p) {
    ++validationCount;
    return p != ptr(0xBAD00);
}
int main() {
    // Cached readability must not freeze live light attributes or rejected pointers.
    light::LightMemoryCache cache;
    bool hit = true;
    const void* one = ptr(0x123400);
    assert(cache.readable(one, 100, validateTest, &hit) && !hit && validationCount == 1);
    assert(cache.readable(one, 108, validateTest, &hit) && hit && validationCount == 1);
    assert(cache.readable(one, 117, validateTest, &hit) && !hit && validationCount == 2);
    const void* bad = ptr(0xBAD00);
    assert(!cache.readable(bad, 118, validateTest, &hit) && !hit && validationCount == 3);
    assert(!cache.readable(bad, 118, validateTest, &hit) && !hit && validationCount == 4);
    // UINT32 tick rollover is well-defined for the unsigned expiry calculation.
    assert(cache.readable(one, 0xFFFFFFF8u, validateTest, &hit) && !hit);
    assert(cache.readable(one, 4u, validateTest, &hit) && hit);

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
    assert( Registry::safeUberLight(projected));
    Record special{ptr(0x30000),0x400000,1,1,true};
    assert(!Registry::safePointLight(special));
    assert(!Registry::safeUberLight(special));
    Record unknown{ptr(0x30000),0x400000,0,1,false};
    assert(!Registry::safePointLight(unknown));
    Record nondynamic{ptr(0x30000),0,0,1,true};
    assert(!Registry::safePointLight(nondynamic));
    assert(!Registry::safeUberLight(nondynamic));
    Record type3{ptr(0x30000),0,0,3,true};
    assert(Registry::safePointLight(type3));
    bool unsafe = false;
    std::array<Record, 128> safeRecords{};
    assert(registry.snapshotSafeOverflowInto(entity, safeRecords.data(), safeRecords.size(), unsafe) == 128);
    assert(!unsafe);
    // Do not silently truncate an overflow snapshot: it could omit native lights.
    assert(registry.snapshotSafeOverflowInto(entity, safeRecords.data(), 10, unsafe) == 0);
    assert(unsafe);
    registry.reset(entity);
    assert(!registry.hasAny());
    assert(registry.snapshot(entity).empty());
    assert(registry.snapshotSafeOverflowInto(entity, safeRecords.data(), safeRecords.size(), unsafe) == 0);
    assert(!unsafe);
    // The fast path must leave the destination buffer untouched when there
    // are 0..4 light sources. This is important for high-mesh-count levels.
    const auto* sentinel = ptr(0xBADBEEF0);
    for (auto& entry : safeRecords) entry.light = sentinel;
    for (int i=0;i<4;++i)
        assert(registry.addClassified(entity,ptr(0x70000+i*16),0x400000,0,1,true));
    assert(registry.snapshotSafeOverflowInto(entity,safeRecords.data(),safeRecords.size(),unsafe)==0);
    assert(!unsafe && safeRecords[0].light == sentinel);
    registry.reset(entity);
    // Re-register the same mesh after a reset. Cached vectors must not leave
    // stale lights, duplicate entries, or a wrong tracked-entity count.
    for (int frame = 0; frame < 200; ++frame) {
        for (int i = 0; i < 5; ++i)
            assert(registry.addClassified(entity, ptr(0x70000 + i*16), 0x400000, 0, 1, true));
        assert(registry.trackedEntities() == 1);
        assert(registry.snapshotSafeOverflowInto(entity, safeRecords.data(), safeRecords.size(), unsafe) == 5);
        assert(!unsafe);
        assert(!registry.addClassified(entity, ptr(0x70000), 0x400000, 0, 1, true));
        registry.reset(entity);
        assert(registry.trackedEntities() == 0);
    }
    for (int i = 0; i < 5; ++i)
        assert(registry.addClassified(entity, ptr(0x70000 + i*16), 0x400000, 0, 1, true));
    assert(!registry.addClassified(entity, ptr(0x70010), 0x400000, 1, 1, true));
    // An unsupported light must NOT blank unrelated overflow candidates.
    assert(registry.snapshotSafeOverflowInto(entity, safeRecords.data(), safeRecords.size(), unsafe) == 5);
    assert(!unsafe);
    assert(Registry::safePointLight(safeRecords[0]));
    assert(!Registry::safePointLight(safeRecords[1]));
    registry.reset(entity);
    // Mixed native/overflow scene: type-2 sources use an Uber pass, regular
    // sources remain eligible for single passes, and specials are ignored.
    for(int i=0;i<4;++i)
        assert(registry.addClassified(entity, ptr(0x80000+16*i), 0x400000, 0, 1, true));
    assert(registry.addClassified(entity, ptr(0x80040), 0x400000, 0, 2, true));
    assert(registry.addClassified(entity, ptr(0x80050), 0x400000, 0, 1, true));
    assert(registry.addClassified(entity, ptr(0x80060), 0x400000, 1, 1, true));
    assert(registry.snapshotRecordsInto(entity, safeRecords.data(), safeRecords.size()) == 7);
    assert(Registry::safeUberLight(safeRecords[4]));
    assert(Registry::safePointLight(safeRecords[5]));
    assert(!Registry::safePointLight(safeRecords[6]));
    assert(!Registry::safeUberLight(safeRecords[6]));
    registry.reset(entity);
    // In particular, no recorded RenderLightPass/RenderUberLightPass calls are
    // required: some editor paths do not call four native passes at all.
    Registry replayReg(16);
    const void* replayMesh=ptr(0xAA000);
    for (int i=0;i<6;++i)
        assert(replayReg.addClassified(replayMesh,ptr(0xAB000+16*i),0x400000,0,1,true));
    std::array<Record, 16> replayRecords{};
    const auto replayN=replayReg.snapshotRecordsInto(replayMesh,replayRecords.data(),16);
    assert(replayN==6);
    auto plan=light::makeReplayPlan(replayRecords.data(),replayN,nullptr,0,2,false);
    assert(plan.valid && plan.nativeCount==4 && plan.extraCount==2);
    assert(plan.singleCount==6 && plan.uberCount==0);
    auto noBudget=light::makeReplayPlan(replayRecords.data(),replayN,nullptr,0,0,false);
    assert(!noBudget.valid);
    // A genuine native set need not contain four lights, and need not be the
    // first elements in AddLight registration order.
    const void* twoNative[2]={ptr(0xAB020),ptr(0xAB030)};
    auto alt=light::makeReplayPlan(replayRecords.data(),replayN,twoNative,2,1,false);
    assert(alt.valid && alt.nativeCount==2 && alt.extraCount==1);
    assert(alt.singles[0]==twoNative[0] && alt.singles[1]==twoNative[1]);
    const void* wrongNative[1]={ptr(0xFEEEE)};
    assert(!light::makeReplayPlan(replayRecords.data(),replayN,wrongNative,1,2,false).valid);
    // Multiple projected lights may share an Uber pass: never send them to
    // the wrong single-pass route when the medium renderer is active.
    Registry uberReg(16);
    for(int i=0;i<10;++i)
        assert(uberReg.addClassified(replayMesh,ptr(0xAC000+16*i),0x400000,0,2,true));
    assert(uberReg.snapshotRecordsInto(replayMesh,replayRecords.data(),16)==10);
    auto uberPlan=light::makeReplayPlan(replayRecords.data(),10,nullptr,0,2,true);
    assert(uberPlan.valid && uberPlan.nativeCount==4 && uberPlan.extraCount==6);
    assert(uberPlan.uberCount==10 && uberPlan.singleCount==0);
    // Invalid records cannot replace a native light; stock path must survive.
    replayRecords[0].valid=false;
    assert(!light::makeReplayPlan(replayRecords.data(),10,uberPlan.uber.data(),1,2,true).valid);
    // Regression: v0.2.6 used 16-element arrays and a default pass budget of
    // two, silently dropping 69 of 75 overlapping ordinary sources.
    Registry many(128);
    const auto heavyMesh=ptr(0xDD000);
    for (int i=0; i<128; ++i)
        assert(many.addClassified(heavyMesh,ptr(0xE0000+i*16),0x400000,0,1,true));
    std::array<Record,128> heavyRecords{};
    const auto heavyN=many.snapshotRecordsInto(heavyMesh,heavyRecords.data(),heavyRecords.size());
    assert(heavyN==128);
    auto full=light::makeReplayPlan(heavyRecords.data(),heavyN,nullptr,0,124,false);
    assert(full.valid && full.nativeCount==4 && full.extraCount==124);
    assert(full.singleCount==128 && full.uberCount==0 && full.skipped==0);
    auto oldBudget=light::makeReplayPlan(heavyRecords.data(),heavyN,nullptr,0,2,false);
    assert(oldBudget.valid && oldBudget.singleCount==6 && oldBudget.skipped==122);
    // Proper Uber accounting: four baseline projected lights consume two
    // 3-light batches, so the fifth and sixth do not cost an extra batch.
    Registry projectedMany(128);
    for (int i=0;i<75;++i)
        assert(projectedMany.addClassified(heavyMesh,ptr(0xF0000+i*16),0x400000,0,2,true));
    const auto projectedN=projectedMany.snapshotRecordsInto(heavyMesh,heavyRecords.data(),heavyRecords.size());
    auto allUber=light::makeReplayPlan(heavyRecords.data(),projectedN,nullptr,0,124,true,3);
    assert(allUber.valid && allUber.uberCount==75 && allUber.extraCount==71);
    auto spareUber=light::makeReplayPlan(heavyRecords.data(),projectedN,nullptr,0,1,true,3);
    assert(spareUber.valid && spareUber.uberCount==9 && spareUber.extraCount==5 &&
           spareUber.extraPassesUsed==1);
    auto invalidWidth=light::makeReplayPlan(heavyRecords.data(),projectedN,nullptr,0,124,true,0);
    assert(!invalidWidth.valid);
    std::cout << "Light: 128-light full-capacity replay and batching tests passed\n";
    std::cout << "Light: replay/no-four-native-path tests passed\n";
    std::cout << "Light: mixed-light overflow selection tests passed\n";
}
