# Using the PC side (Windows 10/11, nothing installed)

Two senders are available and share the same protocol implementation:

| Sender | How you get it | When to use it |
|---|---|---|
| `tools\pc\widi-pc.exe` | `tools\pc\Build-Sender.cmd` (one line, Windows' own C# compiler) | recommended: no PowerShell policy issues, starts instantly |
| `tools\pc\Send-Screen.ps1` | nothing to build | script-only machines, or if you want to read/patch the logic quickly |

```
tools\pc\Build-Sender.cmd
.\tools\pc\widi-pc.exe --selftest     # protocol verification, no tablet needed
.\tools\pc\widi-pc.exe --list
.\tools\pc\widi-pc.exe --host 192.168.1.50 --fps 24 --quality 65
```

The executable accepts the same options as the script (`--host`, `--port`,
`--discovery-port`, `--fps`, `--quality`, `--max-width`, `--no-input`,
`--seconds`, `--list`, `--selftest`, `--help`).

```powershell
# 1. find the tablet and stream to it
.\tools\pc\Send-Screen.ps1

# 2. or pick it explicitly / tune it
.\tools\pc\Send-Screen.ps1 -SinkHost 192.168.1.50 -Fps 24 -Quality 65 -MaxWidth 1280

# 3. only list receivers
.\tools\pc\Send-Screen.ps1 -List

# 4. mirror with touch input disabled
.\tools\pc\Send-Screen.ps1 -NoInput

# 5. fixed run (useful for testing)
.\tools\pc\Send-Screen.ps1 -DurationSeconds 30
```

If script execution is blocked by policy, use one of these for the current
process only (no permanent change):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\pc\Send-Screen.ps1
```

## What "nothing installed" means here

- The only artifact is `tools/pc/lib/WiDiNative.cs` (source). At start,
  `Add-Type -TypeDefinition` compiles it **in memory** with the .NET Framework
  compiler that ships with Windows. No DLL is written to disk, no service, no
  driver, no registry key, no scheduled task.
- Everything else uses inbox Windows components: GDI capture (`BitBlt`),
  `System.Drawing` JPEG encoding, `System.Net.Sockets` transport, `mouse_event`
  for input injection.
- Nothing runs after you press Ctrl+C: the dynamic assembly and the process go
  away.

## Parameters

| Parameter | Default | Meaning |
|---|---|---|
| `-SinkHost` | auto-discover | tablet IP address |
| `-Port` | 47000 | receiver TCP port (from the beacon) |
| `-DiscoveryPort` | 47001 | UDP discovery port |
| `-Fps` | 20 | maximum frame rate the adaptation may use |
| `-Quality` | 60 | maximum JPEG quality (20-90) |
| `-MaxWidth` | 1366 | capture is scaled down to this width (also clamped to the tablet panel) |
| `-NoInput` | off | do not inject pointer events coming from the tablet |
| `-List` | off | only print discovered receivers |
| `-DurationSeconds` | 0 | 0 = run until Ctrl+C |

## What you should see

```
[widi] looking for a receiver on the LAN...
[widi] using 192.168.1.50 (Surface RT)
[widi] connecting to 192.168.1.50:47000...
[widi] connected to 'Surface RT' (1366x768)
[widi] streaming 1920x1080 -> 1366px @ 20 fps, quality 60. Press Ctrl+C to stop.
[widi] 20 fps sent | tablet 19.4 fps, 24.0 ms decode, 0 dropped | q60 1366px
```

The second number is the tablet's own measurement; the script lowers quality,
then frame rate, then resolution when the tablet cannot keep up, and raises them
again when there is headroom.

## Why not `Win+K`?

`Win+K` (Connect) only lists **Miracast/Wi-Fi Direct** sinks, which Windows RT
cannot be - see `FEASIBILITY.md` for the documented reasons (IHV-only x86 API,
no P2P beacon control, MS-MICE still needs the P2P beacon). If you want the "one
click in the Connect flyout" experience on a different device, that device must
be a real WFD sink (an Android TV box, a Miracast dongle, or a Win10/11 PC).

## Troubleshooting

| Symptom | Fix |
|---|---|
| `no receiver found` | tablet app not running/logged out, wrong subnet, AP isolation, or firewall |
| `timeout connecting` | the beacon port differs from `-Port` (check the HUD on the tablet) |
| Video appears but latency grows | a router is buffering (bufferbloat); lower `-MaxWidth`/`-Fps` |
| `Add-Type` fails | Windows PowerShell 3.0+ and .NET Framework 4.5+ are required (both are inbox on Win10/11) |
| Touch does nothing | the PC cannot inject input into an elevated window unless the script itself runs elevated; also check `-NoInput` |
| Script execution blocked | `powershell -ExecutionPolicy Bypass -File ...` |
| Multi-monitor shows the wrong area | capture uses the whole virtual desktop; disconnect unused monitors or raise `-MaxWidth` |

## Security notes

The protocol is unencrypted and unauthenticated (v1), so treat it as a
living-room convenience on a network you trust. Do not expose TCP 47000 to the
internet, and prefer WPA2/WPA3 with a device-isolated guest network if you want
to be strict. A TLS/DTLS layer is on the roadmap.
