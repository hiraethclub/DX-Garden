# Writes OUT (a header) with DXG_REVISION: the short git commit the build
# came from, plus "-modified" if tracked files have uncommitted changes.
# Run on every build; the file is only rewritten when the value changes.
execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE rev
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR rev STREQUAL "")
    set(rev "unknown")
else()
    execute_process(COMMAND git status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE dirty
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT dirty STREQUAL "")
        set(rev "${rev}-modified")
    endif()
endif()
set(text "#define DXG_REVISION \"${rev}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
