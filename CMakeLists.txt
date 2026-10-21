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
foreach(patch IN LISTS KH2_RUNTIME_PATCHES)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
        WORKING_DIRECTORY "${PS2RECOMP_DIR}" RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
    if(NOT already EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${PS2RECOMP_DIR}" RESULT_VARIABLE applied)
        if(NOT applied EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch} to third_party/PS2Recomp")
        endif()
        message(STATUS "kh2: applied ${patch}")
    endif()
endforeach()

# --- PS2Recomp runtime -----------------------------------------------------
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

# --- Runner ---------------------------------------------------------------
add_executable(kh2recomp
    port/runtime/main.cpp
    port/runtime/overlay_manager.cpp
    port/runtime/pad2_hle.cpp
    port/runtime/cd_iso.cpp
    port/runtime/cd_callback.cpp
    port/runtime/trace.cpp
    port/runtime/dma_log.cpp
    port/runtime/gs_image.cpp
    port/runtime/gs_env.cpp
    port/runtime/profiler.cpp
    "${KH2_GENERATED_DIR}/overlays/overlay_registry.cpp")
target_include_directories(kh2recomp PRIVATE port/runtime ${KH2_RUNTIME_INCLUDES})
target_link_libraries(kh2recomp PRIVATE kh2_main ${KH2_OVERLAY_LIBS} ps2_runtime)
if(MSVC)
    target_compile_options(kh2recomp PRIVATE /W3)
    # A PDB for --profile and debuggers: full debug info for the runner and the
    # runtime, public symbols only for the generated code (keeps its build fast).
    target_compile_options(kh2recomp PRIVATE /Zi /FS)
    target_compile_options(ps2_runtime PRIVATE /Zi /FS)
    target_link_options(kh2recomp PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
ps2x_stage_ffmpeg_runtime_dlls(kh2recomp)
