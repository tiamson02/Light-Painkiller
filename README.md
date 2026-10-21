# Light v0.2.1 — experimental open-source dynamic-light extension

For 32-bit DirectX 9 Painkiller Engine.dll builds. Not verified in the game yet.

The original `WorldMesh::Draw` and native light passes remain active. Extra ordinary lights may be rendered *inside* a native light-pass callback only if all four native lights have been identified and observed. Special, unknown, type-2, and projected-light scenarios fall back to the original engine and retain the original light cap. This is conservative by design: torch/flashlight projection takes precedence over maximizing the light count.

The patch does not hook the global D3D9 vtable or intercept `SetRenderState`.

## Configuration (`Light.ini`)

```ini
[Settings]
UnlockLights=1
MaxLights=128
ExtraPassesPerMesh=2
TimerSizeFix=1
LogLights=0
```

## Build

Run `build-win32.bat` on Windows with Visual Studio 2022 Win32 C++ build tools and CMake. Outputs: `dist/vanilla/dinput8.dll` and `dist/custom-exe/Light.dll`, each with `Light.ini`. Keep backups. Only deploy one loader variant at a time.

The `Light_Initialize` export is available for Custom EXE integration; `PKPatch_Initialize` remains as a legacy alias.

### Validation

Compare the same scene (including torch/flashlight, weapons, world geometry and FPS): no patch; `ExtraPassesPerMesh=0`; `ExtraPassesPerMesh=2`. Keep `TimerSizeFix=0` for isolation across runs. Enable `LogLights=1` only for diagnostics. 

The component is an experimental reconstruction of the light overflow pipeline, *not a verified solution for all projected lights, PKMDL models, or mods.* Tests in this package validate only the engine-independent registry and guard logic. Build and live tests on Windows are still required.