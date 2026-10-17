# Build Windows

Windows **cannot** be compiled from this machine (Linux without an mpv/Qt
mingw toolchain — only `wine` to run, not to compile). The project does NOT
keep its own `.exe`: the final build is done on your Windows machine and this
document is the exact list of what it needs. The question of "compiling on
Windows" was **closed in the CMake as a seam**: in the `if(WIN32)` branch it
uses `find_package(unofficial-mpv CONFIG)` instead of `pkg_check_modules(mpv)`,
and the rest stays the same (Qt5, OpenGL, SQLite). This section only runs if
`WIN32`; on Linux/Unix the build stays **identical** (no regression).

## Requirements

1. **Qt 5.15.x** (MSVC 2019 or MinGW) → in the end `windeployqt` takes care of the Qt DLLs.
   **Vendored, do not install Qt.** `third_party/qt-win32` (x86/Win7) and
   `third_party/qt-win64` (x64) are already in this repository and both
   workflows consume them via `Qt5_DIR`. Provenance (per-module commit +
   archive SHA256, configure flags, toolset, OpenSSL version) is in the
   `BOM.json` inside each tree. The recipes that produced them live in a
   separate, app-agnostic repository, so the trees can always be rebuilt;
   see **Vendored Qt and OpenSSL** at the end of this document.
2. **mpv dev for Windows** (the target is 32-bit Windows x86, so the WHOLE build is Win32).> **Phase 5 note**: this requirement applies to the **x64** build (mpv). The **x86** build does not use mpv: it links the FFmpeg i686 **vendored** in `third_party/ffmpeg-win32/` (there is nothing to download) — see "Phase 5" below. **Source: `mpv-player-windows` (sourceforge), `mpv-dev-i686-20210530-git-fd8c673.7z`** MinGW i686 build from 2021-05-30, EARLIER than mpv dropping support for Windows 7: its import table only uses DLLs and symbols that exist in Win7 SP1. The new builds (media-kit 20241021, shinchiro 2026) import STATICALLY W8+ symbols (`AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi` from USER32 W10, `GetDpiForMonitor` from SHCORE, api-ms-win-core-winrt|path|synch) and on Win7 the loader dies at startup with *"The procedure entry point AdjustWindowRectExForDpi could not be located in the dynamic link library USER32.dll"*. Likewise do NOT use `shinchiro/mpv-winbuild-cmake` from May 2026 onward: they embed an OpenSSL 3 from master with the broken commit `1e1ea715` that aborts with `lock != NULL` when initializing mpv. This MinGW build compiles ffmpeg+OpenSSL INSIDE the DLL (it brings no separate dependencies), uses the system `msvcrt` and does not import vulkan-1.dll:
   - `libmpv-1.dll` → rename to `libmpv-2.dll` (the CMake/workflow look for it that way for compatibility with dist.iss and the import lib)
   - headers `include/*.h` → rearrange to `include/mpv/*.h` (the layout CMake expects)
   - The `if(WIN32)` branch of the CMake (`MPV_DEV_DIR`) detects this.
3. **windeployqt** to copy Qt next to the executable.

## Steps (on Windows)

```sh
# in the project folder
# use the vendored Qt tree, NOT an installed C:/Qt/5.15.2 (that is the EOL
# official build the vendor exists to replace)
cmake -B build -DCMAKE_PREFIX_PATH="%CD%\third_party\qt-win32" -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

# package into a single dir
windeployqt build/nevertube_qt2.exe
# + copy libmpv-2.dll next to the exe (and windeployqt for the rest)
```

> Detail: `CMakeLists.txt` already defines `WIN32_EXECUTABLE TRUE` (`:84`); on
> Windows a console does not open by default, so the E2E selftests may not
> print. If you do not see the output of `--selftest-player`, run it with a
> visible console (`cmd` with `set WindowMode`…) or redirect to a file; the rc is
> reflected anyway.

## Post-build verification (Windows)

```sh
./nevertube_qt2.exe --selftest-player   # should print SELFTEST OK
./nevertube_qt2.exe --selftest-db       # SELFTEST OK
```

The player selftest verifies the protected core (hwdec + 33ms timer + render)
**without playing files**. On Windows, the render goes through `vo=libmpv` in a
`QOpenGLWidget` just like on Linux; the stall watchdog uses a local micro-seek
(identical to the `watch_page` fix).

## Phase 5: x86 build WITHOUT mpv (FfmpegBackend)

Since Phase 5 the x86 Windows build (32-bit, Win7) does NOT link
libmpv: its only engine is the `FfmpegBackend` (FFmpeg/libav, much lighter:
~6 MB of DLLs versus the 64.7 MB of libmpv-2.dll). CMake detects it on its own
(`WIN32` + 32-bit pointer), also linking `Qt5 Multimedia` (the backend's
`QAudioSink`). The x64 build (mpv) stays exactly as above
(`-A x64` + `MPV_DEV_DIR`).

**Vendored FFmpeg i686**: there is no public FFmpeg build for 32-bit Windows
(gyan.dev and BtbN/FFmpeg-Builds are win64 only; MSYS2 retired the i686
target). The dev-package lives in `third_party/ffmpeg-win32/` (headers + MSVC
import-libs + DLLs), cross-compiled once with `tools/build_ffmpeg_i686.sh`:
FFmpeg 9.0.2 **LGPL v2.1+** (redistributable), **TLS via schannel** (no OpenSSL
DLLs for FFmpeg), **DXVA2 h264**, **Win7-clean** imports and without
libwinpthread/libgcc/libunwind. The CI and this recipe use it directly. To
rebuild it: `tools/build_ffmpeg_i686.sh`. Details: `third_party/ffmpeg-win32/README.md`.

