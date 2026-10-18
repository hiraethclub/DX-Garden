cmake_minimum_required(VERSION 3.16)

# Two build trees from one source:
#   Vita:  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   Linux: cmake -S . -B build-test   (core/ unit tests only)
project(dxgarden C)

# Bump for every build handed over for testing. Format ##.## (Vita APP_VER).
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" DXG_VERSION LIMIT_COUNT 1)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CORE_SOURCES
    core/config.c
    core/library.c
    core/load_report.c
    core/playlist.c
    core/ring_buffer.c
    core/station.c
    core/stations_file.c
    core/stream_buffer.c
    core/strmap.c
    core/text.c
    core/userlist.c
)

if(VITA)
    include("${VITASDK}/share/vita.cmake" REQUIRED)

    set(VITA_APP_NAME "DX Garden")
    set(VITA_TITLEID  "DXGA00001")

    add_executable(dxgarden
        ${CORE_SOURCES}
        vita/app.c
        vita/data.c
        vita/fs.c
        vita/gfx.c
        vita/ime.c
        vita/log.c
        vita/main.c
        vita/player.c
    )
    target_include_directories(dxgarden PRIVATE core)
    target_compile_definitions(dxgarden PRIVATE DXG_VERSION="${DXG_VERSION}")
    # vita/ uses POSIX bits (strncasecmp) from newlib.
    set_target_properties(dxgarden PROPERTIES C_EXTENSIONS ON)
    target_compile_options(dxgarden PRIVATE -Wall -Wextra -O2)

    target_link_libraries(dxgarden
        vita2d
        curl mbedtls mbedx509 mbedcrypto zstd
        mpg123
        freetype bz2 png jpeg z m
        pthread
        SceAppMgr_stub
        SceAppUtil_stub
        SceAudio_stub
        SceCommonDialog_stub
        SceCtrl_stub
        SceDisplay_stub
        SceGxm_stub
        SceNet_stub
        SceNetCtl_stub
        ScePgf_stub
        ScePower_stub
        SceRtc_stub
        SceSysmodule_stub
    )

    vita_create_self(eboot.bin dxgarden)
    vita_create_vpk(dxgarden-${DXG_VERSION}.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${DXG_VERSION}
        NAME ${VITA_APP_NAME}
        FILE sce_sys/icon0.png sce_sys/icon0.png
        FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
        FILE assets/cacert.pem cacert.pem
        FILE assets/fonts/NotoSans-Regular.ttf fonts/NotoSans-Regular.ttf
        FILE assets/fonts/NotoSans-Bold.ttf fonts/NotoSans-Bold.ttf
        FILE assets/fonts/OFL.txt fonts/OFL.txt
    )
else()
    add_executable(dxgarden_test
        ${CORE_SOURCES}
        tests/test_main.c
        tests/test_library.c
    )
    target_include_directories(dxgarden_test PRIVATE core)
    target_compile_options(dxgarden_test PRIVATE -Wall -Wextra -Wpedantic -g
                           -fsanitize=address,undefined)
    target_link_options(dxgarden_test PRIVATE -fsanitize=address,undefined)
    target_link_libraries(dxgarden_test m)

    enable_testing()
    add_test(NAME core COMMAND dxgarden_test)
endif()
