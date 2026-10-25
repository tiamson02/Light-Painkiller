# Light v0.2.9.11 — An experimental dynamic lighting extension for Painkiller

An open library for the 32-bit PainEngine (DirectX 9), designed for the original *Painkiller* and compatible modifications. Its purpose is to render additional lighting on geometry when a mesh is affected by more than four light sources.

When the engine's standard limit of four light sources is exceeded, the library generates a lighting list and processes it after `WorldMesh::Draw`. The engine's native lighting passes are suppressed only when such a list is successfully constructed. Meshes affected by four or fewer light sources continue to use the standard rendering method. New lighting configurations created in *PainEditor* or *RoE* (Battle out of Hell) should be verified in-game.

## Configuration (`Light.ini`)

```ini
[Settings]
UnlockLights=1
MaxLights=128
ExtraPassesPerMesh=124
TimerSizeFix=1
LogLights=0
```

- `UnlockLights` — Enables the lighting extension (`0` disables lighting interception).
- `MaxLights` — The maximum size of the light source registry per entity (4–512); this is **not** the actual number of lights rendered.
- `ExtraPassesPerMesh` — The budget for additional rendering passes beyond the base set (0–512); setting this to `0` completely disables all lighting hooks.
- `TimerSizeFix` — An independent fix for the timer; can be set to `0` when testing lighting.
- `LogLights` — Provides aggregated diagnostic data at intervals of approximately 5 seconds. `replayedMeshes` indicates how many meshes were re-rendered via the new path, while `replayedNative` and `replayedOverflow` show the number of base and additional light sources, respectively, in the processed passes.

The `Light.log` file is **overwritten once upon process startup** and is saved in the same directory as the loaded DLL. ## Build

Windows 10, Visual Studio 2022 C++ (Win32/x86), CMake. Run `build-win32.bat`. It generates:

- `dist/vanilla/dinput8.dll` — for the standard version of Painkiller (place `Light.ini` alongside it).
- `dist/custom-exe/Light.dll` — for the Custom EXE version (place `Light.ini` alongside it).

Do not load both libraries simultaneously. For the editor, if the DLL load path contains `Bin\\Editor\\DINPUT8.dll`, the configuration file should be placed at `Bin\\Editor\\Light.ini`.

## Verification

Compare the same scene—featuring several green souls/torches—using identical graphics settings. Light should be visible on the ground, not just as glowing particles. Run the game first with `UnlockLights=0`, then with `UnlockLights=1`, `ExtraPassesPerMesh=124`, and `TimerSizeFix=0`. If the lighting does not appear, enable `LogLights=1` and check the `replayedMeshes` / `replayedOverflow` values ​​in `Light.log`. Note that `Installed lighting=1` is **not** proof that rendering is working correctly.

**Limitations:** Additional passes can be computationally expensive. Features such as PKMDL mode, the projection flashlight, interactions with the internal D3Dev state cache, and full compatibility with *Battle out of Hell* (RoE) require testing within the actual engine. Automated tests verify scheduling and call sequences using a simulated renderer, but they do not validate the final visual output in PainEditor.

**Rendering Budget:** `MaxLights` defines the registry capacity, while `ExtraPassesPerMesh` determines the number of *additional render calls* per mesh. For 128 standard lights, up to 124 passes may be required. By default, priority is given to the visibility of registered lights rather than to FPS. If the budget is reduced to 16–32, some light on overloaded meshes will inevitably be lost. Spatial light culling has not yet been restored.

## Integration with Custom EXE

`Data/addons/Light.dll` requires the **patched `Painkiller.exe` build** included in the package. The EXE synchronously calls the `Light_Initialize()` export upon loading the module, prior to the initialization of Lua or the game world. Unlike `dinput8.dll`, this build does not trigger a hook-installation thread from `DllMain`. After placing `dist/custom-exe/Light.dll` and `Light.ini` in the appropriate locations, build and replace the EXE. The EXE log should display `[Addons][Light] initialization=OK lightingActive=1`. This confirms that the hooks are in place, though it does not guarantee that the in-game lighting is functioning correctly. Do not use `Light.dll` simultaneously with a second instance of the patch loaded via `dinput8.dll`.