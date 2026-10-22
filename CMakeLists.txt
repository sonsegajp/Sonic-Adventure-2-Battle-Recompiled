cmake_minimum_required(VERSION 3.25)
project(sa2brecomp LANGUAGES C CXX VERSION 1.0.0)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The recompiler: main.dol + REL modules -> C++ (docs/PLAN.md). Linked into the developer CLI and
# into the launcher, which runs it at first-run setup (docs/SETUP.md).
add_library(sa2_recompiler STATIC
    src/recompiler/image.cpp
    src/recompiler/analyze.cpp
    src/recompiler/emit.cpp
    src/recompiler/build.cpp)
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
# First-run setup builds the game module from the player's disc (src/launcher/setup.cpp): the files
# it compiles the module with are embedded in the launcher.
set(_embed_out ${CMAKE_BINARY_DIR}/generated/setup_files.inc)
add_custom_command(OUTPUT ${_embed_out}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_embed_out} -DROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/tools/launcher/embed.cmake
    DEPENDS tools/launcher/embed.cmake src/runtime/ctx.h src/runtime/modules.h src/runtime/abi.h src/game/glue.cpp
            config/GSNE8P/names.txt config/GSNE8P/patchpoints.txt
    COMMENT "Embedding the game module's sources in the launcher")
add_executable(sa2b_launcher WIN32 src/launcher/main.cpp src/launcher/disc.cpp src/launcher/setup.cpp ${_embed_out} ${SA2_ICON_RC})
set_target_properties(sa2b_launcher PROPERTIES OUTPUT_NAME "SA2BLauncher")
target_include_directories(sa2b_launcher PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(sa2b_launcher PRIVATE SA2B_VERSION="${PROJECT_VERSION}")
target_link_libraries(sa2b_launcher PRIVATE ${AURORA_SDL3_TARGET} sa2_recompiler)
if (WIN32)
    add_custom_command(TARGET sa2b_launcher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:sa2b_launcher> $<TARGET_FILE_DIR:sa2b_launcher>
        COMMAND_EXPAND_LISTS)
endif ()

# ---- The program (sa2b): the runtime. The game's code is not in it: it loads sa2b_game.dll, which
# the launcher builds on the player's machine from their own disc (no translated code ships) -----
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp src/modhost/*.cpp)
add_executable(sa2b ${RUNTIME_SOURCES} ${SA2_ICON_RC})
target_include_directories(sa2b PRIVATE src)
target_compile_options(sa2b PRIVATE /EHsc)
# The game module imports the runtime's functions (RT_API, src/runtime/ctx.h): sa2b.lib.
set_target_properties(sa2b PROPERTIES ENABLE_EXPORTS ON)
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

# ---- Developer build of the game module (sa2b_game.dll) from the recompiler's output -----------
# Generate with: sa2recompile --dol orig/main.dol --rels orig/rel --names config/GSNE8P/names.txt
#                --out build/gen      (nothing generated is ever committed)
# Built the way the launcher builds it (src/launcher/setup.cpp): freestanding, no C library.
set(SA2_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
file(GLOB SA2_GEN_SOURCES CONFIGURE_DEPENDS ${SA2_GEN_DIR}/*.cpp)
if (SA2_GEN_SOURCES)
    add_library(sa2b_game SHARED ${SA2_GEN_SOURCES} src/game/glue.cpp)
    target_include_directories(sa2b_game PRIVATE src ${SA2_GEN_DIR})
    target_compile_definitions(sa2b_game PRIVATE SA2B_GAME_DLL)
    # Generated code: one big function per guest function with gotos; no debug info (it is huge).
    target_compile_options(sa2b_game PRIVATE /O2 /clang:-g0 /clang:-ffreestanding /GS- /Zl /GR- /EHs-c-
        -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
    target_link_options(sa2b_game PRIVATE /NOENTRY /NODEFAULTLIB)
    target_link_libraries(sa2b_game PRIVATE sa2b)
    set_target_properties(sa2b_game PROPERTIES MSVC_RUNTIME_LIBRARY "")
else ()
    message(STATUS "No recompiler output in ${SA2_GEN_DIR}; skipping the developer build of sa2b_game.dll")
endif ()
