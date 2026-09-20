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
if(WIN32)
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
else()
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

target_link_libraries(nevertube_qt2 PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Network
  Qt5::Sql
  OpenGL::GL
  MPV::MPV
)

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
    target_compile_options(nevertube_qt2 PRIVATE
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