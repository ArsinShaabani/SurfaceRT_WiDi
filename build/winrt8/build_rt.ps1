# SurfaceRT-WiDi - build the Windows RT 8.1 (ARM) binaries.
#
# All detection logic lives here rather than in a .cmd file: cmd.exe treats a
# literal parenthesis inside an if-block as the end of the block, which is what
# produced "... was unexpected at this time" for paths such as
# "C:\Program Files (x86)\...". PowerShell has no such trap.
#
# Usage (from the repository root):
#   powershell -NoProfile -ExecutionPolicy Bypass -File build\winrt8\build_rt.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File build\winrt8\build_rt.ps1 -Clean
#
# Requirements: Visual Studio 2013 Update 5 or Visual Studio 2015 (Community is
# free) + Windows SDK 8.1 + the "ARM" cross tools. Nothing newer can target
# Windows RT.
[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $scriptDir)

# Be explicit when the layout is unexpected instead of failing on Push-Location.
if (-not $root -or -not (Test-Path -LiteralPath ($root + '\include\widi\version.h'))) {
    if (Test-Path -LiteralPath ($PWD.Path + '\include\widi\version.h')) { $root = $PWD.Path }
}
if (-not $root -or -not (Test-Path -LiteralPath ($root + '\build\winrt8\Makefile.RT'))) {
    Write-Host '[error] cannot locate the repository root.' -ForegroundColor Red
    Write-Host '        run this from the repository, or call it by full path:'
    Write-Host '          powershell -ExecutionPolicy Bypass -File <repo>\build\winrt8\build_rt.ps1'
    exit 1
}

$outDir = $root + '\build\winrt8'

function Write-Info([string]$text) { Write-Host "[info] $text" }
function Write-Warn2([string]$text) { Write-Host "[warn] $text" -ForegroundColor Yellow }
function Write-Error2([string]$text) { Write-Host "[error] $text" -ForegroundColor Red }

if ($Clean) {
    Get-ChildItem -Path $outDir -Filter '*.obj' -ErrorAction SilentlyContinue | Remove-Item -Force
    Get-ChildItem -Path $outDir -Filter '*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force
    Write-Info 'cleaned build\winrt8'
    return
}

Push-Location $root
try {
    $cmdLines = New-Object System.Collections.Generic.List[string]

    if ($env:VSCMD_ARG_TGT_ARCH -eq 'arm') {
        Write-Info 'already inside an ARM cross tools prompt'
    }
    else {
        $candidates = New-Object System.Collections.Generic.List[string]
        # Fall back to the well-known locations: some shells and services do not
        # export these.
        $pf86 = ${env:ProgramFiles(x86)}
        $pf = $env:ProgramFiles
        if (-not $pf86) { $pf86 = 'C:\Program Files (x86)' }
        if (-not $pf) { $pf = 'C:\Program Files' }
        if ($pf86) {
            $candidates.Add($pf86 + '\Microsoft Visual Studio 12.0\VC\vcvarsall.bat')
            $candidates.Add($pf86 + '\Microsoft Visual Studio 14.0\VC\vcvarsall.bat')
        }
        if ($pf) {
            $candidates.Add($pf + '\Microsoft Visual Studio 12.0\VC\vcvarsall.bat')
            $candidates.Add($pf + '\Microsoft Visual Studio 14.0\VC\vcvarsall.bat')
        }
        if ($env:VS120COMNTOOLS) {
            $candidates.Add($env:VS120COMNTOOLS + '..\..\VC\vcvarsall.bat')
        }
        if ($env:VS140COMNTOOLS) {
            $candidates.Add($env:VS140COMNTOOLS + '..\..\VC\vcvarsall.bat')
        }

        $vcvars = $null
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                $vcvars = $candidate
                break
            }
        }

        if (-not $vcvars) {
            Write-Error2 'No ARM-capable Visual Studio was found.'
            Write-Host ''
            Write-Host '        Building Windows RT binaries needs ALL of these:'
            Write-Host '          - Visual Studio 2013 Update 5, or Visual Studio 2015 Community'
            Write-Host '          - Windows SDK 8.1'
            Write-Host '          - the "ARM" cross tools, selected during setup'
            Write-Host ''
            Write-Host '        Search paths that were tried:'
            foreach ($candidate in $candidates) { Write-Host "          $candidate" }
            Write-Host ''
            Write-Host '        Newer Visual Studios cannot target Windows RT at all.'
            Write-Host '        Already in an "ARM Cross Tools Command Prompt"? Then run:'
            Write-Host '          nmake /nologo /f build\winrt8\Makefile.RT'
            Write-Host ''
            Write-Host '        You do not need this step to use the tablet: build and run the'
            Write-Host '        PC sender instead with   build.cmd pc   then tools\pc\widi-pc.exe'
            exit 1
        }

        Write-Info "using $vcvars"
        $cmdLines.Add('call "' + $vcvars + '" x86_arm')
        $cmdLines.Add('if errorlevel 1 exit /b 2')
        $cmdLines.Add('where nmake.exe >nul 2>nul')
        $cmdLines.Add('if errorlevel 1 exit /b 3')
    }

    $cmdLines.Add('nmake /nologo /f build\winrt8\Makefile.RT')
    $cmdLines.Add('exit /b %ERRORLEVEL%')

    # A generated script keeps quoting simple and avoids cmd parsing entirely.
    $tempScript = $env:TEMP + '\widi-build-' + [System.Guid]::NewGuid().ToString('N') + '.cmd'
    $body = "@echo off`r`n" + ($cmdLines -join "`r`n") + "`r`n"
    [System.IO.File]::WriteAllText($tempScript, $body, [System.Text.Encoding]::ASCII)

    Write-Info 'building widi_sink.exe and widi_service.exe for ARM...'
    try {
        & cmd.exe /c $tempScript
        $exitCode = $LASTEXITCODE
    }
    finally {
        Remove-Item -LiteralPath $tempScript -Force -ErrorAction SilentlyContinue
    }

    if ($exitCode -eq 2) {
        Write-Error2 'vcvarsall.bat x86_arm failed: the ARM tools are probably not installed.'
        exit 1
    }
    if ($exitCode -eq 3) {
        Write-Error2 'vcvarsall.bat ran but nmake.exe is still missing.'
        exit 1
    }
    if ($exitCode -ne 0) {
        Write-Error2 "build failed with exit code $exitCode (see the compiler output above)"
        exit $exitCode
    }

    $sink = $outDir + '\widi_sink.exe'
    $service = $outDir + '\widi_service.exe'
    if (-not (Test-Path -LiteralPath $sink)) {
        Write-Error2 'nmake reported success but widi_sink.exe is missing'
        exit 1
    }
    Write-Host ''
    Write-Info "[ok] $sink"
    Write-Info "[ok] $service"
    Write-Info 'next: install\Install-WiDiService.ps1 -SourceDir build\winrt8'
}
finally {
    Pop-Location
}
