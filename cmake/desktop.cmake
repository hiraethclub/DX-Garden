# The desktop app (desktop/), included from CMakeLists.txt.
#
#   Windows: cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
#   Linux:   cmake -S . -B build-linux -G Ninja -DDXG_DESKTOP=ON
#
# Both need tools/build_win_deps.sh run first (win64 or linux).
set(DXG_LINUX_DEPS "${CMAKE_SOURCE_DIR}/third_party/linux" CACHE PATH "Linux dependency prefix")
if(NOT WIN32)
    list(PREPEND CMAKE_PREFIX_PATH "${DXG_LINUX_DEPS}")
endif()

find_package(SDL3 CONFIG REQUIRED COMPONENTS SDL3-static)
find_package(PkgConfig REQUIRED)
if(WIN32)
    # Static libraries only: the .exe ships without DLLs.
    find_package(CURL CONFIG REQUIRED)
    set(DXG_CURL CURL::libcurl_static)
    find_library(MPG123_LIB NAMES libmpg123.a mpg123 REQUIRED)
    set(DXG_MPG123 ${MPG123_LIB} shlwapi)
    set(DXG_PLATFORM_NAME "Windows")
else()
    pkg_check_modules(DXG_PC REQUIRED IMPORTED_TARGET libcurl libmpg123)
    set(DXG_CURL PkgConfig::DXG_PC)
    set(DXG_MPG123 "")
    set(DXG_PLATFORM_NAME "Linux")
endif()

add_executable(dxgarden_desktop WIN32
    ${CORE_SOURCES}
    desktop/log.c
    desktop/main.c
    desktop/player.c
)
set_target_properties(dxgarden_desktop PROPERTIES OUTPUT_NAME dxgarden C_EXTENSIONS ON)
target_include_directories(dxgarden_desktop PRIVATE core desktop)
target_compile_definitions(dxgarden_desktop PRIVATE DXG_VERSION="${DXG_VERSION}"
                           DXG_BUILD="${DXG_BUILD}" DXG_PLATFORM_NAME="${DXG_PLATFORM_NAME}")
target_compile_options(dxgarden_desktop PRIVATE -Wall -Wextra -O2 -g)
target_link_libraries(dxgarden_desktop PRIVATE SDL3::SDL3-static ${DXG_CURL} ${DXG_MPG123} m)
if(WIN32)
    target_compile_definitions(dxgarden_desktop PRIVATE CURL_STATICLIB WIN32_LEAN_AND_MEAN)
endif()
