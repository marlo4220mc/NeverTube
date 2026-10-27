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
   **Do not install Qt, and do not build it here.** Both Windows workflows
   **download** the tree from a release of the build repository
   (`marlo4220mc/qt5-kde-win-build`, tag `qt-5.15.19-kde-r96`), verify its
   SHA-256 and point `Qt5_DIR` at it. The 177 MB of DLLs are deliberately not
   committed to this repository. Provenance (per-module commit and archive
   SHA256, configure flags, toolset, OpenSSL version) travels inside the
   archive as `BOM.json`; the pinned sources and the recipe that produced it
   live in that other repository, so the tree is reproducible and never a dead
   end. See **Vendored Qt and OpenSSL** at the end of this document.
2. **mpv dev for Windows** — only the builds that link mpv need it: **x64** and
   **Windows 10 x86** (i686). The **Win7 x86** build does NOT use mpv: it links
   the FFmpeg i686 **vendored** in `third_party/ffmpeg-win32/` (there is nothing
   to download) — see "Phase 5" below. Why Win7 has no mpv: every modern 32-bit
   mpv (media-kit 20241021, shinchiro 2026) imports STATICALLY W8+ symbols
   (`AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi` from USER32 W10,
   `GetDpiForMonitor` from SHCORE, api-ms-win-core-winrt|path|synch) and on Win7
   the loader dies at startup with *"The procedure entry point
   AdjustWindowRectExForDpi could not be located in the dynamic link library
   USER32.dll"*. The Win7-era workaround this document used to describe
   (`mpv-dev-i686-20210530-git-fd8c673.7z`, `libmpv-1.dll` renamed to
   `libmpv-2.dll`) is retired together with that engine.
   - **x64**: `shinchiro/mpv-winbuild-cmake`, `mpv-dev-x86_64-20260924-…`
     (tag + asset + sha256 pinned in the `windows-win10-x64` job of `.github/workflows/release.yml`).
   - **Windows 10 x86**: the i686 package pinned in
     the `windows-win10-x86` job of `.github/workflows/release.yml`. It is deliberately NOT the latest
     one: every i686 build published after 2026-03-01 aborts with `OpenSSL
     internal error: assertion failed: lock != NULL`, because the OpenSSL 3 they
     link statically carries commit `1e1ea715` (added 2026-03-31, removed
     upstream in `5dfc09b1` on 2026-09-01, still not in the OpenSSL those builds
     use). The pinned 2026-03-01 build initializes TLS cleanly — see "Windows 10
     x86 build WITH mpv" below for the measurements.
   - Both packages bring `libmpv-2.dll` + `include/mpv/*.h`; `MPV_DEV_DIR` points
     at the unpacked folder.
3. **windeployqt** to copy Qt next to the executable.

## Steps (on Windows)

The tree is not in the repository, so bring it first — that is the same thing
the CI does, minus the automation:

```sh
# fetch the Qt tree (tag + sha256 pinned in .github/workflows/release.yml)
curl -L -o qt-win32.7z https://github.com/marlo4220mc/qt5-kde-win-build/releases/download/qt-5.15.19-kde-r96/qt-win32.7z
7z x qt-win32.7z -othird_party -y && mv third_party/qt-win32/* third_party/ && rmdir third_party/qt-win32
```

```sh
# in the project folder
cmake -B build -DCMAKE_PREFIX_PATH="%CD%\third_party\qt-win32" -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

# package into a single dir
windeployqt build/nevertube.exe
# + copy libmpv-2.dll next to the exe (and windeployqt for the rest)
```

> Detail: `CMakeLists.txt` already defines `WIN32_EXECUTABLE TRUE` (`:84`); on
> Windows a console does not open by default, so the E2E selftests may not
> print. If you do not see the output of `--selftest-player`, run it with a
> visible console (`cmd` with `set WindowMode`…) or redirect to a file; the rc is
> reflected anyway.

## Post-build verification (Windows)

```sh
./nevertube.exe --selftest-player   # should print SELFTEST OK
./nevertube.exe --selftest-db       # SELFTEST OK
```

The player selftest verifies the protected core (hwdec + 33ms timer + render)
**without playing files**. On Windows, the render goes through `vo=libmpv` in a
`QOpenGLWidget` just like on Linux; the stall watchdog uses a local micro-seek
(identical to the `watch_page` fix).

## Phase 5: Windows 7 x86 build WITHOUT mpv (FfmpegBackend)

