@echo off
setlocal

where cl >nul 2>&1
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: cl.exe not in PATH and vswhere.exe not found.
    echo Run this script from a Visual Studio x64 Developer Command Prompt, or install Visual Studio C++ Build Tools.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if "%VSINSTALL%"=="" (
    echo ERROR: No Visual Studio installation with C++ x64 build tools found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize Visual Studio x64 build environment.
    exit /b 1
)

:have_cl
pushd "%~dp0"
echo Building crx2dng.exe (CLI)...
cl /nologo /O2 /Oi /Ot /GL /arch:AVX2 /EHsc /std:c++17 /MT /DNDEBUG ^
   src\main.cpp src\crx_decoder.cpp ^
   /Fe:crx2dng.exe /link /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 goto fail

echo Building crx2dng_gui.exe (GUI)...
cl /nologo /O2 /Oi /Ot /GL /arch:AVX2 /EHsc /std:c++17 /MT /DNDEBUG ^
   src\gui_main.cpp src\crx_decoder.cpp ^
   /Fe:crx2dng_gui.exe /link /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF
if errorlevel 1 goto fail

del /q main.obj gui_main.obj crx_decoder.obj 2>nul
popd
echo Build succeeded!
exit /b 0

:fail
del /q main.obj gui_main.obj crx_decoder.obj 2>nul
popd
exit /b 1
