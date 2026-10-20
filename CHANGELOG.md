# 0.1.7 — Light rename

- Project, custom EXE DLL, INI file, log file, diagnostic prefix, CMake target and release artifact renamed to Light.
- `dinput8.dll` stays unchanged for vanilla Painkiller loader compatibility.
- Public `Light_Initialize` export added; legacy `PKPatch_Initialize` remains exported.
- No lighting algorithm or performance changes relative to 0.1.6.

# v0.1.6 — CPU hot path / cached initialization (experimental)

- `Entity::AddLight` now captures the light flags/type once on registration; `WorldMesh::Draw` uses preclassified `LightRecord` snapshots. Repeated AddLight updates that record.
- A single native `RenderUberLightPass` is dispatched without searching for the Direct3D9 device or saving/restoring render states (`FastSingleUberPass=1`). Switch to 0 if drawing artifacts appear.
- D3D9 discovery is cached per `RenderDevice*`, checked with one pointer read on positive-cache hits. On failure, the lookup is retried only occasionally. Device pointer changes invalidate the cache.
- `LightRegistry::hasAny()` skips hash lookup and mutex for scenes without registered light overflow.
- Avoid zeroing 512-element buffers every draw; capture mesh exclusions once per draw.
- Counters are not atomically updated when `LogLights=0` (use `LogLights=1` only for diagnostic runs).
- `TrustEnginePointers=1` avoids WinAPI `VirtualQuery` for engine-provided pointers on the hot path. Switch to 0 when debugging custom/unknown Engine.dll layouts. If a build crashes, do not assume this flag is safe.
- A `v0.1.6` log marker makes stale DLLs easier to distinguish.
- Added unit tests for classified records, duplicate updates and active entity counts.

## Status and caveats

Core `ctest` passes on Linux; the Windows-specific code has only been syntax-checked with **stub WinAPI headers**, not linked using the user's MSVC toolchain or tested in Painkiller. This release is an optimization experiment, not a verified performance fix. There is **no promise** of 60+ FPS: rendering additional dynamic lights must still run per mesh/frame, and the engine may be limited by additional GPU passes.

# Changelog

## 0.1.5 — Hot-path performance investigation (experimental)
- Removed the per-mesh `std::vector` light-snapshot allocation in WorldMesh::Draw.
- Replaced per-mesh temporary single/uber vectors with bounded stack buffers.
- Validated memory once for the usual Light object layout rather than calling VirtualQuery for every field; fallback handles unusual page/region boundaries.
- Cached failed D3D device discoveries for 2048 lookups, then retries so late device initialization is still possible.
- Added diagnostic counters `d3dScans`, `d3dNegativeCacheSkips`, `lightSlowValidation` in `Light.log` when `LogLights=1`.
- Reduced default test budgets to 8 lights, 4 per mesh, 1 extra light pass.

**Not proven in-game**: these changes remove diagnosed overhead but are not confirmed to fix 11–16 FPS or the missing green light on floors. No actual MSVC DLL build was performed here.

# Changelog

## v0.1.4 — experimental performance budget

- One registry snapshot per WorldMesh::Draw.
- Cache D3D9 device discovery on stable RenderDevice.
- Limit passes and lights per mesh; add A/B mode `RenderExtraLights=0`.
- Add performance counters and FPS test instructions.
- This is not a confirmed fix for the original DLL. Game compatibility remains unverified.
