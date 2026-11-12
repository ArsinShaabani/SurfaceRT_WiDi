<#
.SYNOPSIS
  Streams this Windows 10/11 desktop to a Microsoft Surface RT running
  SurfaceRT-WiDi, without installing anything on the PC.

.DESCRIPTION
  The capture/encode helper (tools/pc/lib/WiDiNative.cs) is compiled into a
  temporary in-memory assembly by Add-Type, so no files, services or drivers are
  added to the system. Frames are sent as JPEG over TCP using the SurfaceRT-WiDi
  protocol v1; the tablet acknowledges with decode-time statistics and the
  script adapts quality/frame rate on the fly.

.EXAMPLE
  .\Send-Screen.ps1
  Finds the tablet on the LAN and starts streaming.

.EXAMPLE
  .\Send-Screen.ps1 -SinkHost 192.168.1.50 -Fps 24 -Quality 65

.EXAMPLE
  .\Send-Screen.ps1 -List
  Only lists the receivers found by UDP discovery.
#>
[CmdletBinding()]
param(
    [string]$SinkHost,
    [int]$Port = 47000,
    [int]$DiscoveryPort = 47001,
    [int]$Fps = 20,
    [int]$Quality = 60,
    [int]$MaxWidth = 1366,
    [switch]$NoInput,
    [switch]$List,
    [int]$DurationSeconds = 0
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$BeaconMagic = @(0x57, 0x49, 0x44, 0x49, 0x44, 0x53)  # "WIDIDS"

function Write-Widi([string]$Message, [string]$Color = 'Cyan') {
    Write-Host ("[widi] " + $Message) -ForegroundColor $Color
}

function Import-WidiHelper {
    $helper = Join-Path $PSScriptRoot 'lib\WiDiNative.cs'
    if (-not (Test-Path $helper)) { throw "missing helper: $helper" }
    # -TypeDefinition (not -Path) so System.Drawing can be referenced explicitly.
    $source = Get-Content -Path $helper -Raw
    Add-Type -TypeDefinition $source -Language CSharp -ReferencedAssemblies 'System.Drawing'
    try { [void][SurfaceRTWiDi.ScreenCapture]::SetProcessDPIAware() } catch { }
}

function Find-WidiSink {
    param([int]$TimeoutMs = 3000, [int]$DiscoveryPort = 47001)

    $udp = New-Object System.Net.Sockets.UdpClient
    $udp.Client.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket,
                                [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
    $udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $DiscoveryPort)))
    $udp.Client.ReceiveTimeout = 400

    $found = @()
    $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try { $data = $udp.Receive([ref]$remote) } catch { continue }
        if ($null -eq $data -or $data.Length -lt 15) { continue }
        $isBeacon = $true
        for ($i = 0; $i -lt $BeaconMagic.Count; $i++) {
            if ($data[$i] -ne $BeaconMagic[$i]) { $isBeacon = $false; break }
        }
        if (-not $isBeacon) { continue }

        $tcpPort = [BitConverter]::ToUInt16($data, 7)
        $screenW = [BitConverter]::ToUInt16($data, 9)
        $screenH = [BitConverter]::ToUInt16($data, 11)
        $nameLen = [int]$data[14]
        $name = 'Surface RT'
        if ($nameLen -gt 0 -and $data.Length -ge (15 + $nameLen)) {
            $name = [System.Text.Encoding]::UTF8.GetString($data, 15, $nameLen)
        }
        $address = $remote.Address.ToString()
        $already = $false
        foreach ($item in $found) { if ($item.Address -eq $address -and $item.TcpPort -eq $tcpPort) { $already = $true } }
        if (-not $already) {
            $found += New-Object PSObject -Property @{
                Address = $address; TcpPort = $tcpPort; Name = $name
                Width = $screenW; Height = $screenH
            }
        }
    }
    $udp.Close()
    return $found
}

function Read-Exactly {
    param([System.IO.Stream]$Stream, [int]$Count)
    $buffer = New-Object byte[] $Count
    $read = 0
    while ($read -lt $Count) {
        $n = $Stream.Read($buffer, $read, $Count - $read)
        if ($n -le 0) { throw 'the tablet closed the connection' }
        $read += $n
    }
    # The unary comma stops PowerShell from unrolling the byte array.
    return ,$buffer
}

