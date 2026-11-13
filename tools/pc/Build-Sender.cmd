@echo off
rem SurfaceRT-WiDi - builds tools\pc\widi-pc.exe with the C# compiler that is
rem already part of Windows (.NET Framework 4.x). No SDK, no Visual Studio, no
rem downloads. One command:
rem
rem   tools\pc\Build-Sender.cmd
rem
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo [error] csc.exe was not found. Windows 10/11 ships it with .NET Framework 4.x;
    echo         if it is missing, enable ".NET Framework 4.8" in Windows Features.
    exit /b 1
)

"%CSC%" /nologo /optimize+ /target:exe /platform:anycpu /out:"%~dp0widi-pc.exe" ^
    /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll ^
    "%~dp0lib\WiDiNative.cs" "%~dp0src\WiDiSender.cs"
if errorlevel 1 (
    echo [error] compilation failed
    exit /b 1
)

echo [ok] %~dp0widi-pc.exe
echo.
echo [info] running the sender self test...
"%~dp0widi-pc.exe" --selftest
exit /b %ERRORLEVEL%
