# Building for Windows RT 8.1 (ARM)

## Why a special toolchain

Windows RT runs ARM binaries only. The last Microsoft toolchain that targets
ARM desktop applications for Windows RT 8.1 is:

- **Visual Studio 2013** (v120) with the ARM cross compiler, and
- **Windows SDK 8.1** (this is what provides `d2d1.lib`, `windowscodecs.lib`,
  `wtsapi32.lib`, ... for ARM).

VS2015 (v140) also still contains the `x86_arm` cross tools and works. Newer
Visual Studios no longer ship an RT-capable ARM desktop toolset.

## Build

```
cd <repo>
build\winrt8\build_rt.cmd
```

`build_rt.cmd` is a three-line wrapper around `build_rt.ps1`, which holds the
real logic. This is deliberate: **cmd.exe treats a literal parenthesis inside an
`if (...)` block as the end of that block**, so a path such as
`C:\Program Files (x86)\...` or an `echo` line containing `(or 2015)` breaks the
script with `... was unexpected at this time`. PowerShell has no such parsing
trap, so all detection, quoting and error handling live there.

If you prefer to call the PowerShell directly:

```
powershell -NoProfile -ExecutionPolicy Bypass -File build\winrt8\build_rt.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build\winrt8\build_rt.ps1 -Clean
```

The script locates `vcvarsall.bat`, switches to `x86_arm`, and calls
`nmake /f build\winrt8\Makefile.RT`. Output:

```
build\winrt8\widi_sink.exe       (the full screen receiver UI)
build\winrt8\widi_service.exe    (the service that keeps it running)
```

Manual equivalent, from an *ARM Cross Tools Command Prompt*:

```
call "%ProgramFiles(x86)%\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_arm
nmake /f build\winrt8\Makefile.RT
```

### Compile flags of note

| Flag | Why |
|---|---|
| `/MT` | static CRT - RT has no redistributable to install |
| `/O2` | Tegra 3 is the bottleneck; do not build this with `/Od` |
| `/D_WIN32_WINNT=0x0603` | Windows 8.1 - prevents accidental use of Win10-only APIs |
| `link /SUBSYSTEM:WINDOWS` | `widi_sink` uses `wWinMain` |
| `link /SUBSYSTEM:CONSOLE` | `widi_service` uses `wmain` (for `--install` output) |

## Verify the portable core first

90% of the logic is platform independent and can be validated on any Linux/macOS
box or on Windows with MinGW. Do this before every device build:

```
make test          # 23 test cases, includes real loopback sockets + fuzz feed
make               # builds the reference sink and a synthetic sender
```

End-to-end smoke test without a tablet:

```
./out/widi-sink-linux --port 47000 --save-every 10 --out /tmp &
./out/widi-sender-linux --host 127.0.0.1 --port 47000 --frames 60
python3 tools/dev/wire_check.py --port 47000     # byte-exact PC-side layout
```

## Running unsigned ARM binaries on Surface RT

Windows RT 8.1 enforces code integrity: only Microsoft-signed desktop binaries
run. Installing a *Store* app is not an option either - the Store for Windows 8.1
/ RT is retired and sideloading needs a developer licence that can no longer be
obtained.

The practical route (and the one this project targets) is the community
"Windows RT jailbreak" which disables the user-mode signing check so ARM desktop
EXEs run. Two important notes:

1. **This is at your own risk.** It modifies the boot/CI configuration of a
device that Microsoft no longer services. Understand what you are doing, and
   keep a recovery USB image.
2. **The jailbreak is only needed for code signing.** SurfaceRT-WiDi installs no
   drivers and touches nothing in the kernel; it is a user-mode app plus a
   normal Windows service.

After the jailbreak, run an elevated PowerShell (RT ships PowerShell 3.0):

```
.\install\Install-WiDiService.ps1 -SourceDir C:\widi\build\winrt8
```

See `DEPLOY-RT.md` for what the installer does and the `widi.ini` reference.

## Troubleshooting the build

| Symptom | Cause / fix |
|---|---|
| `nmake` not found | you are not in the ARM cross tools environment; use `build_rt.cmd` |
| `d2d1.lib` for ARM not found | Windows SDK 8.1 is not installed, or you are in an x86 prompt |
| `CL : fatal error C1060` | running both toolchains at once on a low-RAM host; close VS |
| `LNK2001: wWinMain` | you linked `sink_app.cpp` into the console subsystem target |
| `error LNK2038: RuntimeLibrary mismatch` | `/MT` vs `/MD` mix; keep the flags from the makefile |
| Device says "This app can't run on your PC" | code integrity is still on: the device is not jailbroken, or the binary is x86/x64 |
| `... was unexpected at this time` from a .cmd | a parenthesis inside an `if (...)` block (old versions of `build_rt.cmd`); use `build_rt.ps1` or update the repo |
| `No ARM-capable Visual Studio was found` | install VS2013 Update 5 or VS2015 Community **with** the ARM cross tools, plus Windows SDK 8.1. Newer Visual Studios cannot target Windows RT |

## Cross-checking a Windows build from Linux

`tools/dev/wire_check.py` re-implements the PC sender's byte layout in Python
and talks to the reference sink, so a mistake in the C# encoder or in the
protocol tables shows up long before it reaches the tablet.
