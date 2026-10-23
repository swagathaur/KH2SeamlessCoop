# KH2FM Co-op Recomp: native port of Kingdom Hearts II Final Mix (PS2, SLPM_666.75)
# by static recompilation. Licensed GPL-3.0. Generated game code (generated/) is
# produced locally from the user's own disc and never committed.
#
# Build structure follows DQ8Recomp (GPL-3.0): the PS2Recomp runtime as a
# subproject, the translated corpus as static libraries, overlays as separate
# libraries dispatched at runtime.

cmake_minimum_required(VERSION 3.24)
project(KH2CoopRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(KH2_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH
    "ps2_recomp output root (main/, ovl_<name>/, overlays/)")
set(KH2_OVERLAYS title movie shop gumimenu gumibattle)
set(PS2RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PS2Recomp")

# --- Local patches to the PS2Recomp fork (patches/ps2recomp/*.patch) --------
find_package(Git REQUIRED)
file(GLOB KH2_RUNTIME_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/ps2recomp/*.patch")
list(SORT KH2_RUNTIME_PATCHES)
# Applied patches are recorded (name + hash) in a stamp inside the fork, with a
# copy of each. On configure, applied patches from the first one that changed
# or disappeared onwards are reversed (newest first, from the saved copies),
# then the rest of the list is applied, so files no patch touched keep their
# timestamps. Without a stamp the fork is reset to its commit and everything
# applied. (A per-patch "reverse --check" is not enough: a later patch touching
# nearby lines makes an earlier one look unapplied.)
set(KH2_PATCH_STAMP "${PS2RECOMP_DIR}/.kh2-applied-patches")
set(KH2_PATCH_COPIES "${PS2RECOMP_DIR}/.kh2-applied")
set(kh2_wanted "")
foreach(patch IN LISTS KH2_RUNTIME_PATCHES)
    get_filename_component(name "${patch}" NAME)
    file(SHA256 "${patch}" hash)
    list(APPEND kh2_wanted "${name}=${hash}")
endforeach()
list(LENGTH kh2_wanted kh2_wanted_count)
set(kh2_keep 0)
if(EXISTS "${KH2_PATCH_STAMP}")
    file(STRINGS "${KH2_PATCH_STAMP}" kh2_applied)
    list(LENGTH kh2_applied kh2_applied_count)
    # Longest common prefix of applied and wanted.
    set(kh2_matching TRUE)
    foreach(i RANGE ${kh2_applied_count})
        if(kh2_matching AND i LESS kh2_applied_count AND i LESS kh2_wanted_count)
            list(GET kh2_applied ${i} have)
            list(GET kh2_wanted ${i} want)
            if(have STREQUAL want)
                math(EXPR kh2_keep "${i} + 1")
            else()
                set(kh2_matching FALSE)
            endif()
        else()
            set(kh2_matching FALSE)
        endif()
    endforeach()
    # Reverse what is applied beyond the common prefix, newest first.
    if(kh2_keep LESS kh2_applied_count)
        math(EXPR last "${kh2_applied_count} - 1")
        foreach(i RANGE ${last} ${kh2_keep} -1)
            execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse "${KH2_PATCH_COPIES}/${i}.patch"
                WORKING_DIRECTORY "${PS2RECOMP_DIR}" RESULT_VARIABLE reversed)
            if(NOT reversed EQUAL 0)
                message(FATAL_ERROR "Could not reverse saved patch ${i}; delete ${KH2_PATCH_STAMP} to start over")
            endif()
            list(GET kh2_applied ${i} entry)
            message(STATUS "kh2: reversed ${entry}")
        endforeach()
    endif()
    file(WRITE "${KH2_PATCH_STAMP}" "")
    if(kh2_keep GREATER 0)
        math(EXPR last "${kh2_keep} - 1")
        foreach(i RANGE ${last})
            list(GET kh2_applied ${i} entry)
            file(APPEND "${KH2_PATCH_STAMP}" "${entry}\n")
        endforeach()
    endif()
else()
    message(STATUS "kh2: no patch stamp; resetting third_party/PS2Recomp")
    execute_process(COMMAND "${GIT_EXECUTABLE}" checkout -- .
        WORKING_DIRECTORY "${PS2RECOMP_DIR}" RESULT_VARIABLE reset_result)
    if(NOT reset_result EQUAL 0)
        message(FATAL_ERROR "Could not reset third_party/PS2Recomp")
    endif()
    file(REMOVE_RECURSE "${KH2_PATCH_COPIES}")
    file(WRITE "${KH2_PATCH_STAMP}" "")
endif()
file(MAKE_DIRECTORY "${KH2_PATCH_COPIES}")
if(kh2_keep LESS kh2_wanted_count)
    math(EXPR last "${kh2_wanted_count} - 1")
    foreach(i RANGE ${kh2_keep} ${last})
        list(GET KH2_RUNTIME_PATCHES ${i} patch)
        list(GET kh2_wanted ${i} entry)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}"
            WORKING_DIRECTORY "${PS2RECOMP_DIR}" RESULT_VARIABLE applied)
        if(NOT applied EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch} to third_party/PS2Recomp")
        endif()
        file(COPY_FILE "${patch}" "${KH2_PATCH_COPIES}/${i}.patch")
        file(APPEND "${KH2_PATCH_STAMP}" "${entry}\n")
        message(STATUS "kh2: applied ${patch}")
    endforeach()
endif()

# --- PS2Recomp runtime -----------------------------------------------------
# VU1 microprograms compiled to native code (the runtime's
# tools/compile_vu_programs.py) from recordings of a local run with
# PS2_VU_PROGRAM_PROFILE=build/vu-profile (tools/win/vu-record.ps1). Recordings
# are game code and stay local; without them VU1 is interpreted.
set(KH2_VU_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/vu-profile")
if(EXISTS "${KH2_VU_PROFILE_DIR}/entries.txt")
    set(PS2X_VU_PROGRAM_PROFILES "${KH2_VU_PROFILE_DIR}" CACHE STRING "" FORCE)
    message(STATUS "kh2: compiling VU1 programs recorded in ${KH2_VU_PROFILE_DIR}")
endif()
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PS2RECOMP_DIR}" ps2recomp EXCLUDE_FROM_ALL)
if(MSVC AND TARGET ps2_runtime)
    # Whole-program optimisation makes linking a 12k-function corpus take hours
    # (DQ8Recomp measured this); compile the runtime normally instead.
    set_property(TARGET ps2_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    get_target_property(_opts ps2_runtime COMPILE_OPTIONS)
    if(_opts)
        list(REMOVE_ITEM _opts /GL)
        set_property(TARGET ps2_runtime PROPERTY COMPILE_OPTIONS "${_opts}")
    endif()
endif()

set(KH2_RUNTIME_INCLUDES
    "${PS2RECOMP_DIR}/ps2xRuntime/include"
    "${PS2RECOMP_DIR}/ps2xRuntime/include/runtime"
    "${PS2RECOMP_DIR}/ps2xIOP/include"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel")

if(MSVC)
    # /Zc:inline stops every object carrying unused inline runtime/STL code.
    set(KH2_GENERATED_FLAGS /O2 /Zc:inline /bigobj /w)
else()
    # Generated memory accesses alias across widths and guest arithmetic wraps.
    set(KH2_GENERATED_FLAGS -O2 -g0 -fno-strict-aliasing -fwrapv -w)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        list(APPEND KH2_GENERATED_FLAGS -msse4.1)
    endif()
endif()

function(kh2_generated_library target dir)
    file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
    list(REMOVE_ITEM sources ${ARGN})
    list(LENGTH sources count)
    if(count EQUAL 0)
        message(FATAL_ERROR "No generated sources in ${dir}. Run tools/recomp.py first.")
    endif()
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PRIVATE "${dir}" ${KH2_RUNTIME_INCLUDES})
    target_compile_options(${target} PRIVATE ${KH2_GENERATED_FLAGS})
    # Generated files declare only the functions they name (tools/recomp.py
    # slim_includes), so the all-functions headers stay out of the PCH: adding
    # a function no longer forces every object to rebuild.
    target_precompile_headers(${target} PRIVATE
        <ps2_runtime_macros.h> <ps2_runtime.h> <ps2_syscalls.h> <ps2_stubs.h>)
    message(STATUS "kh2: ${target}: ${count} translation units")
endfunction()

# --- Translated main executable -------------------------------------------
kh2_generated_library(kh2_main "${KH2_GENERATED_DIR}/main")

# --- Overlays: own namespace tables instead of register_functions.cpp ------
set(KH2_OVERLAY_LIBS)
foreach(ovl IN LISTS KH2_OVERLAYS)
    set(dir "${KH2_GENERATED_DIR}/ovl_${ovl}")
    kh2_generated_library(kh2_ovl_${ovl} "${dir}" "${dir}/register_functions.cpp")
    list(APPEND KH2_OVERLAY_LIBS kh2_ovl_${ovl})
endforeach()

# --- Switchable game patches (tools/game_patches.py) -----------------------
# Recompiled copies of patched functions; optional (empty table if not built).
if(EXISTS "${KH2_GENERATED_DIR}/patches/game_patch_table.cpp")
    kh2_generated_library(kh2_patches "${KH2_GENERATED_DIR}/patches")
    target_include_directories(kh2_patches PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port/runtime")
    set(KH2_PATCH_SOURCES)
    set(KH2_PATCH_LIBS kh2_patches)
else()
    set(KH2_PATCH_SOURCES port/runtime/game_patch_table_empty.cpp)
    set(KH2_PATCH_LIBS)
endif()

# --- Runner ---------------------------------------------------------------
# --- GPU renderer: paraLLEl-GS (LGPL-3.0+), Vulkan compute GS emulation -------
# Built as a library only (no Granite renderer/platform). Its CMake defaults the
# MSVC runtime to the static CRT; keep the DLL CRT everything else uses.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/parallel-gs EXCLUDE_FROM_ALL)

# --- Dear ImGui (MIT): the in-game settings menu ---------------------------
include(FetchContent)
FetchContent_Declare(kh2_imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(kh2_imgui_src)
# SDL3, for controllers only (PlayStation, Switch and other pads natively,
# with rumble; XInput stays as the fallback). Static, most subsystems off.
option(KH2_SDL3 "Use SDL3 for game controllers" ON)
if(KH2_SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    foreach(_sub AUDIO GPU RENDER CAMERA DIALOG POWER)
        set(SDL_${_sub} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(kh2_sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.28
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(kh2_sdl3)
endif()

add_library(kh2_imgui STATIC
    ${kh2_imgui_src_SOURCE_DIR}/imgui.cpp
    ${kh2_imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${kh2_imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${kh2_imgui_src_SOURCE_DIR}/imgui_widgets.cpp)
if(WIN32)
    target_sources(kh2_imgui PRIVATE ${kh2_imgui_src_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
endif()
target_include_directories(kh2_imgui PUBLIC ${kh2_imgui_src_SOURCE_DIR})

add_executable(kh2recomp
    port/runtime/main.cpp
    port/runtime/overlay_manager.cpp
    port/runtime/pad2_hle.cpp
    port/runtime/cd_iso.cpp
    port/runtime/cd_callback.cpp
    port/runtime/movie_eof.cpp
    port/runtime/cutscene_skip.cpp
    port/runtime/english_text.cpp
    port/runtime/gpu_sync.cpp
    port/runtime/file_override.cpp
    port/runtime/trace.cpp
    port/runtime/dma_log.cpp
    port/runtime/gs_image.cpp
    port/runtime/gs_env.cpp
    port/runtime/profiler.cpp
    port/runtime/pgs_renderer.cpp
    port/runtime/hd_textures.cpp
    port/runtime/host_input.cpp
    port/runtime/key_bindings.cpp
    port/runtime/game_patches.cpp
    port/runtime/watchdog.cpp
    port/runtime/overlay_ui.cpp
    port/coop/game_state.cpp
    port/coop/coop.cpp
    port/coop/puppets.cpp
    port/coop/partners.cpp
    port/coop/enemies.cpp
    port/coop/guest_call.cpp
    port/coop/net/udp_socket.cpp
    port/coop/net/protocol.cpp
    port/coop/net/channel.cpp
    port/coop/net/session.cpp
    ${KH2_PATCH_SOURCES}
    "${KH2_GENERATED_DIR}/overlays/overlay_registry.cpp")
target_include_directories(kh2recomp PRIVATE port/runtime port/coop ${KH2_RUNTIME_INCLUDES}
    third_party/parallel-gs/Granite/third_party/stb/stb)
target_link_libraries(kh2recomp PRIVATE kh2_main ${KH2_OVERLAY_LIBS} ${KH2_PATCH_LIBS} ps2_runtime parallel-gs kh2_imgui)
if(MSVC)
    target_compile_options(kh2recomp PRIVATE /W3)
    # A PDB for --profile and debuggers: full debug info for the runner and the
    # runtime, public symbols only for the generated code (keeps its build fast).
    target_compile_options(kh2recomp PRIVATE /Zi /FS)
    target_compile_options(ps2_runtime PRIVATE /Zi /FS)
    target_link_options(kh2recomp PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
ps2x_stage_ffmpeg_runtime_dlls(kh2recomp)

# --- Tools linked against the runtime --------------------------------------
# kh2vureplay: run a captured VU1 program (PS2_VU_CAPTURE_DIR) and decode its
# XGKICK packets. Built on request: cmake --build build\win --target kh2vureplay
add_executable(kh2vureplay EXCLUDE_FROM_ALL tools/vureplay/vureplay.cpp)
target_include_directories(kh2vureplay PRIVATE ${KH2_RUNTIME_INCLUDES})
target_link_libraries(kh2vureplay PRIVATE ps2_runtime)
ps2x_stage_ffmpeg_runtime_dlls(kh2vureplay)

# Co-op network layer test (no game needed): loopback host + two clients with
# simulated packet loss. ctest, or run build\win\coop_net_test.exe.
option(KH2_BUILD_TESTS "Build the tests that need no game (co-op network, key bindings)" ON)
if(KH2_BUILD_TESTS)
    enable_testing()
    add_executable(coop_net_test tests/coop_net_test.cpp
        port/coop/net/udp_socket.cpp port/coop/net/protocol.cpp port/coop/net/channel.cpp port/coop/net/session.cpp)
    target_include_directories(coop_net_test PRIVATE port/coop/net)
    if(WIN32)
        target_link_libraries(coop_net_test PRIVATE ws2_32)
    endif()
    add_test(NAME coop_net COMMAND coop_net_test)
    add_executable(key_bindings_test tests/key_bindings_test.cpp port/runtime/key_bindings.cpp)
    target_include_directories(key_bindings_test PRIVATE port/runtime)
    add_test(NAME key_bindings COMMAND key_bindings_test)
endif()
if(WIN32)
    target_link_libraries(kh2recomp PRIVATE ws2_32)
endif()
if(KH2_SDL3)
    target_link_libraries(kh2recomp PRIVATE SDL3::SDL3-static)
    target_compile_definitions(kh2recomp PRIVATE KH2_HAVE_SDL3=1)
endif()
