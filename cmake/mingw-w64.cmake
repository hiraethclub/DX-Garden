# Cross-compiling for 64-bit Windows from Linux with MinGW-w64:
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(DXG_MINGW_PREFIX x86_64-w64-mingw32)
# The -posix variant gives C11/pthread threads, which curl and SDL expect.
set(CMAKE_C_COMPILER ${DXG_MINGW_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${DXG_MINGW_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER ${DXG_MINGW_PREFIX}-windres)
# Libraries built by tools/build_win_deps.sh land here.
set(DXG_WIN_DEPS "${CMAKE_CURRENT_LIST_DIR}/../third_party/win64" CACHE PATH "Windows dependency prefix")
set(CMAKE_FIND_ROOT_PATH /usr/${DXG_MINGW_PREFIX} ${DXG_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
# Link the MinGW runtime (libgcc, libwinpthread) in, so the .exe runs
# without extra DLLs beside it.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
