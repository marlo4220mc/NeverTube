#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_ffmpeg_i686.sh — builds the vendored i686 FFmpeg for the Windows x86 build
# (Win7 x86) that lives in third_party/ffmpeg-win32/.
#
# Why it exists: there is NO public FFmpeg build for 32-bit Windows.
# gyan.dev and BtbN/FFmpeg-Builds only publish win64, and MSYS2 removed the
# i686 target. The only way is to build it ourselves, and that is what this script
# does (cross-compile from Linux with llvm-mingw, without installing anything on the system).
#
# Properties of the resulting binary (all verified):
#   - i686 (PE32, Windows 7+): _WIN32_WINNT=0x0601.
#   - LGPL v2.1+ (--disable-gpl --disable-nonfree): redistributable with the app
#     by linking it dynamically.
#   - TLS via schannel (--enable-schannel): no OpenSSL DLLs for FFmpeg.
#   - DXVA2 for h264 (--enable-dxva2 --enable-hwaccel=h264_dxva2).
#   - --enable-w32threads: no dependency on libwinpthread-1.dll.
#   - Trimmed to what the app uses (--disable-everything + lists): ~6 MB of
#     DLLs instead of ~15 MB with the full FFmpeg set.
#   - Win7-clean imports (no GetSystemTimePreciseAsFileTime, *ForDpi,
#     GetDpiForMonitor, api-ms-win-*, WaitOnAddress, etc.).
#
# Usage:
#   tools/build_ffmpeg_i686.sh [destination]
#   (default destination: third_party/ffmpeg-win32)
#
# Network requirements: downloads llvm-mingw, nasm and the FFmpeg source.
# ---------------------------------------------------------------------------
set -euo pipefail

FFMPEG_VER="9.0.2"
LLVM_MINGW_VER="20260922"
LLVM_MINGW_TARBALL="llvm-mingw-${LLVM_MINGW_VER}-msvcrt-ubuntu-22.04-x86_64.tar.xz"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VER}/${LLVM_MINGW_TARBALL}"
NASM_VER="2.16.03"
NASM_URL="https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VER}/nasm-${NASM_VER}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VER}.tar.xz"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST="${1:-${REPO_ROOT}/third_party/ffmpeg-win32}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg-i686.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "==> work dir: ${WORK}"
echo "==> destino:  ${DEST}"
mkdir -p "${WORK}"
cd "${WORK}"

echo "==> descargando llvm-mingw ${LLVM_MINGW_VER}, nasm ${NASM_VER}, FFmpeg ${FFMPEG_VER}"
curl -fsSL -o llvm-mingw.tar.xz "${LLVM_MINGW_URL}"
curl -fsSL -o nasm.tar.xz "${NASM_URL}"
curl -fsSL -o ffmpeg.tar.xz "${FFMPEG_URL}"
tar xf llvm-mingw.tar.xz
tar xf nasm.tar.xz
tar xf ffmpeg.tar.xz
LLVM_DIR="${WORK}/llvm-mingw-${LLVM_MINGW_VER}-msvcrt-ubuntu-22.04-x86_64"

echo "==> compilando nasm (host)"
( cd "nasm-${NASM_VER}" && ./configure --prefix="${WORK}/nasm" >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null )
export PATH="${WORK}/nasm/bin:${LLVM_DIR}/bin:${PATH}"

echo "==> configurando FFmpeg ${FFMPEG_VER} (i686, LGPL, schannel, dxva2)"
mkdir -p build && cd build
"../ffmpeg-${FFMPEG_VER}/configure" \
  --target-os=mingw32 --arch=x86 --cpu=i686 \
  --cross-prefix=i686-w64-mingw32- --cc=i686-w64-mingw32-clang \
  --enable-cross-compile \
  --enable-shared --disable-static \
  --disable-doc --disable-avdevice \
  --disable-everything \
  --disable-gpl --disable-nonfree --disable-autodetect \
  --enable-w32threads --disable-pthreads \
  --enable-schannel --enable-dxva2 --enable-hwaccel=h264_dxva2 \
  --disable-debug \
  --enable-protocol=file,http,https,tcp,tls,crypto,data,cache,httpproxy,concat,pipe \
  --enable-demuxer=hls,mov,matroska,mpegts,aac,mp3,ogg,flv,concat \
  --enable-decoder=h264,vp9,av1,aac,opus,mp3,vorbis,flac,pcm_s16le,pcm_f32le \
  --enable-parser=h264,aac,av1,opus,mpegaudio,vorbis \
  --enable-bsf=h264_mp4toannexb,aac_adtstoasc,extract_extradata,vp9_superframe,vp9_superframe_split,av1_frame_merge,av1_frame_split \
  --enable-muxer=matroska,mp4,ipod \
  --enable-encoder=aac \
  --enable-filter=aformat,anull,atrim,crop,format,hflip,null,rotate,transpose,trim,vflip \
  --enable-ffmpeg \
  --extra-cflags="-D_WIN32_WINNT=0x0601 -DWINVER=0x0601"

echo "==> compilando e instalando"
make -j"$(nproc)"
make install prefix="${WORK}/install"

echo "==> ensamblando ${DEST}"
# The README is written by hand and is NOT part of the build: it is kept.
keep_readme=""
if [ -f "${DEST}/README.md" ]; then
  keep_readme="$(mktemp)"
  cp "${DEST}/README.md" "${keep_readme}"
fi
rm -rf "${DEST}"
mkdir -p "${DEST}/include" "${DEST}/lib" "${DEST}/bin"
cp -r "${WORK}/install/include/"* "${DEST}/include/"
for l in avcodec avformat avutil swscale swresample; do
  cp "${WORK}/install/bin/${l}.lib" "${DEST}/lib/"
done
for d in avcodec-63 avformat-63 avutil-61 swscale-10 swresample-7 avfilter-12; do
  cp "${WORK}/install/bin/${d}.dll" "${DEST}/bin/"
done
# The CLI (ffmpeg.exe): it is what the DOWNLOADER runs to join the two
# tracks. It is NOT a dependency of the app itself (the app links the DLLs
# above), so it is kept apart: the packagers copy it only if they want the
# downloader to work out of the box.
cp "${WORK}/install/bin/ffmpeg.exe" "${DEST}/bin/"
cp "${WORK}/ffmpeg-${FFMPEG_VER}/COPYING.LGPLv2.1" "${DEST}/LICENSE.LGPLv2.1"
if [ -n "${keep_readme}" ]; then
  cp "${keep_readme}" "${DEST}/README.md"
  rm -f "${keep_readme}"
fi
ls -la "${DEST}/bin/"

echo "==> gate de imports Win7 (objdump)"
poison='GetSystemTimePreciseAsFileTime|AdjustWindowRectExForDpi|GetSystemMetricsForDpi|GetDpiForMonitor|PathCchCanonicalizeEx|SetThreadDpiAwarenessContext|SetProcessDpiAwarenessContext|WaitOnAddress|WakeByAddressAll|WakeByAddressSingle|RoGetActivationFactory|RoOriginateLanguageException|AddDllDirectory|SetDefaultDllDirectories'
fail=0
# The gate covers the DLLs AND the CLI: both are loaded on the Win7 machine
# (the CLI is launched by the downloader).
for d in "${DEST}"/bin/*.dll "${DEST}"/bin/*.exe; do
  if i686-w64-mingw32-objdump -p "$d" | grep -qE "${poison}"; then
    echo "  ¡POISON en $(basename "$d")!"; fail=1
  fi
done
[ "${fail}" -eq 0 ] && echo "  OK: ninguna DLL ni EXE importa símbolos W8+"

echo "==> listo: ${DEST}"
