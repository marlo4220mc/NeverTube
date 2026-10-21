# FFmpeg x86_64 for the x64 build (Windows 10+)

**Precompiled** package of shared FFmpeg 9.0.2 for the **x86_64** build, and
of the **CLI** (`ffmpeg.exe`).

## Why it is here

The x64 build plays with **mpv** (the protected piece): it does NOT link
libav. But the **downloader** needs the ffmpeg to join the two tracks of a
downloaded video, and on Windows the setup carried no ffmpeg at all: the
download finished and the mux failed with "ffmpeg was not found".

Instead of downloading a third-party binary (36 MB, GPLv3, from gyan or
BtbN), it is **our own build**: the same recipe as the x86 one
(`tools/build_ffmpeg_i686.sh <dest> x86_64`), cross-compiled from Linux
with llvm-mingw. Same licence as the rest (LGPL v2.1+, see
`LICENSE.LGPLv2.1`), same trimming, and it is **405 KB** for the CLI.

## What it is for

- `bin/ffmpeg.exe`: the CLI the downloader launches (`-c copy` into an MKV).
- `bin/av*.dll`: the libraries the CLI itself needs. **The app does not
  link them** (it uses mpv): they are here for the CLI.

## Properties

- **x86_64** (PE32+), LGPL v2.1+ (`--disable-gpl --disable-nonfree`).
- **TLS via schannel**: no OpenSSL DLLs for FFmpeg.
- **Win7-clean imports** (the gate of the recipe covers the EXE too).
- **No external dependencies**: `--enable-w32threads` and a static
  llvm-mingw runtime (there is no `libgcc_s_*.dll` nor `libunwind.dll`).

## The mp3 encoder IS there

**LAME is LGPL v2, not GPL** (FFmpeg's own list of the GPL libraries does
not carry `libmp3lame`), so the LGPL build links it: the recipe compiles
LAME as a DLL (`libmp3lame-0.dll`) and FFmpeg links it against it. The
"solo el audio" download is an mp3.

## Contents

```
bin/       ffmpeg.exe + avcodec-63.dll, avformat-63.dll, avutil-61.dll,
           swscale-10.dll, swresample-7.dll, avfilter-12.dll, libmp3lame-0.dll
LICENSE.LGPLv2.1        (FFmpeg)
LICENSE.LGPLv2-LAME.txt  (LAME, the mp3 encoder)
```

## How it was built

```sh
tools/build_ffmpeg_i686.sh third_party/ffmpeg-win64 x86_64
```

Verified under Wine: the `ffmpeg.exe` muxes an h264 mp4 + an aac m4a into an
MKV with both tracks, and it writes the M4A of the audio fallback.
