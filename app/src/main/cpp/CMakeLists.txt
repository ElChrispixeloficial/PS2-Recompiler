cmake_minimum_required(VERSION 3.22.1)
project(ps2recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wno-unused-parameter
    -O2
    -ffast-math
    -fno-exceptions
)

# ARM64 optimisations
if(ANDROID_ABI STREQUAL "arm64-v8a")
    add_compile_options(-march=armv8-a+simd)
endif()

# ── Source files ──────────────────────────────────────────────────────────────
set(SOURCES
    # JNI bridge
    jni_bridge.cpp

    # EE (Emotion Engine / R5900)
    ee/ee_core.cpp
    ee/recompiler_arm64.cpp
    ee/recompiler_arm32.cpp
    ee/code_cache.cpp
    ee/ee_memory.cpp
    ee/mips_disasm.cpp

    # GS (Graphics Synthesizer)
    gs/gs_core.cpp
    gs/gs_vulkan.cpp
    gs/vulkan_context.cpp
    gs/gs_rasterizer.cpp

    # SPU2 (Sound Processing Unit 2)
    spu2/spu2_core.cpp

    # IOP (I/O Processor / R3000A)
    iop/iop_core.cpp
    iop/iop_recompiler.cpp

    # VU (Vector Units 0 & 1)
    vu/vu_core.cpp
    vu/vu_recompiler.cpp
    vu/vif_unpacker.cpp

    # Bus / DMAC
    bus/memory_map.cpp
    bus/dma_controller.cpp

    # ISO / ELF loader
    iso/iso_loader.cpp
    iso/cdvd.cpp

    # BIOS
    bios/bios_native.cpp

    # Input (DualShock 2)
    input/dualshock.cpp
)

# ── Shared library ────────────────────────────────────────────────────────────
add_library(ps2recomp SHARED ${SOURCES})

# ── Include paths ─────────────────────────────────────────────────────────────
target_include_directories(ps2recomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ee
    ${CMAKE_CURRENT_SOURCE_DIR}/include/arm64
)

# ── Android / system libraries ────────────────────────────────────────────────
find_library(log-lib      log)
find_library(android-lib  android)
find_library(opensl-lib   OpenSLES)
find_library(vulkan-lib   vulkan)
find_library(game-lib     game)

target_link_libraries(ps2recomp
    ${log-lib}
    ${android-lib}
    ${opensl-lib}
    ${vulkan-lib}
)

# Link game library if available (for controller input)
if(game-lib)
    target_link_libraries(ps2recomp ${game-lib})
endif()

# ── Strip debug info in release ───────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(ps2recomp PROPERTIES
        LINK_FLAGS "-Wl,--strip-all"
    )
endif()
