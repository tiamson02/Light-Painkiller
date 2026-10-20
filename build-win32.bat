@echo off
setlocal
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
if errorlevel 1 exit /b 1
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 exit /b 1
if not exist dist\vanilla mkdir dist\vanilla
if not exist dist\custom-exe mkdir dist\custom-exe
copy /Y build\release\Release\dinput8.dll dist\vanilla\ >nul
copy /Y build\release\Release\Light.dll dist\custom-exe\ >nul
copy /Y config\Light.ini dist\vanilla\ >nul
copy /Y config\Light.ini dist\custom-exe\ >nul
if not exist dist\third_party mkdir dist\third_party
copy /Y build\_deps\minhook-src\LICENSE.txt dist\third_party\MinHook_LICENSE.txt >nul
if errorlevel 1 echo WARNING: MinHook licence copy failed
if not exist dist\vanilla\dinput8.dll exit /b 1
if not exist dist\custom-exe\Light.dll exit /b 1
echo.
echo Build complete: dist\vanilla and dist\custom-exe
endlocal