function Receive-WidiMessage {
    param([System.IO.Stream]$Stream, [int]$TimeoutMs = 2000)
    $Stream.ReadTimeout = $TimeoutMs
    $header = Read-Exactly -Stream $Stream -Count 20
    $type = $header[5]
    $length = [int][BitConverter]::ToUInt32($header, 12)
    [byte[]]$payload = New-Object byte[] 0
    if ($length -gt 0) { [byte[]]$payload = Read-Exactly -Stream $Stream -Count $length }
    return New-Object PSObject -Property @{ Type = $type; Payload = $payload }
}

Import-WidiHelper

$tabletScreen = $null
if ($List) {
    Write-Widi 'searching for Surface RT receivers (3 s)...'
    $sinks = Find-WidiSink -DiscoveryPort $DiscoveryPort
    if (-not $sinks -or $sinks.Count -eq 0) {
        Write-Widi 'no receiver answered. Start SurfaceRT-WiDi on the tablet, or use -SinkHost <ip>.' 'Yellow'
        exit 1
    }
    foreach ($sink in $sinks) {
        Write-Host ("{0,-16} tcp/{1,-6} {2} ({3}x{4})" -f $sink.Address, $sink.TcpPort,
                                                  $sink.Name, $sink.Width, $sink.Height)
    }
    exit 0
}

if (-not $SinkHost) {
    Write-Widi 'looking for a receiver on the LAN...'
    $sinks = Find-WidiSink -DiscoveryPort $DiscoveryPort
    if (-not $sinks -or $sinks.Count -eq 0) {
        Write-Widi 'no receiver found. Start SurfaceRT-WiDi on the tablet, or pass -SinkHost <ip>.' 'Yellow'
        exit 1
    }
    $SinkHost = $sinks[0].Address
    $Port = [int]$sinks[0].TcpPort
    $tabletScreen = $sinks[0]
    Write-Widi ("using {0} ({1})" -f $SinkHost, $sinks[0].Name)
}

$client = New-Object System.Net.Sockets.TcpClient
$client.NoDelay = $true
$stream = $null
$capture = $null
$exitCode = 0

