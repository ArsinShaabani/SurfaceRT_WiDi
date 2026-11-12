@echo off
rem SurfaceRT-WiDi - builds the Windows RT 8.1 (ARM) binaries with nmake.
setlocal enabledelayedexpansion

set "REPO=%~dp0..\.."
pushd "%REPO%" || exit /b 1

if /i "%VSCMD_ARG_TGT_ARCH%"=="arm" goto build

set "VCVARS="
for %%e in (12.0 14.0) do (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio %%e\VC\vcvarsall.bat" (
        set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio %%e\VC\vcvarsall.bat"
    )
    if exist "%ProgramFiles%\Microsoft Visual Studio %%e\VC\vcvarsall.bat" (
        set "VCVARS=%ProgramFiles%\Microsoft Visual Studio %%e\VC\vcvarsall.bat"
    )
)

if not defined VCVARS (
    echo [error] Visual Studio 2013 (or 2015) with the ARM tools was not found.
    echo         Install VS2013 + Windows SDK 8.1 and run this from the
    echo         "ARM Cross Tools Command Prompt".
    popd
    exit /b 1
)

echo [info] using %VCVARS%
call "%VCVARS%" x86_arm || (popd & exit /b 1)

:build
echo [info] building widi_sink.exe and widi_service.exe for ARM...
nmake /nologo /f build\winrt8\Makefile.RT
set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" (
    echo [error] build failed
    exit /b %RC%
)
echo [ok] build\winrt8\widi_sink.exe
mkdir build\winrt8 >nul 2>nul
exit /b 0
