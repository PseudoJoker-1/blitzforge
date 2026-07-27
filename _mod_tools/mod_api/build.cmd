@echo off
setlocal
pushd "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo Visual Studio 2022 C++ tools were not found.
    popd
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo vcvars32 failed.
    popd
    exit /b 1
)

if not exist "build" mkdir "build"
if not exist "build\smoke_env" mkdir "build\smoke_env"
if not exist "build\smoke_env\mods" mkdir "build\smoke_env\mods"
if not exist "build\smoke_env\mods\data\hello_mod\resources" mkdir "build\smoke_env\mods\data\hello_mod\resources"
if not exist "build\smoke_env\mods\data\resource_mod\low" mkdir "build\smoke_env\mods\data\resource_mod\low"
if not exist "build\smoke_env\mods\data\resource_mod\high" mkdir "build\smoke_env\mods\data\resource_mod\high"
copy /y "tests\fixtures\example.yaml.dvpl" "build\smoke_env\mods\data\hello_mod\resources\example.yaml.dvpl" >nul
copy /y "tests\fixtures\low\value.txt" "build\smoke_env\mods\data\resource_mod\low\value.txt" >nul
copy /y "tests\fixtures\high\value.txt" "build\smoke_env\mods\data\resource_mod\high\value.txt" >nul

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /Iinclude ^
    src\wotb_mod_runtime.cpp ^
    /Fo"build\wotb_mod_runtime.obj"
if errorlevel 1 goto :failed

cl /nologo /TC /W4 /c ^
    /Iinclude ^
    tests\abi_c_compile.c ^
    /Fo"build\abi_c_compile.obj"
if errorlevel 1 goto :failed

lib /nologo /OUT:"build\wotb_mod_runtime.lib" ^
    "build\wotb_mod_runtime.obj"
if errorlevel 1 goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\hello_mod\hello_mod.cpp ^
    /Fo"build\hello_mod.obj" ^
    /link /OUT:"build\hello_mod.dll" /IMPLIB:"build\hello_mod.lib"
if errorlevel 1 goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\fault_mod.cpp ^
    /Fo"build\fault_mod.obj" ^
    /link /OUT:"build\fault_mod.dll" /IMPLIB:"build\fault_mod.lib"
if errorlevel 1 goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\resource_mod.cpp ^
    /Fo"build\resource_mod.obj" ^
    /link /OUT:"build\resource_mod.dll" /IMPLIB:"build\resource_mod.lib"
if errorlevel 1 goto :failed

copy /y "build\hello_mod.dll" "build\smoke_env\mods\hello_mod.dll" >nul
copy /y "build\fault_mod.dll" "build\smoke_env\mods\fault_mod.dll" >nul
copy /y "build\resource_mod.dll" "build\smoke_env\mods\resource_mod.dll" >nul

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /Iinclude ^
    tests\smoke_host.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\smoke_host.obj" ^
    /link /OUT:"build\smoke_host.exe" /IMPLIB:"build\smoke_host.lib"
if errorlevel 1 goto :failed

"build\smoke_host.exe" "build\smoke_env"
if errorlevel 1 goto :failed

echo.
echo === Mod API build and smoke test passed ===
echo Runtime: build\wotb_mod_runtime.lib
echo SDK:     include\wotb_mod_api.h
echo Example: build\hello_mod.dll
popd
exit /b 0

:failed
echo.
echo Build or smoke test failed.
popd
exit /b 1