```sh
cmake -B build -DCMAKE_PREFIX_PATH="%CD%\third_party\qt-win32" \
      -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

`FFMPEG_DEV_DIR` (optional) points to an alternative dev-package; by default
CMake already resolves it to `third_party/ffmpeg-win32`. The package contains
`include/libavcodec/avcodec.h` (and avformat/avutil/swscale/swresample), the
import-libs `avcodec.lib`, `avformat.lib`, `avutil.lib`, `swscale.lib`,
`swresample.lib` (COFF i386 import linkable by MSVC, generated by FFmpeg
itself) and the DLLs `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`,
`swscale-10.dll`, `swresample-7.dll`. When packaging, libmpv-2.dll is NOT
copied: those 5 FFmpeg DLLs go in its place (with no additional external
dependencies).

What changes in x86 with respect to the mpv sections of this document:

- `--selftest-player` verifies only the 33 ms timer and that the backend is
  alive (there is no `hwdec=auto` without mpv); the real playback gate is
  `--selftest-ffmpeg` (embedded clips: demux → decode → swscale offline, with
  DXVA2 for the h264 if the iGPU has it).
- `--selftest-hwprobe` runs only the offline decision table (the live probe
  uses mpv); in x86 the codec profile is decided by platform (`h264-hw`,
  DXVA2 is the target's ceiling) and the reactive ladder
  (`streamDecodeInfoReady` → `hwdecCurrent()=="no"`) covers the machines
  without HW.
- Ajustes → "Volver a detectar" does not appear (there is no probe to relaunch).

## Vendored Qt and OpenSSL

Both Windows workflows get Qt from `third_party/qt-win32` (x86/Win7) and
`third_party/qt-win64` (x64) through `Qt5_DIR`. **There is no `install-qt-action`
any more and no Qt is downloaded**: `jurplel/install-qt-action@v4` only ever
offered **5.15.2 (September 2021)**, and the official Windows binaries stop
there — the 5.15.19 archive is source only. The vendored trees are what let the
Windows packages run a Qt with the last four years of fixes instead.

| | x86 (`qt-win32`) | x64 (`qt-win64`) |
|---|---|---|
| Version | **5.15.19+kde+r96** | **5.15.19+kde+r96** |
| Origin | KDE branch `kde/5.15`, per-module commit + `archiveSha256` in `BOM.json` | same |
| Modules | qtbase, qttools, qtimageformats, **qtmultimedia** | qtbase, qttools, qtimageformats |
| OpenSSL | 3.5.9 | 3.5.9 |
| Toolset | `cl` v142 (`-vcvars_ver=14.29`, SDK 19041) | `cl` v144 (`windows-latest`) |

`+kde+r96` is Qt's own patch-level notation: 96 commits sit on the branch after
the `v5.15.19-lts-lgpl` tag. Those are the fixes that never reach an official
5.15 release but do reach the distro-maintained one, which is the whole reason
the trees are built here.

**Rebuilding.** The recipes live in a separate, app-agnostic repository (Qt for
Windows from the KDE maintenance branch, with pinned commits, a self-provisioning
bootstrap and a watcher that re-triggers the build when upstream moves), so
these trees are reproducible and are never a dead end. Provenance is verifiable
in two independent directions: `BOM.json` records the commit and the
`archiveSha256` of every module it actually consumed.

**OpenSSL is vendored too** (`third_party/openssl-win32|64`), the same
precedent. `configure -openssl-runtime` bakes the OpenSSL DLL *name* into
`Qt5Network` at compile time, so a Qt built against 1.1 looks for
`libssl-1_1*.dll` and never finds the 3.x ones: swapping the DLLs afterwards
does nothing. OpenSSL 1.1.1 has been EOL since September 2023, so the rebuild
was the precondition for retiring the last 1.1-only dependency in the packages;
a test app built against the vendored tree reports `supportsSsl = yes` with
build == runtime == OpenSSL 3.5.9.

**Windows traps in these recipes** (each one cost a round trip; they are the
reason the recipes are checked, not eyeballed):

- The v142 toolset is versioned: `Microsoft.VisualStudio.Component.VC.14.29.16.11.x86.x64`.
  The unversioned `VC.v142.x86.x64` is accepted by the installer and silently
  installs nothing.
- `vcvarsall` takes the **newest** SDK by default, and 10.0.26100 headers drag
  W8+/W10 symbols into the import table (the x86 Win7 gate then fails). The SDK
  is a **positional** argument; `-winsdk=` is not a `vcvarsall` option.
- `vcvarsall` **rebuilds** `INCLUDE`/`LIB`, so any path added before the call is
  wiped. Injecting after it must come from a multi-line `.bat`: in a one-line
  `cmd /c`, `%INCLUDE%` expands at parse time, i.e. *before* `vcvarsall` runs.
- Git for Windows ships a `tar.exe` that **shadows** bsdtar; the GNU tar that
  comes with it exits 128 and extracts nothing. Call
  `%SystemRoot%\System32\tar.exe` by absolute path.
- `$ErrorActionPreference='Stop'` plus a native command writing to **stderr**
  kills the build even on success; judge by `$LASTEXITCODE` around the call.
- `"$var: ..."` inside a double-quoted string: PowerShell reads `$var:` as a
  *scoped* variable (`$env:`, `$script:`…) and the script fails to parse. Write
  `"${var}: ..."`.
- Anything that runs on Windows is ASCII-only + BOM + CRLF: PowerShell 5.1
  reads a BOM-less UTF-8 file as cp1252 and a stray byte sequence decodes into
  a typographic quote, which kills the parser with a confusing error.
