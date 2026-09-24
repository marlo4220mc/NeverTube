# Build Windows

Windows **no** se puede compilar desde esta máquina (Linux sin toolchain mpv/Qt
mingw — solo `wine` para ejecutar, no compilar). El proyecto NO mantiene un
`.exe` propio: el build final se hace en tu equipo Windows y este documento es
la lista exacta de lo que necesita. La cuestión de "compilar en Windows" quedó
**cerrada en el CMake como costura**: en la rama `if(WIN32)` se usa
`find_package(unofficial-mpv CONFIG)` en vez de `pkg_check_modules(mpv)`, y el
resto sigue igual (Qt5, OpenGL, SQLite). Esta sección solo se ejecuta si
`WIN32`; en Linux/Unix el build queda **idéntico** (sin regresión).

## Requisitos

1. **Qt 5.15.x** (MSVC 2019 o MinGW) → al final `windeployqt` se encarga de las DLLs de Qt.
2. **mpv dev para Windows** (el target es Windows x86 de 32 bits, así que TODO el build es Win32).> **Nota Fase 5**: este requisito aplica al build **x64** (mpv). El build **x86** no usa mpv: enlaza el FFmpeg i686 **vendorizado** en `third_party/ffmpeg-win32/` (no hay que descargar nada) — ver "Fase 5" abajo. **Fuente: `mpv-player-windows` (sourceforge), `mpv-dev-i686-20210530-git-fd8c673.7z`** build MinGW i686 de 2021-05-30, ANTERIOR a que mpv dejara de soportar Windows 7: su tabla de imports solo usa DLLs y símbolos que existen en Win7 SP1. Los builds nuevos (media-kit 20241021, shinchiro 2026) importan ESTÁTICAMENTE símbolos W8+ (`AdjustWindowRectExForDpi`, `GetSystemMetricsForDpi` de USER32 W10, `GetDpiForMonitor` de SHCORE, api-ms-win-core-winrt|path|synch) y en Win7 el loader muere al arrancar con *"The procedure entry point AdjustWindowRectExForDpi could not be located in the dynamic link library USER32.dll"*. Igualmente NO usar `shinchiro/mpv-winbuild-cmake` de mayo 2026 en adelante: incrustan un OpenSSL 3 de master con el commit roto `1e1ea715` que aborta con `lock != NULL` al inicializar mpv. Este build MinGW compila ffmpeg+OpenSSL DENTRO del DLL (no trae dependencias aparte), usa `msvcrt` del sistema y no importa vulkan-1.dll:
   - `libmpv-1.dll` → renombrar a `libmpv-2.dll` (el CMake/workflow lo buscan así por compatibilidad con dist.iss y la import lib)
   - cabeceras `include/*.h` → reordenar a `include/mpv/*.h` (layout que espera CMake)
   - La rama `if(WIN32)` del CMake (`MPV_DEV_DIR`) detecta esto.
3. **windeployqt** para copiar Qt al lado del ejecutable.

## Pasos (en Windows)

```sh
# en la carpeta del proyecto
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019" -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

# empaquetar en un solo dir
windeployqt build/nevertube_qt2.exe
# + copiar libmpv-2.dll al lado del exe (y windeployqt para el resto)
```

> Detalle: `CMakeLists.txt` ya define `WIN32_EXECUTABLE TRUE` (`:84`); en
> Windows no se abre consola por defecto, así que los selftest E2E pueden no
> imprimir. Si no ves la salida de `--selftest-player`, ejecuta con una consola
> visible (`cmd` con `set WindowMode`…) o redirect a archivo; el rc se refleja
> igualmente.

## Verificación post-build (Windows)

```sh
./nevertube_qt2.exe --selftest-player   # debería salir SELFTEST OK
./nevertube_qt2.exe --selftest-db       # SELFTEST OK
```

El selftest del player verifica el núcleo protegido (hwdec + timer 33ms + render)
**sin reproducir archivos**. En Windows, el render va por `vo=libmpv` en un
`QOpenGLWidget` igual que en Linux; el watchdog de atasco usa micro-seek local
(idéntico al fix de `watch_page`).

## Fase 5: build x86 SIN mpv (FfmpegBackend)

Desde la Fase 5 el build x86 de Windows (32 bits, Win7) NO enlaza
libmpv: su único motor es el `FfmpegBackend` (FFmpeg/libav, mucho más ligero:
~6 MB de DLLs frente a los 64,7 MB de libmpv-2.dll). El CMake lo detecta solo
(`WIN32` + puntero de 32 bits), enlazando además `Qt5 Multimedia` (el
`QAudioSink` del backend). El build x64 (mpv) queda exactamente como arriba
(`-A x64` + `MPV_DEV_DIR`).

**FFmpeg i686 vendorizado**: no existe build público de FFmpeg para Windows de
32 bits (gyan.dev y BtbN/FFmpeg-Builds son solo win64; MSYS2 retiró el target
i686). El dev-package vive en `third_party/ffmpeg-win32/` (headers + import-libs
MSVC + DLLs), cross-compilado una vez con `tools/build_ffmpeg_i686.sh`:
FFmpeg 9.0.2 **LGPL v2.1+** (redistribuible), **TLS por schannel** (sin DLLs de
OpenSSL para FFmpeg), **DXVA2 h264**, imports **Win7-limpios** y sin
libwinpthread/libgcc/libunwind. El CI y esta receta lo usan directamente. Para
reconstruirlo: `tools/build_ffmpeg_i686.sh`. Detalles: `third_party/ffmpeg-win32/README.md`.

```sh
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019" \
      -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

`FFMPEG_DEV_DIR` (opcional) apunta a un dev-package alternativo; por defecto el
CMake ya lo resuelve a `third_party/ffmpeg-win32`. El paquete contiene
`include/libavcodec/avcodec.h` (y avformat/avutil/swscale/swresample), las
import-libs `avcodec.lib`, `avformat.lib`, `avutil.lib`, `swscale.lib`,
`swresample.lib` (COFF-import i386 enlazables por MSVC, generadas por el propio
FFmpeg) y las DLLs `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`,
`swscale-10.dll`, `swresample-7.dll`. Al empaquetar NO se copia libmpv-2.dll:
en su lugar van esas 5 DLLs de FFmpeg (sin dependencias externas adicionales).

Qué cambia en x86 respecto a las secciones de mpv de este documento:

- `--selftest-player` verifica solo el timer de 33 ms y que el backend esté
  vivo (no hay `hwdec=auto` sin mpv); el gate de reproducción real es
  `--selftest-ffmpeg` (clips embebidos: demux → decode → swscale offline, con
  DXVA2 para el h264 si la iGPU lo tiene).
- `--selftest-hwprobe` corre solo la tabla de decisión offline (el sondeo vivo
  usa mpv); en x86 el perfil de códec se decide por plataforma (`h264-hw`,
  DXVA2 es el techo del target) y la escalera reactiva
  (`streamDecodeInfoReady` → `hwdecCurrent()=="no"`) cubre a las máquinas sin
  HW.
- Ajustes → "Volver a detectar" no aparece (no hay sondeo que relanzar).
