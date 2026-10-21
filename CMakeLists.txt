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

# ---- Developer build: runtime + the game recompiled at build time (sa2b) ----------------------
# Generate with: sa2recompile --dol orig/main.dol --rels orig/rel --names config/GSNE8P/names.txt
#                --out build/gen      (nothing generated is ever committed)
set(SA2_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Recompiler output for the developer build")
file(GLOB SA2_GEN_SOURCES CONFIGURE_DEPENDS ${SA2_GEN_DIR}/*.cpp)
if (SA2_GEN_SOURCES)
    file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/*.cpp)
    add_executable(sa2b ${RUNTIME_SOURCES} ${SA2_GEN_SOURCES})
    target_include_directories(sa2b PRIVATE src ${SA2_GEN_DIR})
    # Generated code: one big function per guest function with gotos; no debug info (it is huge),
    # unwind tables so a guest thread can end by unwinding through it.
    set_source_files_properties(${SA2_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
        "/O2;/clang:-g0;/clang:-funwind-tables;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")
    target_compile_options(sa2b PRIVATE /EHsc)
else ()
    message(STATUS "No recompiler output in ${SA2_GEN_DIR}; skipping the sa2b developer build")
endif ()
