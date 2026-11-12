<#
.SYNOPSIS
  Installs the SurfaceRT-WiDi wireless display receiver on a Surface RT
  (Windows RT 8.1, ARM) and registers it as an auto-starting Windows service.

.DESCRIPTION
  Copies the binaries to %ProgramData%\SurfaceRT-WiDi, writes a default
  widi.ini, opens the firewall for TCP 47000 / UDP 47001 and registers the
  SurfaceRTWiDi service. Windows RT 8.1 only runs unsigned desktop binaries
  after the community jailbreak, so run this from an elevated PowerShell on a
  jailbroken device.

.EXAMPLE
  .\Install-WiDiService.ps1 -SourceDir C:\widi\build\winrt8

.EXAMPLE
  .\Install-WiDiService.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$SourceDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\winrt8'),
    [int]$Port = 47000,
    [int]$DiscoveryPort = 47001,
    [string]$Name = 'Surface RT',
    [switch]$Uninstall,
    [switch]$SkipFirewall
)

$ErrorActionPreference = 'Stop'
$destination = Join-Path $env:ProgramData 'SurfaceRT-WiDi'
$serviceName = 'SurfaceRTWiDi'
$sinkExe = Join-Path $destination 'widi_sink.exe'
$serviceExe = Join-Path $destination 'widi_service.exe'

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Remove-FirewallRules {
    foreach ($rule in @('SurfaceRT-WiDi TCP', 'SurfaceRT-WiDi UDP')) {
        & netsh advfirewall firewall delete rule name="$rule" | Out-Null
    }
}

if (-not (Test-Administrator)) {
    throw 'Run this script from an elevated PowerShell (Run as Administrator).'
}

if ($Uninstall) {
    Write-Host 'Stopping service...'
    & sc.exe stop $serviceName 2>$null | Out-Null
    Start-Sleep -Seconds 2
    if (Test-Path $serviceExe) { & $serviceExe --uninstall | Out-Null }
    else { & sc.exe delete $serviceName 2>$null | Out-Null }
    Remove-FirewallRules
    if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
    Write-Host 'SurfaceRT-WiDi removed.'
    return
}

if (-not (Test-Path (Join-Path $SourceDir 'widi_sink.exe'))) {
    throw "widi_sink.exe not found in '$SourceDir'. Build it first (see docs/BUILD-RT.md)."
}

if (-not (Test-Path $destination)) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
}

Write-Host "Copying binaries to $destination"
Copy-Item (Join-Path $SourceDir 'widi_sink.exe') $sinkExe -Force
Copy-Item (Join-Path $SourceDir 'widi_service.exe') $serviceExe -Force

$iniPath = Join-Path $destination 'widi.ini'
if (-not (Test-Path $iniPath)) {
    $ini = @"
[sink]
Port=$Port
DiscoveryPort=$DiscoveryPort
Name=$Name
ForwardInput=1
BroadcastBeacons=1
Windowed=0
"@
    Set-Content -Path $iniPath -Value $ini -Encoding ASCII
    Write-Host "Wrote $iniPath"
} else {
    Write-Host "Keeping existing $iniPath"
}

if (-not $SkipFirewall) {
    Write-Host 'Adding firewall rules...'
    Remove-FirewallRules
    & netsh advfirewall firewall add rule name="SurfaceRT-WiDi TCP" dir=in action=allow protocol=TCP localport=$Port profile=any | Out-Null
    & netsh advfirewall firewall add rule name="SurfaceRT-WiDi UDP" dir=in action=allow protocol=UDP localport=$DiscoveryPort profile=any | Out-Null
}

Write-Host 'Registering service...'
& $serviceExe --uninstall 2>$null | Out-Null
& $serviceExe --install
if ($LASTEXITCODE -ne 0) { throw 'service registration failed' }
& sc.exe config $serviceName start= auto | Out-Null
& sc.exe start $serviceName | Out-Null

Start-Sleep -Seconds 2
$status = & sc.exe query $serviceName
Write-Host $status
Write-Host ''
Write-Host 'Done. On the Windows 10/11 PC run:'
Write-Host '  .\tools\pc\Send-Screen.ps1'
Write-Host "or connect manually to this tablet's IP on TCP $Port."
Write-Host 'Note: Windows RT requires an interactive logon before the receiver can draw.'
