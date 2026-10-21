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
    core/bidi_order.c
    core/config.c
    core/controls.c
    core/globe.c
    core/icy.c
    core/library.c
    core/load_report.c
    core/playlist.c
    core/ring_buffer.c
    core/search.c
    core/station.c
    core/stations_file.c
    core/stream_buffer.c
    core/stream_info.c
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
        vita/font.c
        vita/fs.c
        vita/gfx.c
        vita/gfx_fallback.c
        vita/globe_view.c
        vita/ime.c
        vita/log.c
        vita/main.c
        vita/player.c
    )
    target_include_directories(dxgarden PRIVATE core "${VITASDK}/arm-vita-eabi/include/freetype2"
                               "${VITASDK}/arm-vita-eabi/include/harfbuzz"
                               "${VITASDK}/arm-vita-eabi/include/fribidi"
                               "${CMAKE_BINARY_DIR}/generated")
    # revision.h: the git commit, refreshed on every build (shown in About).
    add_custom_target(dxg_revision
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
                -DOUT=${CMAKE_BINARY_DIR}/generated/revision.h
                -P ${CMAKE_SOURCE_DIR}/cmake/revision.cmake
        BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/revision.h)
    add_dependencies(dxgarden dxg_revision)
    target_compile_definitions(dxgarden PRIVATE DXG_VERSION="${DXG_VERSION}")
    # vita/ uses POSIX bits (strncasecmp) from newlib.
    set_target_properties(dxgarden PROPERTIES C_EXTENSIONS ON)
    target_compile_options(dxgarden PRIVATE -Wall -Wextra -O2)
    # Keeps room after the code segment for vita-elf-create (see the file).
    target_link_options(dxgarden PRIVATE "-Wl,-T,${CMAKE_SOURCE_DIR}/cmake/vita-gap.ld")

    target_link_libraries(dxgarden
        vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
        vita2d
        curl mbedtls mbedx509 mbedcrypto zstd
        mpg123
        harfbuzz fribidi freetype bz2 png jpeg z m
        pthread
        SceAppMgr_stub
        SceAppUtil_stub
        SceAudio_stub
        SceCommonDialog_stub
        SceCtrl_stub
        SceDisplay_stub
        SceGxm_stub
        SceIme_stub
        SceKernelDmacMgr_stub
        SceNet_stub
        SceNetCtl_stub
        ScePgf_stub
        ScePower_stub
        SceRtc_stub
        SceSysmodule_stub
        SceTouch_stub
        stdc++
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
        FILE assets/stations/stations.txt stations.txt
        FILE assets/fonts/NotoSans-Regular.ttf fonts/NotoSans-Regular.ttf
        FILE assets/fonts/NotoSans-Bold.ttf fonts/NotoSans-Bold.ttf
        FILE assets/fonts/OFL.txt fonts/OFL.txt
        FILE assets/fonts/NotoSansArabic-Regular.ttf fonts/NotoSansArabic-Regular.ttf
        FILE assets/fonts/NotoSansHebrew-Regular.ttf fonts/NotoSansHebrew-Regular.ttf
        FILE assets/fonts/NotoSansThai-Regular.ttf fonts/NotoSansThai-Regular.ttf
        FILE assets/fonts/NotoSansGeorgian-Regular.ttf fonts/NotoSansGeorgian-Regular.ttf
        FILE assets/fonts/NotoSansEthiopic-Regular.ttf fonts/NotoSansEthiopic-Regular.ttf
        FILE assets/fonts/NotoSansCJKsc-Subset.otf fonts/NotoSansCJKsc-Subset.otf
        FILE assets/map/world.png map/world.png
        FILE assets/map/lines.bin map/lines.bin
    )
else()
    add_executable(dxgarden_test
        ${CORE_SOURCES}
        tests/test_main.c
        tests/test_library.c
        tests/test_globe.c
        tests/test_controls.c
        tests/test_stationfiles.c
        tests/test_bidi.c
        tests/test_stream.c
    )
    target_include_directories(dxgarden_test PRIVATE core)
    target_compile_definitions(dxgarden_test PRIVATE
                               DXG_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
    target_compile_options(dxgarden_test PRIVATE -Wall -Wextra -Wpedantic -g
                           -fsanitize=address,undefined)
    target_link_options(dxgarden_test PRIVATE -fsanitize=address,undefined)
    target_link_libraries(dxgarden_test m)

    enable_testing()
    add_test(NAME core COMMAND dxgarden_test)
endif()
