@echo off
REM Drag and drop one or more .CRX files onto this script to convert them
REM into DaVinci Resolve CinemaDNG folders right next to the source files.
setlocal
if "%~1"=="" (
    echo Drag and drop a .CRX file onto this script, or run:
    echo   crx2dng.exe path\to\CRX00001.CRX
    pause
    exit /b 0
)
:loop
if "%~1"=="" goto done
echo.
"%~dp0crx2dng.exe" "%~1"
shift
goto loop
:done
echo.
pause
