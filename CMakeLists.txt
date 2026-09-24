cmake_minimum_required(VERSION 3.16)
project(nevertube_qt2 VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Limitar la dependencia del sistema operativo a Windows 7 (WINVER=0x0601 / WIN32_WINNT=0x0601)
# para que el binario no intente importar funciones nuevas de Win8+ como AdjustWindowRectExForDpi,
# que no existen en USER32.dll de Win7 SP1.
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
find_package(OpenGL REQUIRED)
# pkg-config SOLO se usa para mpv en Linux (PkgConfig::MPV). En Windows NO
# existe pkg-config y mpv va por MPV_DEV_DIR (find_path/find_library), así que
# el find_package(PkgConfig) debe vivir en el else() para no reventar el
# configure WIN32 (sin pkg-config en el runner → "Could NOT find PkgConfig").
# mpv: en Linux se halla con pkg-config (mpv.pc, target PkgConfig::MPV). En
# Windows NO hay CONFIG usable en el registro de vcpkg (el port `mpv` se retiró:
# `error: mpv does not exist`), así que se enlaza contra el dev-package oficial
# de mpv.io (shinchiro/mpv-winbuild-cmake, `mpv-dev-*.7z`): headers `mpv/*.h` +
# import-lib `mpv-2.lib` + `mpv-2.dll`, ruta en la variable MPV_DEV_DIR
# (BUILD_WINDOWS.md / workflow windows). Se unifica en MPV::MPV para que
# target_link_libraries no dependa de la plataforma. Esta máquina (Linux)
# compila/verifica la rama else(); la if(WIN32) es build-config y el build
# final de Windows corre en el equipo/CI del usuario.
#
# Fase 5: el build x86 de Windows (Win7) NO enlaza mpv — su único
# motor es el FfmpegBackend (FFMPEG_DEV_DIR + Qt5 Multimedia, más abajo).
# Todo lo demás (Linux y Windows x64) conserva mpv exactamente como estaba.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(NEVERTUBE_X86_FFMPEG_BUILD ON)
else()
  set(NEVERTUBE_X86_FFMPEG_BUILD OFF)
endif()

if(WIN32 AND NOT NEVERTUBE_X86_FFMPEG_BUILD)
  set(MPV_DEV_DIR "" CACHE PATH "Directorio del paquete mpv-dev de Windows (include/ + lib/ + dll)")
  if(NOT MPV_DEV_DIR)
    message(FATAL_ERROR "WIN32 necesita -DMPV_DEV_DIR=<ruta a mpv-dev> (BUILD_WINDOWS.md)")
  endif()
  find_path(MPV_INCLUDE_DIR mpv/client.h PATHS "${MPV_DEV_DIR}" PATH_SUFFIXES include NO_DEFAULT_PATH)
  find_library(MPV_LIBRARY NAMES mpv-2.lib libmpv-2.dll mpv-2 mpv PATHS "${MPV_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
    message(FATAL_ERROR "mpv-dev incompleto en '${MPV_DEV_DIR}': falta include/mpv/client.h (got '${MPV_INCLUDE_DIR}') o mpv-2.lib (got '${MPV_LIBRARY}'). Revisa BUILD_WINDOWS.md")
  endif()
  add_library(MPV::MPV INTERFACE IMPORTED)
  set_target_properties(MPV::MPV PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${MPV_LIBRARY}")
elseif(NOT WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
  add_library(MPV::MPV INTERFACE IMPORTED)
  set_target_properties(MPV::MPV PROPERTIES
    INTERFACE_LINK_LIBRARIES PkgConfig::MPV)
endif()

add_executable(nevertube_qt2
  src/main.cpp
  src/models.h
  src/db/app_database.cpp
  src/db/app_database.h
  src/db/repositories.cpp
  src/db/repositories.h
  src/hls_server.cpp
  src/hls_server.h
  src/hwprobe.cpp
  src/hwprobe.h
  src/playback_backend.h
  src/backend_factory.cpp
  src/ffmpeg_selftest.cpp
  src/ffmpeg_selftest.h
  src/playerwidget.cpp
  src/playerwidget.h
  src/player_controller.cpp
  src/player_controller.h
  src/player_selftest.cpp
  src/player_selftest.h
  src/debug.h
  src/yt.cpp
  src/yt.h
  src/yt_innertube.cpp
  src/yt_innertube.h
  src/mainwindow.cpp
  src/mainwindow.h
  src/ui/theme.cpp
  src/ui/theme.h
  src/ui/format.cpp
  src/ui/format.h
  src/ui/icons.cpp
  src/ui/icons.h
  src/ui/thumbnail_cache.cpp
  src/ui/thumbnail_cache.h
  src/ui/channel_row.cpp
  src/ui/channel_row.h
  src/ui/video_list_view.cpp
  src/ui/video_list_view.h
  src/ui/flow_layout.cpp
  src/ui/flow_layout.h
  src/ui/pages/home_page.cpp
  src/ui/pages/home_page.h
  src/ui/pages/channel_page.cpp
  src/ui/pages/channel_page.h
  src/ui/pages/search_page.cpp
  src/ui/pages/search_page.h
  src/ui/pages/watch_page.cpp
  src/ui/pages/watch_page.h
  src/ui/pages/library_page.cpp
  src/ui/pages/library_page.h
  src/ui/pages/playlist_page.cpp
  src/ui/pages/playlist_page.h
  src/ui/pages/history_page.cpp
  src/ui/pages/history_page.h
  src/ui/pages/settings_page.cpp
  src/ui/pages/settings_page.h
  src/ui/pages/placeholder_page.cpp
  src/ui/pages/placeholder_page.h
  resources/resources.qrc
)

# mpv_backend solo se compila donde mpv enlaza (Linux y Windows x64); el
# build x86 (NEVERTUBE_X86_FFMPEG_BUILD) usa solo el FfmpegBackend.
if(NOT NEVERTUBE_X86_FFMPEG_BUILD)
  target_sources(nevertube_qt2 PRIVATE
    src/mpv_backend.cpp
    src/mpv_backend.h)
endif()

target_link_libraries(nevertube_qt2 PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Network
  Qt5::Sql
  OpenGL::GL
)
if(NOT NEVERTUBE_X86_FFMPEG_BUILD)
  target_link_libraries(nevertube_qt2 PRIVATE MPV::MPV)
endif()

# --- FfmpegBackend (reproductor barato, Fases 2-5) ---
# La x64 usa mpv SIEMPRE en runtime (pieza protegida); el FfmpegBackend se
# COMPILA cuando hay FFmpeg para poder desarrollarlo/verificarlo:
#   - Linux: detección por pkg-config (libavformat/codec/util/swscale/
#     swresample + pulse-simple). Aquí se compila y se valida con
#     --selftest-ffmpeg; el runtime por defecto sigue siendo mpv
#     (NEVERTUBE_BACKEND=ffmpeg solo para pruebas de desarrollo).
#   - Windows x86 (Win7, Fase 5): el ÚNICO motor del build. Se enlaza
#     contra el dev-package FFmpeg Win32 (FFMPEG_DEV_DIR: include/ + lib/ +
#     DLLs) y Qt5 Multimedia (QAudioSink del backend). El build x64 NO se toca.
if(NEVERTUBE_X86_FFMPEG_BUILD)
  # ---- Windows x86 (Win7): FFmpeg dev por FFMPEG_DEV_DIR ----
  # Dev-package Win32 (headers *.h + import-libs *.lib + DLLs), misma mecánica
  # que MPV_DEV_DIR. Sin pkg-config en Windows: find_path/find_library.
  # El paquete vendorizado (i686, LGPL + schannel + DXVA2, cross-compilado con
  # tools/build_ffmpeg_i686.sh) vive en third_party/ffmpeg-win32/ y es el valor
  # por defecto; -DFFMPEG_DEV_DIR permite apuntar a otro.
  set(FFMPEG_DEV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-win32"
      CACHE PATH "Directorio del dev-package FFmpeg Win32 (include/ + lib/ + DLLs)")
  if(NOT EXISTS "${FFMPEG_DEV_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "ffmpeg-dev no hallado en '${FFMPEG_DEV_DIR}': falta include/libavcodec/avcodec.h. Genera el paquete con tools/build_ffmpeg_i686.sh o pasa -DFFMPEG_DEV_DIR=<ruta> (BUILD_WINDOWS.md)")
  endif()
  find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES include NO_DEFAULT_PATH)
  find_library(FFMPEG_AVCODEC_LIB NAMES avcodec.lib avcodec PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  find_library(FFMPEG_AVFORMAT_LIB NAMES avformat.lib avformat PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  find_library(FFMPEG_AVUTIL_LIB NAMES avutil.lib avutil PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  find_library(FFMPEG_SWSCALE_LIB NAMES swscale.lib swscale PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  find_library(FFMPEG_SWRESAMPLE_LIB NAMES swresample.lib swresample PATHS "${FFMPEG_DEV_DIR}" PATH_SUFFIXES lib "" NO_DEFAULT_PATH)
  if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC_LIB OR NOT FFMPEG_AVFORMAT_LIB OR NOT FFMPEG_AVUTIL_LIB OR NOT FFMPEG_SWSCALE_LIB OR NOT FFMPEG_SWRESAMPLE_LIB)
    message(FATAL_ERROR "ffmpeg-dev incompleto en '${FFMPEG_DEV_DIR}': falta include/libavcodec/avcodec.h (got '${FFMPEG_INCLUDE_DIR}') o las .lib avcodec/avformat/avutil/swscale/swresample. Revisa BUILD_WINDOWS.md")
  endif()
  add_library(FFMPEG::AV INTERFACE IMPORTED)
  set_target_properties(FFMPEG::AV PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${FFMPEG_AVCODEC_LIB};${FFMPEG_AVFORMAT_LIB};${FFMPEG_AVUTIL_LIB};${FFMPEG_SWSCALE_LIB};${FFMPEG_SWRESAMPLE_LIB}")
  find_package(Qt5 REQUIRED COMPONENTS Multimedia)
  target_sources(nevertube_qt2 PRIVATE
    src/ffmpeg_backend.cpp
    src/ffmpeg_backend.h)
  target_compile_definitions(nevertube_qt2 PRIVATE
    NEVERTUBE_HAS_FFMPEG_BACKEND=1
    NEVERTUBE_NO_MPV=1)
  target_link_libraries(nevertube_qt2 PRIVATE FFMPEG::AV Qt5::Multimedia)
elseif(NOT WIN32)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(AV QUIET IMPORTED_TARGET
      libavformat libavcodec libavutil libswscale libswresample)
    pkg_check_modules(PULSE QUIET IMPORTED_TARGET libpulse-simple)
  endif()
  if(TARGET PkgConfig::AV)
    message(STATUS "FFmpeg encontrado: compilando FfmpegBackend (Fase 2)")
    target_sources(nevertube_qt2 PRIVATE
      src/ffmpeg_backend.cpp
      src/ffmpeg_backend.h)
    target_compile_definitions(nevertube_qt2 PRIVATE
      NEVERTUBE_HAS_FFMPEG_BACKEND=1)
    target_link_libraries(nevertube_qt2 PRIVATE PkgConfig::AV)
    if(TARGET PkgConfig::PULSE)
      message(STATUS "pulse-simple encontrado: audio del FfmpegBackend")
      target_compile_definitions(nevertube_qt2 PRIVATE NEVERTUBE_HAS_PULSE=1)
      target_link_libraries(nevertube_qt2 PRIVATE PkgConfig::PULSE)
    endif()
  else()
    message(STATUS "FFmpeg no encontrado: sin FfmpegBackend (solo mpv)")
  endif()
endif()

if(WIN32)
  set_target_properties(nevertube_qt2 PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    # Qt5.15.2, qlist.h:915 (QList::operator==): usa stdext::make_checked_array_iterator
    # vía QT_MAKE_CHECKED_ARRAY_ITERATOR de forma INCONDICIONAL en MSVC. VS2026
    # (MSVC 14.5x, _MSC_VER >= 1950, la única imagen que deja GitHub) ELIMINÓ ese
    # símbolo de su STL => C2653/C3861 en cualquier TU que instancie QList==.
    # Fix: forzar inclusión de un shim que lo reimplementa devolviendo el puntero
    # crudo (idéntico a la rama no-MSVC de Qt). Es un no-op fuera de MSVC>=1950.
    # /FI (force-include): cabe antes de TODO cabecera Qt, sin tocar el reproductor.
    # /utf-8: los .cpp son UTF-8 SIN BOM y MSVC por defecto los lee como CP1252
    # (codepage del sistema). QStringLiteral compila a u"..." (UTF-16): sin
    # /utf-8, "á"/"ñ" (bytes 0xC3+) se transforman con la página de códigos
    # equivocada y salen como "Ã¡"/"Ã±" (mojibake) en Windows; en Linux
    # (GCC) con UTF-8 iba bien. /utf-8 fuerza source+execution charset a UTF-8.
    target_compile_options(nevertube_qt2 PRIVATE
      "/utf-8"
      "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_stdext_shim.h"
      "/DWINVER=0x0601" "/D_WIN32_WINNT=0x0601")
    # NOTA: _ITERATOR_DEBUG_LEVEL=0 NO arregla esto (el símbolo no depende de ese
    # macro); se deja de usar en favor del shim.
  endif()
  # Compilación dirigida a Windows 7 (SP1): evita que el linker genere importaciones
  # de funciones de Windows 10/11 (como AdjustWindowRectExForDpi, GetSystemTimePreciseAsFileTime)
  # que hacen que nevertube_qt2.exe falle en el target con "Entry Point Not Found".
  if(WIN32)
    target_compile_definitions(nevertube_qt2 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
  endif()
endif()