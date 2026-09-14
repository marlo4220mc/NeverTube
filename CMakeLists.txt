cmake_minimum_required(VERSION 3.16)
project(nevertube_qt2 VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)

add_executable(nevertube_qt2
  src/main.cpp
  src/models.h
  src/db/app_database.cpp
  src/db/app_database.h
  src/db/repositories.cpp
  src/db/repositories.h
  src/hls_server.cpp
  src/hls_server.h
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
  src/ui/video_list_view.cpp
  src/ui/video_list_view.h
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
  PkgConfig::MPV
)

if(WIN32)
  set_target_properties(nevertube_qt2 PROPERTIES WIN32_EXECUTABLE TRUE)
endif()