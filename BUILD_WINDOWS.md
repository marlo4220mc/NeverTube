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
2. **mpv dev for Windows** (the target is 32-bit Windows x86, so the WHOLE build is Win32).> **Phase 5 note**: this requirement applies to the **x64** build (mpv). The **x86** build does not use mpv: it links the FFmpeg i686 **vendored** in `third_party/ffmpeg-win32/` (there is nothing to download) — see "Phase 5" below. **Source: `mpv-player-windows` (sourceforge), `mpv-dev-i686-20210530-git-fd8c673.7z`** MinGW i686 build from 2021-05-30, EARLIER than mpv dropping support for Windows 7: its import table only uses DLLs and symbols that exist in Win7 SP1. The new builds (media-kit 20241021, shinchiro 2026) import STATICALLY W8+ symbols (`AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi` from USER32 W10, `GetDpiForMonitor` from SHCORE, api-ms-win-core-winrt|path|synch) and on Win7 the loader dies at startup with *"The procedure entry point AdjustWindowRectExForDpi could not be located in the dynamic link library USER32.dll"*. Likewise do NOT use `shinchiro/mpv-winbuild-cmake` from May 2026 onward: they embed an OpenSSL 3 from master with the broken commit `1e1ea715` that aborts with `lock != NULL` when initializing mpv. This MinGW build compiles ffmpeg+OpenSSL INSIDE the DLL (it brings no separate dependencies), uses the system `msvcrt` and does not import vulkan-1.dll:
   - `libmpv-1.dll` → rename to `libmpv-2.dll` (the CMake/workflow look for it that way for compatibility with dist.iss and the import lib)
   - headers `include/*.h` → rearrange to `include/mpv/*.h` (the layout CMake expects)
   - The `if(WIN32)` branch of the CMake (`MPV_DEV_DIR`) detects this.
3. **windeployqt** to copy Qt next to the executable.

## Steps (on Windows)

```sh
# in the project folder
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019" -G "Visual Studio 17 2022" -A Win32
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
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019" \
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