The **Win7** x86 build (32-bit) does NOT link
libmpv: its only engine is the `FfmpegBackend` (FFmpeg/libav, much lighter:
~6 MB of DLLs versus the 64.7 MB of libmpv-2.dll). CMake detects it on its own
(`WIN32` + 32-bit pointer), also linking `Qt5 Multimedia` (the backend's
`QAudioSink`). The x64 build (mpv) stays exactly as above
(`-A x64` + `MPV_DEV_DIR`), and Windows 10 x86 has its own engine choice
(next section).

**Vendored FFmpeg i686**: there is no public FFmpeg build for 32-bit Windows
(gyan.dev and BtbN/FFmpeg-Builds are win64 only; MSYS2 retired the i686
target). The dev-package lives in `third_party/ffmpeg-win32/` (headers + MSVC
import-libs + DLLs), cross-compiled once with `tools/build_ffmpeg_i686.sh`:
FFmpeg 9.0.2 **LGPL v2.1+** (redistributable), **TLS via schannel** (no OpenSSL
DLLs for FFmpeg), **DXVA2 h264**, **Win7-clean** imports and without
libwinpthread/libgcc/libunwind. The CI and this recipe use it directly. To
rebuild it: `tools/build_ffmpeg_i686.sh`. Details: `third_party/ffmpeg-win32/README.md`.

The same recipe also builds the **CLI** that the downloader needs
(`ffmpeg.exe`, with the muxers enabled): it is part of the same package, and
the installer carries it. The **x64** build gets its own from the same
script (`tools/build_ffmpeg_i686.sh <dest> x86_64`) into
`third_party/ffmpeg-win64/`: the x64 build plays with mpv and does not link
libav, so those DLLs are the ones the CLI needs and not the ones the app
uses. Both builds carry the **mp3 encoder** (LAME, which is LGPL v2 and
NOT GPL, so the LGPL build can link it: the recipe compiles it as
`libmp3lame-0.dll`).

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

What changes in the Win7 x86 build with respect to the mpv sections of this document:

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

## Windows 10 x86 build WITH mpv (i686, same engine as x64)

The 32-bit build for Windows 10/11 is **not** the Win7 one. This build links
libmpv and uses `MpvBackend` exactly like x64: `hwdec=auto`, `HwProbe`, the
codec ladder and Ajustes → "Volver a detectar hardware". The `FfmpegBackend` is
not compiled at all (no `Qt5 Multimedia`, no `FFMPEG_DEV_DIR`).

Source of that i686 libmpv (MEASURED, not assumed): the workflow of this
deliverable pins the `mpv-dev-i686-20260301-git-05fac7f.7z` package of the
`mpv-player-windows` project on SourceForge (mpv 0.41.0-243-g05fac7f; URL +
sha256 pinned in the `windows-win10-x86` job). That project is the
same mpv-winbuild-cmake tree shinchiro publishes on GitHub, with the i686 target
enabled; Predidit's `libmpv-win32-video-cmake` is the same build system too (its
`20251210` release was the previous pin here, and its 2026-06/08/09 releases are
broken like everything else after March).

The i686 builds are broken in a **window**, and the cause is OpenSSL, not mpv.
The OpenSSL 3 that libmpv links statically took commit `1e1ea715` (2026-03-31),
whose `CRYPTO_atomic_*` asserts `lock != NULL`; on 32-bit Windows that branch is
the one that runs (`!OSSL_USE_INTERLOCKEDOR64`), so the process aborts the
moment mpv initializes TLS: `OpenSSL internal error: assertion failed: lock !=
NULL`. x64 never reaches it (that is why only the i686 deliverable is affected,
and why the x64 package of the same day works). Upstream removed the assert in
`5dfc09b1` (2026-09-01), but the OpenSSL release these builds link does not carry
that fix yet.

Measured 2026-10-26 with a real 32-bit host (`mpv_create` + `loadfile` of an
https URL, 32-bit PowerShell + P/Invoke; the same check reproduces the abort
with the i686 `mpv.exe` packages):

| i686 package | TLS init |
|---|---|
| 2026-03-01 (`05fac7f`) | **OK** — `MPV_EVENT_FILE_LOADED`, exit 0 |
| 2026-04-05 (`befe1e7`) | aborts (`lock != NULL`) |
| 2026-08-30, 2026-09-20/27, 2026-10-04/11/18 | abort |
| shinchiro GitHub 2026-09-24 / 2026-10-21 | abort (also on `--version`) |
| Predidit 2026-06-23 / 2026-08-11 / 2026-09-15 | abort |

