@echo off
rem SurfaceRT-WiDi - one-command build for Windows.
rem
rem   build.cmd            build everything this machine can build, then run the
rem                        PC sender's self test
rem   build.cmd test       same, plus the full C++ test suite (23 cases)
rem   build.cmd pc         only the PC sender (in-box C# compiler, zero installs)
rem   build.cmd rt         only the Windows RT 8.1 ARM binaries (needs VS2013)
rem   build.cmd clean      remove build output
rem
rem Nothing is installed and nothing is downloaded: it uses whatever compiler
rem already exists on the machine (MSVC / Clang / MinGW) and, for the PC sender,
rem the C# compiler that ships inside Windows itself.
setlocal enabledelayedexpansion
set "ROOT=%~dp0"
pushd "%ROOT%" || exit /b 1

set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

if /i "%MODE%"=="clean" goto :clean
if /i "%MODE%"=="rt" goto :rt
if /i "%MODE%"=="pc" goto :pc

set "CC_KIND="
where cl.exe >nul 2>nul && set "CC_KIND=cl"
if not defined CC_KIND (
    where clang++.exe >nul 2>nul && set "CC_KIND=clang"
)
if not defined CC_KIND (
    where g++.exe >nul 2>nul && set "CC_KIND=mingw"
)

if defined CC_KIND (
    echo [info] C++ compiler: !CC_KIND!
    call :build_cpp
    if errorlevel 1 goto :fail
) else (
    echo [warn] no C++ compiler found - skipping the portable core and its tests.
    echo        Any one of these is enough: Visual Studio Build Tools ^(cl.exe^),
    echo        LLVM/Clang ^(clang++.exe^) or MinGW-w64 ^(g++.exe^).
    echo        The PC sender below still builds with the C# compiler Windows ships.
)

call :build_pc
if errorlevel 1 goto :fail

if /i "%MODE%"=="test" (
    if not exist "out\widi_tests.exe" (
        echo [warn] test suite was not built on this machine
    ) else (
        echo.
        echo [info] running the portable core test suite...
        out\widi_tests.exe
        if errorlevel 1 goto :fail
    )
)

echo.
echo [ok] build finished
if exist "out\widi_tests.exe"     echo      out\widi_tests.exe      portable core tests
echo      tools\pc\widi-pc.exe    PC sender ^(try: tools\pc\widi-pc.exe --list^)
echo      RT binaries: build.cmd rt   ^(needs Visual Studio 2013 + Windows SDK 8.1^)
popd
exit /b 0

rem --------------------------------------------------------------------------
:build_cpp
if not exist "out" mkdir "out"
pushd "out"
if /i "!CC_KIND!"=="cl" (
    cl /nologo /O2 /EHsc /MT /W3 /I..\include /Fe:widi_tests.exe ^
        ..\tests\test_main.cpp ^
        ..\src\core\protocol.cpp ..\src\core\session.cpp ^
        ..\src\core\rate_control.cpp ..\src\core\beacon.cpp ^
        ..\src\core\sink_engine.cpp ..\src\core\log.cpp ^
        ..\src\core\net_posix.cpp ..\src\platform\winrt8\net_win32.cpp ^
        ws2_32.lib iphlpapi.lib
) else (
    set "CXX=g++"
    if /i "!CC_KIND!"=="clang" set "CXX=clang++"
    !CXX! -std=c++17 -O2 -pthread -static -I..\include -o widi_tests.exe ^
        ..\tests\test_main.cpp ^
        ..\src\core\protocol.cpp ..\src\core\session.cpp ^
        ..\src\core\rate_control.cpp ..\src\core\beacon.cpp ^
        ..\src\core\sink_engine.cpp ..\src\core\log.cpp ^
        ..\src\core\net_posix.cpp ..\src\platform\winrt8\net_win32.cpp ^
        -lws2_32 -liphlpapi
)
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

rem --------------------------------------------------------------------------
:pc
call :build_pc
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

rem --------------------------------------------------------------------------
:build_pc
call "tools\pc\Build-Sender.cmd"
exit /b %ERRORLEVEL%

rem --------------------------------------------------------------------------
:rt
call "build\winrt8\build_rt.cmd"
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%

rem --------------------------------------------------------------------------
:clean
if exist "out" rd /s /q "out"
if exist "tools\pc\widi-pc.exe" del /q "tools\pc\widi-pc.exe"
if exist "build\winrt8\*.obj" del /q "build\winrt8\*.obj"
if exist "build\winrt8\*.exe" del /q "build\winrt8\*.exe"
echo [ok] cleaned
popd
exit /b 0

:fail
echo.
echo [error] build failed
popd
exit /b 1
