cmake_minimum_required(VERSION 3.16)
project(nevertube_qt2 VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Restrict the OS dependency to Windows 7 (WINVER=0x0601 / WIN32_WINNT=0x0601)
# so the binary does not try to import new Win8+ functions such as AdjustWindowRectExForDpi,
# which do not exist in Win7 SP1's USER32.dll.
add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
find_package(OpenGL REQUIRED)
# pkg-config is used ONLY for mpv on Linux (PkgConfig::MPV). On Windows there
# is NO pkg-config and mpv goes through MPV_DEV_DIR (find_path/find_library), so
# find_package(PkgConfig) must live in the else() to avoid breaking the
# WIN32 configure (no pkg-config in the runner → "Could NOT find PkgConfig").
# mpv: on Linux it is found with pkg-config (mpv.pc, target PkgConfig::MPV). On
# Windows there is NO usable CONFIG in the vcpkg registry (the `mpv` port was removed:
# `error: mpv does not exist`), so it is linked against the official dev-package
# from mpv.io (shinchiro/mpv-winbuild-cmake, `mpv-dev-*.7z`): headers `mpv/*.h` +
# import-lib `mpv-2.lib` + `mpv-2.dll`, path in the MPV_DEV_DIR variable
# (BUILD_WINDOWS.md / workflow windows). It is unified into MPV::MPV so that
# target_link_libraries does not depend on the platform. This machine (Linux)
# compiles/verifies the else() branch; the if(WIN32) is build-config and the
# final Windows build runs on the user's machine/CI.
#
# Phase 5: the Windows x86 build (Win7) does NOT link mpv — its only
# engine is the FfmpegBackend (FFMPEG_DEV_DIR + Qt5 Multimedia, further below).
# Everything else (Linux and Windows x64) keeps mpv exactly as it was.
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
  src/i18n.cpp
  src/i18n.h
  src/ui/scan.h
  src/ui/theme.cpp
  src/ui/theme.h
  src/ui/format.cpp
  src/ui/format.h
  src/ui/icons.cpp
  src/ui/icons.h
  src/ui/player_glyphs.cpp
  src/ui/player_glyphs.h
  src/ui/thumbnail_cache.cpp
  src/ui/thumbnail_cache.h
  src/ui/channel_row.cpp
  src/ui/channel_row.h
  src/ui/video_list_view.cpp
  src/ui/video_list_view.h
  src/ui/row_menu.cpp
  src/ui/row_menu.h
  src/ui/live_detect.h
  src/ui/flow_layout.cpp
  src/ui/flow_layout.h
  src/ui/confirm_dialog.cpp
  src/ui/confirm_dialog.h
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
  localization/localization.qrc
)

# mpv_backend is only compiled where mpv links (Linux and Windows x64); the
# x86 build (NEVERTUBE_X86_FFMPEG_BUILD) uses only the FfmpegBackend.
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

# --- FfmpegBackend (cheap player, Phases 2-5) ---
# x64 uses mpv ALWAYS at runtime (protected piece); the FfmpegBackend is
# COMPILED when FFmpeg is available so it can be developed/verified:
#   - Linux: detection via pkg-config (libavformat/codec/util/swscale/
#     swresample + pulse-simple). Here it is compiled and validated with
#     --selftest-ffmpeg; the default runtime is still mpv
#     (NEVERTUBE_BACKEND=ffmpeg only for development tests).
#   - Windows x86 (Win7, Phase 5): the ONLY engine of the build. It links
#     against the FFmpeg Win32 dev-package (FFMPEG_DEV_DIR: include/ + lib/ +
#     DLLs) and Qt5 Multimedia (the backend's QAudioSink). The x64 build is NOT touched.
if(NEVERTUBE_X86_FFMPEG_BUILD)
  # ---- Windows x86 (Win7): FFmpeg dev via FFMPEG_DEV_DIR ----
  # Win32 dev-package (headers *.h + import-libs *.lib + DLLs), same mechanics
  # as MPV_DEV_DIR. No pkg-config on Windows: find_path/find_library.
  # The vendored package (i686, LGPL + schannel + DXVA2, cross-compiled with
  # tools/build_ffmpeg_i686.sh) lives in third_party/ffmpeg-win32/ and is the
  # default value; -DFFMPEG_DEV_DIR lets you point to another one.
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
  # Icon of the .exe (Explorer, taskbar, apps list). nt.rc
  # references "icon/nt.ico" and rc.exe resolves it with the include dir added
  # here; the multires .ico is generated by tools/gen_app_icon.py.
  # Win7 (0x0601) and Win10+ read the 7 entries without issue.
  target_include_directories(nevertube_qt2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources")
  target_sources(nevertube_qt2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/nt.rc")
  if(MSVC)
    # Qt5.15.2, qlist.h:915 (QList::operator==): uses stdext::make_checked_array_iterator
    # via QT_MAKE_CHECKED_ARRAY_ITERATOR UNCONDITIONALLY on MSVC. VS2026
    # (MSVC 14.5x, _MSC_VER >= 1950, the only image GitHub leaves) REMOVED that
    # symbol from its STL => C2653/C3861 in any TU that instantiates QList==.
    # Fix: force-include a shim that reimplements it returning the raw
    # pointer (identical to Qt's non-MSVC branch). It is a no-op outside MSVC>=1950.
    # /FI (force-include): fits before EVERY Qt header, without touching the player.
    # /utf-8: the .cpp files are UTF-8 WITHOUT BOM and MSVC reads them as CP1252 by default
    # (system codepage). QStringLiteral compiles to u"..." (UTF-16): without
    # /utf-8, "á"/"ñ" (bytes 0xC3+) are transformed with the wrong code
    # page and come out as "Ã¡"/"Ã±" (mojibake) on Windows; on Linux
    # (GCC) with UTF-8 it was fine. /utf-8 forces source+execution charset to UTF-8.
    target_compile_options(nevertube_qt2 PRIVATE
      "/utf-8"
      "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_stdext_shim.h"
      "/DWINVER=0x0601" "/D_WIN32_WINNT=0x0601")
    # NOTE: _ITERATOR_DEBUG_LEVEL=0 does NOT fix this (the symbol does not depend on that
    # macro); it is dropped in favor of the shim.
  endif()
  # Compilation targeting Windows 7 (SP1): prevents the linker from generating imports
  # of Windows 10/11 functions (such as AdjustWindowRectExForDpi, GetSystemTimePreciseAsFileTime)
  # that make nevertube_qt2.exe fail on the target with "Entry Point Not Found".
  if(WIN32)
    target_compile_definitions(nevertube_qt2 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
  endif()
endif()