try {
    Write-Widi ("connecting to {0}:{1}..." -f $SinkHost, $Port)
    $async = $client.BeginConnect($SinkHost, $Port, $null, $null)
    if (-not $async.AsyncWaitHandle.WaitOne(5000)) { throw "timeout connecting to $SinkHost`:$Port" }
    $client.EndConnect($async)
    $stream = $client.GetStream()

    $hello = [SurfaceRTWiDi.Wire]::Hello($env:COMPUTERNAME, 1920, 1080, $Fps, [byte]0x07)
    $stream.Write($hello, 0, $hello.Length)
    $ack = Receive-WidiMessage -Stream $stream -TimeoutMs 5000
    if ($ack.Type -ne 3) { throw "unexpected handshake reply (type $($ack.Type))" }
    $accepted = $ack.Payload[10] -eq 1
    $sinkW = [BitConverter]::ToUInt16($ack.Payload, 2)
    $sinkH = [BitConverter]::ToUInt16($ack.Payload, 4)
    $nameLength = [int]$ack.Payload[12]
    $sinkName = 'Surface RT'
    if ($nameLength -gt 0 -and $ack.Payload.Length -ge (13 + $nameLength)) {
        $sinkName = [System.Text.Encoding]::UTF8.GetString($ack.Payload, 13, $nameLength)
    }
    if (-not $accepted) { throw "the tablet rejected the session (reason $($ack.Payload[11]))" }
    Write-Widi ("connected to '{0}' ({1}x{2})" -f $sinkName, $sinkW, $sinkH)

    $capture = New-Object SurfaceRTWiDi.ScreenCapture
    $targetWidth = [Math]::Min($MaxWidth, $capture.SourceWidth)
    if ($sinkW -gt 0 -and $sinkW -lt $targetWidth) { $targetWidth = $sinkW }

    $quality = $Quality
    $fps = $Fps
    $frameMs = 1000.0 / $fps
    $seq = 10
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $nextDue = 0.0
    $lastReport = 0
    $sent = 0
    $stats = $null

    Write-Widi ("streaming {0}x{1} -> {2}px @ {3} fps, quality {4}. Press Ctrl+C to stop." -f
                $capture.SourceWidth, $capture.SourceHeight, $targetWidth, $fps, $quality)

    while ($true) {
        if ($DurationSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $DurationSeconds) { break }

        $jpeg = $capture.CaptureFrame($targetWidth, $quality)
        $frameWidth = $capture.LastWidth
        $frameHeight = $capture.LastHeight
        $stamp = [uint32]($clock.ElapsedMilliseconds -band 0x7FFFFFFF)
        $message = [SurfaceRTWiDi.Wire]::Frame([uint32]$seq, $frameWidth, $frameHeight, [byte]1,
                                             $stamp, $jpeg, $jpeg.Length)
        $seq++
        $stream.Write($message, 0, $message.Length)
        $sent++

        # Drain feedback without stalling the pipeline.
        $guard = 0
        while ($guard -lt 8) {
            try {
                if ($client.Available -lt 20) { break }
                $incoming = Receive-WidiMessage -Stream $stream -TimeoutMs 400
            } catch { $incoming = $null }
            if ($null -eq $incoming) { break }
            $guard++

            if ($incoming.Type -eq 5) {
                $stats = New-Object PSObject -Property @{
                    Decoded  = [BitConverter]::ToUInt32($incoming.Payload, 0)
                    Dropped  = [BitConverter]::ToUInt32($incoming.Payload, 4)
                    DecodeMs = [BitConverter]::ToUInt16($incoming.Payload, 8) / 16.0
                    Fps      = [BitConverter]::ToUInt16($incoming.Payload, 10) / 16.0
                }
            } elseif ($incoming.Type -eq 6) {
                if (-not $NoInput) {
                    $kind = [int]$incoming.Payload[0]
                    $flags = [int]$incoming.Payload[1]
                    $px = [BitConverter]::ToUInt16($incoming.Payload, 2)
                    $py = [BitConverter]::ToUInt16($incoming.Payload, 4)
                    if ($kind -eq 2) {
                        [SurfaceRTWiDi.Pointer]::Wheel([BitConverter]::ToInt32($incoming.Payload, 6))
                    } elseif ($kind -eq 0 -or $kind -eq 1) {
                        $down = ($flags -band 8) -ne 0
                        $right = ($flags -band 2) -ne 0
                        if ($down) { [SurfaceRTWiDi.Pointer]::Button($px, $py, $true, $right) }
                        elseif (($flags -band 0x10) -ne 0) { [SurfaceRTWiDi.Pointer]::Button($px, $py, $false, $right) }
                        else { [SurfaceRTWiDi.Pointer]::Move($px, $py) }
                    }
                }
            } elseif ($incoming.Type -eq 9) {
                Write-Widi 'the tablet closed the session' 'Yellow'
                $sent = -1
                break
            }
        }
        if ($sent -lt 0) { break }

        # Once per second: report and adapt.
        if (($clock.ElapsedMilliseconds - $lastReport) -ge 1000) {
            $lastReport = [int]$clock.ElapsedMilliseconds
            if ($stats) {
                Write-Widi ("{0} fps sent | tablet {1:0.0} fps, {2:0.0} ms decode, {3} dropped | q{4} {5}px" -f
                            $sent, $stats.Fps, $stats.DecodeMs, $stats.Dropped, $quality, $targetWidth)
                $budget = (1000.0 / $fps) * 0.7
                if ($stats.DecodeMs -gt $budget) {
                    if ($quality -gt 32) { $quality -= 8 }
                    elseif ($fps -gt 12) { $fps -= 2; $frameMs = 1000.0 / $fps }
                    elseif ($targetWidth -gt 960) { $targetWidth -= 120 }
                } elseif ($stats.DecodeMs -lt ($budget / 2.0)) {
                    if ($quality -lt $Quality) { $quality += 3 }
                    elseif ($fps -lt $Fps) { $fps += 1; $frameMs = 1000.0 / $fps }
                    elseif ($targetWidth -lt $MaxWidth) { $targetWidth += 80 }
                }
                $stats = $null
            } else {
                Write-Widi ("{0} fps sent (no feedback from the tablet yet)" -f $sent)
            }
        }

        $nextDue += $frameMs
        $wait = $nextDue - $clock.ElapsedMilliseconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($wait)) }
        else { $nextDue = [double]$clock.ElapsedMilliseconds }
    }
}
catch {
    Write-Widi ("error: " + $_.Exception.Message) 'Red'
    $exitCode = 1
}
finally {
    if ($stream) {
        try {
            $bye = [SurfaceRTWiDi.Wire]::Bye()
            $stream.Write($bye, 0, $bye.Length)
            $stream.Flush()
        } catch { }
    }
    if ($capture) { $capture.Dispose() }
    if ($client) { $client.Close() }
    Write-Widi 'stopped'
}
if ($exitCode -eq 1) { exit 1 }
