@echo off
rem SurfaceRT-WiDi - ARM build entry point.
rem
rem The real logic lives in build_rt.ps1. cmd.exe treats a literal parenthesis
rem inside an if-block as the end of that block, which is exactly why the old
rem version failed with "... was unexpected at this time" on paths such as
rem "C:\Program Files (x86)\...". PowerShell has no such trap.
setlocal
where powershell.exe >nul 2>nul
if errorlevel 1 goto nowinps
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_rt.ps1" %*
exit /b %ERRORLEVEL%
:nowinps
echo [error] Windows PowerShell was not found. Run build_rt.ps1 manually.
exit /b 1
