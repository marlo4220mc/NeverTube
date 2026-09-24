# FFmpeg i686 para el build x86 (Windows 7 de 32 bits)

Paquete **precompilado** de FFmpeg 9.0.2 compartido (DLLs) para el build
**x86 de Windows** (Win7 de 32 bits). Es el motor del `FfmpegBackend` cuando
`NEVERTUBE_X86_FFMPEG_BUILD` está activo (ver `CMakeLists.txt`).

## Por qué está vendorizado

**No existe ningún build público de FFmpeg para Windows de 32 bits.** Las dos
fuentes mantenidas son solo x64, y MSYS2 retiró el target i686:

- `gyan.dev` — "All builds are 64-bit" (y desde 2026 exige Windows 10+).
- `BtbN/FFmpeg-Builds` — assets únicamente `win64`.
- MSYS2 — `mingw-w64-i686-ffmpeg` ya no existe.

Como no hay binario que descargar, se cross-compila una vez con
`tools/build_ffmpeg_i686.sh` y se vendoriza aquí. El CI no descarga ni compila
FFmpeg: usa este paquete directamente.

## Propiedades (verificadas)

- **i686** (PE32, Windows 7+): compilado con `-D_WIN32_WINNT=0x0601`.
- **LGPL v2.1+** (`--disable-gpl --disable-nonfree`): redistribuible con la app
  propietaria enlazándolo dinámicamente. Ver `LICENSE.LGPLv2.1`.
- **TLS por schannel** (`--enable-schannel`): FFmpeg no necesita DLLs de
  OpenSSL; usa `Secur32.dll`/`ncrypt.dll`/`CRYPT32.dll` del sistema. (La app
  sigue usando OpenSSL 1.1 solo para el HTTPS de Qt.)
- **DXVA2 para H.264** (`--enable-dxva2 --enable-hwaccel=h264_dxva2`): la
  aceleración por hardware del equipo (Fase 4 del backend).
- **Sin dependencias externas**: `--enable-w32threads` evita
  `libwinpthread-1.dll`; el runtime de llvm-mingw va estático (no hay
  `libgcc_s_*.dll` ni `libunwind.dll`). Las DLLs solo importan DLLs del sistema
  presentes en Win7 SP1 (`msvcrt`, `KERNEL32`, `USER32`, `ole32`, `WS2_32`,
  `Secur32`, `ncrypt`, `CRYPT32`, `bcrypt`).
- **Imports Win7-limpios**: ninguna DLL importa símbolos W8+
  (`GetSystemTimePreciseAsFileTime`, `*ForDpi`, `GetDpiForMonitor`,
  `api-ms-win-*`, `WaitOnAddress`, `RoGetActivationFactory`, …). Lo verifica
  `tools/build_ffmpeg_i686.sh` con `objdump -p` (mismo gate que el CI).

## Contenido

```
include/   headers públicos (libavcodec, libavformat, libavutil, libswscale, libswresample)
lib/       import-libs MSVC (avcodec.lib, avformat.lib, avutil.lib, swscale.lib, swresample.lib)
bin/       DLLs de runtime (avcodec-63.dll, avformat-63.dll, avutil-61.dll, swscale-10.dll, swresample-7.dll)
LICENSE.LGPLv2.1
```

`CMakeLists.txt` usa `include/` + `lib/`; el workflow copia `bin/*.dll` al
instalador. Los `.lib` los genera el propio FFmpeg (`dlltool -m i386`) y son
enlazables por MSVC.

## Configuración exacta

FFmpeg 9.0.2, llvm-mingw 20260922 (variante **msvcrt**), nasm 2.16.03.
Recortado a lo que usa la app:

- decoders: h264, vp9, av1, aac, opus, mp3, vorbis, flac, pcm_s16le, pcm_f32le
- demuxers: hls, mov, matroska, mpegts, aac, mp3, ogg, flv, concat
- protocols: file, http, https, tcp, tls, crypto, data, cache, httpproxy, concat, pipe
- parsers: h264, aac, av1, opus, mpegaudio, vorbis
- bsfs: h264_mp4toannexb, aac_adtstoasc, extract_extradata, vp9_superframe(_split), av1_frame_merge/split
- sin encoders/muxers/avfilter/avdevice

Resultado: ~6 MB de DLLs (frente a ~15 MB con el set completo de FFmpeg y a los
64,7 MB de `libmpv-2.dll`).

## Verificación realizada

- Demux + decodificación real bajo Wine de los clips del repo
  (`h264.mp4`, `vp9.webm`, `av1.mkv`) y de un HLS fMP4 h264+aac generado con el
  ffmpeg del sistema (`-count_frames`: 75 vídeo + 142 audio).
- Handshake TLS real bajo Wine contra un `https://` (`[tls] Handshake …`),
  confirmando el camino schannel.
- Gate de imports Win7 con `objdump -p` sobre las 5 DLLs: limpio.

## Reconstruir

```sh
tools/build_ffmpeg_i686.sh            # escribe en third_party/ffmpeg-win32
```

El script descarga llvm-mingw, nasm y el código de FFmpeg, compila y vuelca el
mismo layout. Es la fuente de verdad de la procedencia de este paquete.
