cmake_minimum_required(VERSION 3.25)
project(sa2brecomp LANGUAGES C CXX VERSION 0.1)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The recompiler: main.dol + REL modules -> C++ (docs/PLAN.md). Linked into the developer CLI and,
# later, into the shipped program for first-run setup.
add_library(sa2_recompiler STATIC
    src/recompiler/image.cpp
    src/recompiler/analyze.cpp
    src/recompiler/emit.cpp)
target_include_directories(sa2_recompiler PUBLIC src)

add_executable(sa2recompile tools/recomp/sa2recompile.cpp)
target_link_libraries(sa2recompile PRIVATE sa2_recompiler)

# ---- Aurora: GX renderer (WebGPU/Dawn), window, controllers (extern/aurora, MIT) ---------------
# The disc is read by our own DVD layer and saves will go through our CARD layer, so Aurora's DVD
# (nod) and CARD components are off.
# Local changes to Aurora are kept as patches (patches/aurora) so the submodule stays pristine.
# The series is applied to a pristine checkout only; a checkout with changes is never touched (it
# is the patched tree, perhaps with work not yet saved as a patch - a reset here once wiped that).
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
set(_aurora ${CMAKE_SOURCE_DIR}/extern/aurora)
execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _dirty)
if (_dirty EQUAL 0)
    # Git for Windows' core.autocrlf=true checks the submodule out with CRLF and LF patches then
    # fail: switch it off and rewrite the (pristine) files with LF first.
    execute_process(COMMAND git config core.autocrlf false WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git rm -r -q --cached . WORKING_DIRECTORY ${_aurora} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND git reset -q --hard WORKING_DIRECTORY ${_aurora})
    execute_process(COMMAND git checkout-index -f -a WORKING_DIRECTORY ${_aurora})
    foreach (_patch ${AURORA_PATCHES})
        execute_process(COMMAND git apply ${_patch} WORKING_DIRECTORY ${_aurora} RESULT_VARIABLE _result)
        if (NOT _result EQUAL 0)
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endforeach ()
else ()
    message(STATUS "Aurora has local changes: taken as the patched tree (patches/aurora), left as is")
endif ()
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# ---- Launcher: disc, settings, HD textures, mods; drawn with the game's menu art from the disc ----
# The programs' icon, on Windows: Sonic's lives icon from the disc when tools/launcher/mkicon.py
# --disc has made it (build/icon, never committed), else our own (res/sa2b.ico).
set(SA2_ICON_RC "")
if (WIN32)
    if (EXISTS ${CMAKE_SOURCE_DIR}/build/icon/sa2b.rc)
        set(SA2_ICON_RC ${CMAKE_SOURCE_DIR}/build/icon/sa2b.rc)
    else ()
        set(SA2_ICON_RC ${CMAKE_SOURCE_DIR}/res/sa2b.rc)
    endif ()
endif ()
add_executable(sa2b_launcher WIN32 src/launcher/main.cpp src/launcher/disc.cpp ${SA2_ICON_RC})
set_target_properties(sa2b_launcher PROPERTIES OUTPUT_NAME "SA2BLauncher")
target_include_directories(sa2b_launcher PRIVATE src)
target_link_libraries(sa2b_launcher PRIVATE ${AURORA_SDL3_TARGET})
if (WIN32)
    add_custom_command(TARGET sa2b_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:sa2b_launcher> $<TARGET_FILE_DIR:sa2b_launcher>
        COMMAND_EXPAND_LISTS)
endif ()

# ---- Developer build: runtime + the game recompiled at build time (sa2b) ----------------------
# Generate with: sa2recompile --dol orig/main.dol --rels orig/rel --names config/GSNE8P/names.txt
#                --out build/gen      (nothing generated is ever committed)
set(SA2_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
file(GLOB SA2_GEN_SOURCES CONFIGURE_DEPENDS ${SA2_GEN_DIR}/*.cpp)
if (SA2_GEN_SOURCES)
    file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp src/modhost/*.cpp)
    add_executable(sa2b ${RUNTIME_SOURCES} ${SA2_GEN_SOURCES} ${SA2_ICON_RC})
    target_include_directories(sa2b PRIVATE src ${SA2_GEN_DIR})
    # Generated code: one big function per guest function with gotos; no debug info (it is huge),
    # unwind tables so a guest thread can end by unwinding through it.
    set_source_files_properties(${SA2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
        "/O2;/clang:-g0;/clang:-funwind-tables;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")
    target_compile_options(sa2b PRIVATE /EHsc)
    target_link_libraries(sa2b PRIVATE aurora::core aurora::gx aurora::vi aurora::pad aurora::mtx)
    # The GX bridge feeds Aurora's command FIFO directly (internal headers).
    target_include_directories(sa2b PRIVATE ${CMAKE_SOURCE_DIR}/extern/aurora/lib)
    if (WIN32)
        add_custom_command(TARGET sa2b POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:sa2b> $<TARGET_FILE_DIR:sa2b>
            COMMAND_EXPAND_LISTS)
        # Dawn's D3D12 backend loads the shader compiler DLLs at run time.
        file(GLOB _dawn_dx_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
        if (_dawn_dx_dlls)
            add_custom_command(TARGET sa2b POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_dx_dlls} $<TARGET_FILE_DIR:sa2b>)
        endif ()
    endif ()
else ()
    message(STATUS "No recompiler output in ${SA2_GEN_DIR}; skipping the sa2b developer build")
endif ()
