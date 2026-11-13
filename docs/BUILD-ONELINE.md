# One command build (no IDE, no SDK, no installs)

Two entry points exist, one per platform. Both are wrappers that detect what is
already on the machine instead of demanding a specific toolchain.

## Windows

```
build.cmd            build everything this machine can build + run the PC self test
build.cmd test       same, plus the full C++ test suite (22 cases)
build.cmd pc         only tools\pc\widi-pc.exe  (zero installs, in-box compiler)
build.cmd rt         only the Windows RT 8.1 ARM binaries (needs VS2013 + SDK 8.1)
build.cmd clean      delete build output
```

### What `build.cmd` finds by itself

| Detected | What gets built |
|---|---|
| `cl.exe` (Visual Studio / Build Tools) | `out\widi_tests.exe` - the whole portable core test suite |
| `clang++.exe` or `g++.exe` (LLVM, MinGW-w64, MSYS2) | same test suite, statically linked |
| nothing at all | still builds `tools\pc\widi-pc.exe` and runs its self test |
| VS2013 + Windows SDK 8.1 (only for `build.cmd rt`) | `widi_sink.exe`, `widi_service.exe` for ARM |

### The zero-install path (this is the important one)

Windows 10/11 ships a C# compiler inside .NET Framework:

```
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
```

That is why the PC sender compiles with **one command and nothing to install**:

```
tools\pc\Build-Sender.cmd
```

which is exactly:

```
csc.exe /nologo /optimize+ /target:exe /out:widi-pc.exe ^
        /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll ^
        tools\pc\lib\WiDiNative.cs tools\pc\src\WiDiSender.cs
```

It finishes by running `widi-pc.exe --selftest`, which verifies the wire encoder
(CRC-32 vector, message offsets, Hello/HelloAck/beacon layouts) before a tablet
is even involved. Expected tail:

```
  PASS  crc32 vector
  PASS  frame crc covers payload only
  ...
self test passed - the sender speaks protocol v1 correctly
```

### Do I need a decompiler?

No - there is nothing opaque to decompile. This repository contains **only
source**: `widi-pc.exe` is compiled from the two `.cs` files above on your own
machine, `widi_tests.exe` from the C++ sources in `src/`, and the RT binaries
from the same sources with `build.cmd rt`. Nothing is shipped as a binary blob,
so you can read, audit and rebuild every artifact with the commands above.

## Linux / macOS / WSL

```
./build.sh            build the reference sink, synthetic sender and tests
./build.sh test       build and run the portable core test suite
./build.sh clean      remove build output
```

Needs only `g++` (or `clang++`) and `make`. `make` directly also works.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `'build.cmd' is not recognized` | run it as `build.cmd` from the repo root, or `cmd /c build.cmd` |
| `csc.exe was not found` | enable ".NET Framework 4.8" in Windows Features (it is on by default) |
| `no C++ compiler found` | expected on a bare machine; the PC sender still builds. Install VS Build Tools, LLVM or MinGW-w64 if you also want the C++ tests |
| `cl.exe` present but `windows.h` not found | use the *Developer Command Prompt*, not the plain one, or run `build.cmd` after `vcvarsall.bat` |
| `clang++.exe` links against MSVC but fails | its `link.exe` is missing: install VS Build Tools, or use MinGW-w64 `g++.exe` |
| Antivirus blocks `widi-pc.exe` | it is a freshly compiled local binary; add an exclusion or use `Send-Screen.ps1` instead |
| `build.cmd rt` fails | ARM binaries need VS2013 Update 5 / VS2015 Community + Windows SDK 8.1 with the ARM tools - see `BUILD-RT.md` |
| `... was unexpected at this time` | that is cmd.exe choking on a parenthesis inside an `if` block; `build.cmd rt` and `build\winrt8\build_rt.cmd` now only wrap `build_rt.ps1`, which has no such trap. Update the repo, or run the `.ps1` directly |

## خلاصهٔ فارسی

- در ویندوز فقط `build.cmd` را اجرا کنید. خودش کامپایلر موجود را پیدا می‌کند.
- اگر **هیچ** کامپایلری نصب نباشد، باز هم `tools\pc\widi-pc.exe` ساخته می‌شود،
  چون از کامپایلر C# که داخل خود ویندوز است (`csc.exe` در .NET Framework)
  استفاده می‌کند — نه نصب، نه دانلود، نه ویژوال استودیو.
- برای ساخت فقط فرستندهٔ کامپیوتر با یک دستور: `tools\pc\Build-Sender.cmd`
- در پایان خودکار `--selftest` اجرا می‌شود و صحت پروتکل را قبل از اتصال به تبلت
  بررسی می‌کند.
- باینری‌های ویندوز RT (ARM) همچنان به Visual Studio 2013 + SDK 8.1 نیاز دارند؛
  این محدودیت خود ویندوز RT است، نه انتخاب پروژه (`docs/BUILD-RT.md`).
- نیازی به «دیکامپایلر» نیست: کل پروژه سورس است و همین دستورها همهٔ فایل‌های خروجی
  را از همان سورس می‌سازند.
