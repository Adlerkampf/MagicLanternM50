@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Visual Studio 2022 x64 environment not found.
    exit /b 1
)
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