So 2026-03-01 is the **newest** i686 libmpv that works, and that is what is
pinned. It keeps the same `libmpv-2.dll` / `include/mpv/*.h` layout, so the rest
of this section does not change. Two notes for whoever bumps it: the pinned
build does NOT import `vulkan-1.dll` (the 2026-10 ones do, so the bundled
`vulkan-1.dll` becomes mandatory), and its `VSScript.dll` is a **delay-load**
import (VapourSynth filters, unused by the app). It is also ~95 MB unstripped
against the ~33 MB of the Predidit package, so the x86 setup grows by ~18 MB:
that is the price of the newer mpv, not a mistake.

CMake needs the explicit switch, because by default **every** 32-bit Windows
target is taken to be the Win7 build and sent to FFmpeg:

```sh
# 1) the dev package (headers + libmpv-2.dll), from the release the CI uses
#    (MPV_URL / MPV_ASSET / MPV_SHA256 at the top of the windows-win10-x86 job)
# 2) MSVC import-lib from the DLL: the package only ships the MinGW
#    libmpv.dll.a. dumpbin /exports -> .def -> lib /machine:x86
#    (find_text = mpv_\w*), saved as mpv-2.lib inside the package folder.
cmake -B build -A Win32 -DNEVERTUBE_MPV_ON_X86=ON `
      -DCMAKE_PREFIX_PATH="%CD%\third_party\qt-win32\lib\cmake" `
      -DMPV_DEV_DIR=<ruta al mpv-dev i686>
cmake --build build --config Release
```

`-DNEVERTUBE_MPV_ON_X86=ON` turns `NEVERTUBE_X86_FFMPEG_BUILD` off, so:

- `mpv_backend.cpp` is compiled and `MPV::MPV` is linked (the x64 path).
- `NEVERTUBE_NO_MPV` is **not** defined: `HwProbe`, the settings page's
  "redetect" button, the reactive codec ladder and the AV1 handling behave as on
  x64. In particular AV1 is only requested when the probe found AV1 hardware.
- `FFMPEG_DEV_DIR` is not used and `Qt5 Multimedia` is not needed.

Packaging (what `dist_x86_win10.iss` expects in `dist\`): Qt via `windeployqt`
(x86 tree), `libmpv-2.dll`, the 32-bit `vulkan-1.dll` (belt-and-braces today:
MEASURED on the pinned i686 libmpv of 2026-03-01 it does NOT import vulkan-1.dll
— mpv gets its device via OpenGL/D3D there — while the 2026-10 builds do;
LunarG runtime, `\x86\`), `libssl-3.dll` + `libcrypto-3.dll`
(`third_party/openssl-win32`), `d3dcompiler_47.dll` from `SysWOW64`, the
downloader's CLI (`third_party\ffmpeg-win32\bin\ffmpeg.exe` + its DLLs, a
separate process: the app does not link libav here) and `VC_redist.x86.exe` from
**vs/17** (`aka.ms/vs/17`; the Win10 line has no Win7 import restriction).
Toolset: the default of `windows-latest`, no `-T v142` — the v142/SDK-19041 pin
belongs to Win7.

The installer is exclusive to this target: it cancels on 64-bit Windows (→
`nevertube-setup-x64.exe`) and on Windows before 10 (→
`nevertube-setup-win7-x86.exe`).

## Vendored Qt and OpenSSL

Both Windows workflows get Qt from `third_party/qt-win32` (x86/Win7) and
`third_party/qt-win64` (x64), **fetched from a release** of the build repository
rather than committed here. **There is no `install-qt-action` any more and no Qt
is built or downloaded from Qt**: `jurplel/install-qt-action@v4` only ever
offered **5.15.2 (September 2021)**, and the official Windows binaries stop
there — the 5.15.19 archive is source only. That tree is what lets the Windows
packages run a Qt with the last four years of fixes instead.

Two rules keep this honest, because a binary fetched from somewhere is exactly
the thing that rots silently:

- **Pinned tag, not "latest".** `QT_TAG` names one published release. A floating
  tag would change the Qt of every run without anyone deciding it, which is the
  same failure as a `BOM.json` that describes something other than what it
  contains.
- **Pinned SHA-256.** `QT_SHA32` is the digest of the exact `.7z`. The job fails
  if the bytes differ, so a rebuilt or re-uploaded artifact cannot pass as the
  one the app was tested against.

To move forward, bump `QT_TAG` and `QT_SHA32` together (both are at the top of
each Windows workflow) — the archive also carries its own `BOM.json` with the
per-module commits behind it.

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
`archiveSha256` of every module it actually consumed, and the workflow's
`QT_SHA32` records the digest of the archive it actually downloaded.

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
