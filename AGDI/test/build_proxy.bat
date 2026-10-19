@echo off
rem ---------------------------------------------------------------------------
rem Build AGDIProxy.cpp into CMSIS_AGDI.dll (32-bit - uVision is a 32-bit host).
rem
rem   build_proxy.bat            32-bit (default, what uVision needs)
rem   build_proxy.bat x64        for reference only, will not load in uVision
rem
rem Output: CMSIS_AGDI.dll next to this script.
rem ---------------------------------------------------------------------------
setlocal

if /i "%~1"=="x64" (set VCVARS=vcvars64.bat) else (set VCVARS=vcvars32.bat)

set VSDIR=D:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build
if not exist "%VSDIR%\%VCVARS%" (
    echo [proxy] vcvars not found: "%VSDIR%\%VCVARS%"
    exit /b 1
)

call "%VSDIR%\%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [proxy] vcvars failed
    exit /b 1
)

cd /d "%~dp0"

if exist CMSIS_AGDI.dll del /q CMSIS_AGDI.dll

cl /nologo /W3 /LD /O2 AGDIProxy.cpp /Fe:CMSIS_AGDI.dll
if errorlevel 1 (
    echo [proxy] BUILD FAILED
    exit /b 1
)

if not exist CMSIS_AGDI.dll (
    echo [proxy] BUILD FAILED - no DLL produced
    exit /b 1
)

echo [proxy] BUILD OK
for %%F in (CMSIS_AGDI.dll) do echo [proxy]   %%~nxF  %%~zF bytes
exit /b 0
