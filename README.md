# SurfaceRT-WiDi

**Turn a Microsoft Surface RT (Windows RT 8.1, Tegra 3) into a wireless second
screen for a Windows 10/11 PC - with nothing installed on the PC.**

[فارسی](README.fa.md)

---

## Start here: what is (and is not) possible

Windows 10/11's own `Win+K` -> *Connect* only lists **Miracast** receivers, and a
Miracast sink is a **Wi-Fi Direct peer**. On Windows RT 8.1 that role is not
available to any application:

- Microsoft exposed "Miracast Receive" only as an **IHV/OEM Wi-Fi driver API set
  for x86/x64** applications (August 2014 update) - ARM/RT was never included.
- Even *Miracast over Infrastructure* (MS-MICE, TCP 7250) still begins with
  **Wi-Fi Direct beacons/probe responses carrying WSC vendor IEs**, which require
  raw 802.11 control that RT gives to no app.

Full evidence and citations: **[docs/FEASIBILITY.md](docs/FEASIBILITY.md)**.

So this project does the next best thing - and does it properly:

> A complete, tested **wireless display receiver** that runs on the Surface RT as
a Windows service, plus a **zero-install** sender for Windows 10/11 that uses
only inbox components (GDI capture, `System.Drawing` JPEG, sockets). ~40-85 ms
end-to-end, 12-20 fps at 1366x768, adaptive, with touch-to-cursor remoting.

## Features

- **Tablet side:** full-screen receiver, auto-start Windows service, session-aware
  relaunch, crash recovery, keep-screen-awake, discovery beacons, HUD, log files.
- **PC side:** one PowerShell script; the helper class is compiled **in memory**
  by `Add-Type` - no install, no registry, no leftover files.
- **Adaptive streaming:** the tablet reports decode time and drops; the sender
  lowers frame rate, then scale, then JPEG quality, and climbs back up.
- **Robust framing:** CRC32 per message, one-byte resync on garbage, bounded
  buffers, latest-frame-wins pacing so latency never grows.
- **Touch input:** tap/drag on the tablet moves the PC pointer (multi-monitor
  aware, `MOUSEEVENTF_VIRTUALDESK`).
- **Tested core:** 23 test cases / ~4,200 assertions (clean under ASan+UBSan),
  including real TCP loopback streaming, a fuzz feed and a
  byte-exact cross-language validation of the PC-side encoder.

## Quick start

### 0. One command (no IDE, no SDK, no installs)

```
build.cmd            # Windows: detects any compiler, always builds the PC sender
build.cmd test       # ... plus the full C++ test suite
./build.sh test      # Linux / macOS / WSL
```

On a bare Windows box with no C++ compiler at all, `build.cmd` still produces
`tools\pc\widi-pc.exe`, because it uses the C# compiler that .NET Framework
already puts in `%WINDIR%\Microsoft.NET`. That single-command path is described
in **[docs/BUILD-ONELINE.md](docs/BUILD-ONELINE.md)**.

### 1. Tablet (Surface RT, jailbroken - see [BUILD-RT](docs/BUILD-RT.md))

```
build\winrt8\build_rt.cmd
.\install\Install-WiDiService.ps1 -SourceDir C:\widi\build\winrt8
```

The service starts the receiver immediately and after every boot/logon.

### 2. PC (Windows 10/11)

Either the compiled sender (recommended):

```
tools\pc\Build-Sender.cmd              # one line, uses Windows' own C# compiler
.\tools\pc\widi-pc.exe                 # auto-discovers and streams
.\tools\pc\widi-pc.exe --list          # show receivers on the LAN
.\tools\pc\widi-pc.exe --selftest      # verify the protocol without a tablet
```

or the PowerShell sender (also zero-install, no build step):

```powershell
.\tools\pc\Send-Screen.ps1              # auto-discovers the tablet
.\tools\pc\Send-Screen.ps1 -List        # just show receivers on the LAN
```

Press `Ctrl+C` to stop. Nothing remains on the PC.

### 3. Develop / verify without any Windows machine

```bash
make test        # 23 test cases, ~4,200 assertions
make             # reference sink + synthetic sender
./out/widi-sink-linux --port 47000 &
./out/widi-sender-linux --host 127.0.0.1 --port 47000 --frames 60
python3 tools/dev/wire_check.py --port 47000   # validates the C# wire layout
```

## Repository layout

```
build.cmd / build.sh     one-command build (auto-detects any available compiler)
include/widi/            portable core headers (protocol, session, engine, net, log)
src/core/                portable implementation - unit tested on Linux
src/platform/winrt8/     WIC decoder, Direct2D/GDI presenter, sink app, service, Winsock
src/apps/console/        Linux reference sink + synthetic sender
tests/                   23 test cases / ~4,200 assertions for the core
build/winrt8/            nmake build for Windows RT 8.1 / ARM (VS2013 + SDK 8.1)
install/                 on-device installer/uninstaller (PowerShell)
tools/pc/                zero-install PC sender: widi-pc.exe (built by csc) + PowerShell
tools/pc/src/            C# sender program compiled by the in-box .NET compiler
tools/dev/               cross-language wire check
docs/                    feasibility, architecture, protocol, build, deploy, roadmap
```

## Documentation

| Document | Contents |
|---|---|
| [docs/FEASIBILITY.md](docs/FEASIBILITY.md) | Why a native Miracast sink is impossible on RT 8.1, with sources |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, threading, latency budget, design decisions |
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | Wire format v1, state machine, input semantics |
| [docs/BUILD-RT.md](docs/BUILD-RT.md) | ARM toolchain, jailbreak, build, troubleshooting |
| [docs/DEPLOY-RT.md](docs/DEPLOY-RT.md) | Installer, `widi.ini`, daily use, network tuning |
| [docs/BUILD-ONELINE.md](docs/BUILD-ONELINE.md) | One command build on Windows/Linux, no IDE |
| [docs/PC-SENDER.md](docs/PC-SENDER.md) | PC side usage, parameters, what "no install" means |
| [docs/ROADMAP.md](docs/ROADMAP.md) | H.264 hardware path, tiling, audio, DTLS |

## Honest status

| Item | State |
|---|---|
| Portable core (protocol, session, engine, adaptation) | built and tested on Linux: 23 test cases, 0 failures, ASan/UBSan clean |
| Reference sink + synthetic sender, end-to-end over TCP | verified (24/24 frames, 0 dropped) |
| PC sender wire layout | verified byte-for-byte by `tools/dev/wire_check.py` |
| PC sender compiles with the in-box C# 5 compiler | verified: `csc` against .NET Framework 4.8 reference assemblies, 0 errors |
| `widi-pc.exe --selftest` | verified: 32/32 checks pass |
| Real C# sender handshake against the reference sink | verified (Hello/HelloAck/Bye accepted, sink logs the peer name and geometry) |
| Windows RT binaries (`widi_sink.exe`, `widi_service.exe`) | source complete; must be compiled with VS2013+SDK 8.1 for ARM and validated on hardware |
| Real Surface RT hardware test | not performed in this repository environment (no device available) |

That last row matters: the Windows-specific code is written against documented
Windows 8.1 APIs and reviewed for RT-availability, but it has not been executed on
a physical Surface RT here. `docs/BUILD-RT.md` lists the exact toolchain and the
checklist to run on the device.

## License

MIT - see [LICENSE](LICENSE).
