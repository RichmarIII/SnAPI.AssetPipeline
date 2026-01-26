include(FetchContent)

# stduuid - UUID parsing/generation (MIT)
FetchContent_Declare(
    stduuid
    GIT_REPOSITORY https://github.com/mariusbancila/stduuid.git
    GIT_TAG        v1.2.3
)

# xxHash - Fast hashing (BSD)
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.2
)

# LZ4 - Fast compression (BSD-2)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.9.4
)

# Zstd - High compression (BSD)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.5
)

# cereal - Serialization (BSD)
FetchContent_Declare(
    cereal
    GIT_REPOSITORY https://github.com/USCiLab/cereal.git
    GIT_TAG        v1.3.2
)

# SQLite3 - Database (Public Domain)
FetchContent_Declare(
    sqlite3
    GIT_REPOSITORY https://github.com/azadkuh/sqlite-amalgamation.git
    GIT_TAG        3.38.2
)

# FreeImage - Image loading (GPL/FIPL)
FetchContent_Declare(
    freeimage
    GIT_REPOSITORY https://github.com/danoli3/FreeImage.git
    GIT_TAG        master
)

# Catch2 - Testing (BSL)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
)

# Compressonator - BCn texture compression (MIT)
FetchContent_Declare(
    compressonator
    GIT_REPOSITORY https://github.com/GPUOpen-Tools/compressonator.git
    GIT_TAG        V4.5.52
    GIT_SHALLOW    TRUE
)

# astc-encoder - ASTC texture compression (Apache-2.0)
FetchContent_Declare(
    astcencoder
    GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
    GIT_TAG        4.8.0
    GIT_SHALLOW    TRUE
)

# GLFW - Windowing for examples (Zlib)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.3.8
)

# Make stduuid available
FetchContent_MakeAvailable(stduuid)

# xxHash - we only need the header
FetchContent_GetProperties(xxhash)
if(NOT xxhash_POPULATED)
    FetchContent_Populate(xxhash)
endif()
add_library(xxhash INTERFACE)
target_include_directories(xxhash INTERFACE ${xxhash_SOURCE_DIR})

# LZ4
FetchContent_GetProperties(lz4)
if(NOT lz4_POPULATED)
    FetchContent_Populate(lz4)
endif()
add_library(lz4 STATIC
    ${lz4_SOURCE_DIR}/lib/lz4.c
    ${lz4_SOURCE_DIR}/lib/lz4hc.c
    ${lz4_SOURCE_DIR}/lib/lz4frame.c
    ${lz4_SOURCE_DIR}/lib/xxhash.c
)
target_include_directories(lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)
set_target_properties(lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Zstd - build manually to avoid CMake version issues
FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
    FetchContent_Populate(zstd)
endif()

# Build zstd from source files directly
file(GLOB ZSTD_COMMON_SOURCES ${zstd_SOURCE_DIR}/lib/common/*.c)
file(GLOB ZSTD_COMPRESS_SOURCES ${zstd_SOURCE_DIR}/lib/compress/*.c)
file(GLOB ZSTD_DECOMPRESS_SOURCES ${zstd_SOURCE_DIR}/lib/decompress/*.c)

add_library(libzstd_static STATIC
    ${ZSTD_COMMON_SOURCES}
    ${ZSTD_COMPRESS_SOURCES}
    ${ZSTD_DECOMPRESS_SOURCES}
)
target_include_directories(libzstd_static PUBLIC ${zstd_SOURCE_DIR}/lib)
target_compile_definitions(libzstd_static PRIVATE ZSTD_MULTITHREAD ZSTD_DISABLE_ASM)
set_target_properties(libzstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# cereal - header only
FetchContent_GetProperties(cereal)
if(NOT cereal_POPULATED)
    FetchContent_Populate(cereal)
endif()
add_library(cereal INTERFACE)
target_include_directories(cereal INTERFACE ${cereal_SOURCE_DIR}/include)

# SQLite3
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
    FetchContent_Populate(sqlite3)
endif()
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
set_target_properties(sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX AND NOT APPLE)
    target_link_libraries(sqlite3 PUBLIC dl pthread)
endif()

# FreeImage - complex build, use system or build manually
FetchContent_GetProperties(freeimage)
if(NOT freeimage_POPULATED)
    FetchContent_Populate(freeimage)
endif()
# FreeImage has a complex build system - we'll add it as a subdirectory if CMakeLists exists
# or fall back to finding system FreeImage
if(EXISTS ${freeimage_SOURCE_DIR}/CMakeLists.txt)
    add_subdirectory(${freeimage_SOURCE_DIR} ${freeimage_BINARY_DIR} EXCLUDE_FROM_ALL)
else()
    find_package(FreeImage QUIET)
    if(NOT FreeImage_FOUND)
        message(WARNING "FreeImage not found - Texture plugin may not build. Install FreeImage or provide FREEIMAGE_INCLUDE_DIR and FREEIMAGE_LIBRARY")
        # Create a dummy target
        add_library(freeimage INTERFACE)
        set(FREEIMAGE_FOUND FALSE CACHE BOOL "FreeImage found" FORCE)
    else()
        add_library(freeimage INTERFACE)
        target_include_directories(freeimage INTERFACE ${FREEIMAGE_INCLUDE_DIR})
        target_link_libraries(freeimage INTERFACE ${FREEIMAGE_LIBRARY})
        set(FREEIMAGE_FOUND TRUE CACHE BOOL "FreeImage found" FORCE)
    endif()
endif()

# Compressonator - build core compression library only
FetchContent_GetProperties(compressonator)
if(NOT compressonator_POPULATED)
    FetchContent_Populate(compressonator)
endif()

# Build Compressonator's core compression SDK as a static library
# Source structure mirrors cmp_compressonatorlib/CMakeLists.txt
set(CMP_LIB_DIR ${compressonator_SOURCE_DIR}/cmp_compressonatorlib)

file(GLOB_RECURSE CMP_CODEC_SOURCES
    ${CMP_LIB_DIR}/apc/*.cpp
    ${CMP_LIB_DIR}/atc/*.cpp
    ${CMP_LIB_DIR}/ati/*.cpp
    ${CMP_LIB_DIR}/ati/*.c
    ${CMP_LIB_DIR}/basis/*.cpp
    ${CMP_LIB_DIR}/bc6h/*.cpp
    ${CMP_LIB_DIR}/bc7/*.cpp
    ${CMP_LIB_DIR}/block/*.cpp
    ${CMP_LIB_DIR}/buffer/*.cpp
    ${CMP_LIB_DIR}/dxt/*.cpp
    ${CMP_LIB_DIR}/dxtc/*.cpp
    ${CMP_LIB_DIR}/dxtc/*.c
    ${CMP_LIB_DIR}/etc/*.cpp
    ${CMP_LIB_DIR}/etc/etcpack/*.cpp
    ${CMP_LIB_DIR}/etc/etcpack/*.cxx
    ${CMP_LIB_DIR}/gt/*.cpp
    ${CMP_LIB_DIR}/common/*.cpp
)

# Root-level library sources
set(CMP_ROOT_SOURCES
    ${CMP_LIB_DIR}/compress.cpp
    ${CMP_LIB_DIR}/compressonator.cpp
)

# cmp_framework common sources (needed for mip generation, format conversion)
file(GLOB CMP_FRAMEWORK_SOURCES
    ${compressonator_SOURCE_DIR}/cmp_framework/common/*.cpp
    ${compressonator_SOURCE_DIR}/cmp_framework/common/half/*.cpp
)

# applications/_plugins/common sources (atiformats, codec_common, format_conversion)
set(CMP_PLUGIN_COMMON_SOURCES
    ${compressonator_SOURCE_DIR}/applications/_plugins/common/atiformats.cpp
    ${compressonator_SOURCE_DIR}/applications/_plugins/common/format_conversion.cpp
    ${compressonator_SOURCE_DIR}/applications/_plugins/common/codec_common.cpp
    ${compressonator_SOURCE_DIR}/applications/_plugins/common/texture_utils.cpp
    ${compressonator_SOURCE_DIR}/applications/_libs/cmp_math/cpu_extensions.cpp
    ${compressonator_SOURCE_DIR}/applications/_libs/cmp_math/cmp_math_common.cpp
)

# cmp_core sources (dispatcher + SIMD variants)
set(CMP_CORE_SOURCES
    ${compressonator_SOURCE_DIR}/cmp_core/source/cmp_core.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_sse.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_avx.cpp
    ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_avx512.cpp
)

# Set per-file SIMD compile flags for cmp_core SIMD sources
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64)")
    set_source_files_properties(
        ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_sse.cpp
        PROPERTIES COMPILE_FLAGS "-msse4.1"
    )
    set_source_files_properties(
        ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_avx.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2 -mf16c"
    )
    set_source_files_properties(
        ${compressonator_SOURCE_DIR}/cmp_core/source/core_simd_avx512.cpp
        PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512bw -mavx512vl"
    )
endif()

if(EXISTS ${CMP_LIB_DIR}/compressonator.cpp)
    add_library(cmp_compressonatorlib STATIC
        ${CMP_ROOT_SOURCES}
        ${CMP_CODEC_SOURCES}
        ${CMP_FRAMEWORK_SOURCES}
        ${CMP_PLUGIN_COMMON_SOURCES}
        ${CMP_CORE_SOURCES}
    )
    target_include_directories(cmp_compressonatorlib PUBLIC
        ${CMP_LIB_DIR}
        ${CMP_LIB_DIR}/common
        ${CMP_LIB_DIR}/apc
        ${CMP_LIB_DIR}/atc
        ${CMP_LIB_DIR}/ati
        ${CMP_LIB_DIR}/basis
        ${CMP_LIB_DIR}/bc6h
        ${CMP_LIB_DIR}/bc7
        ${CMP_LIB_DIR}/block
        ${CMP_LIB_DIR}/buffer
        ${CMP_LIB_DIR}/dxt
        ${CMP_LIB_DIR}/dxtc
        ${CMP_LIB_DIR}/etc
        ${CMP_LIB_DIR}/etc/etcpack
        ${CMP_LIB_DIR}/gt
        ${compressonator_SOURCE_DIR}/cmp_framework/common
        ${compressonator_SOURCE_DIR}/cmp_framework/common/half
        ${compressonator_SOURCE_DIR}/applications/_plugins/common
        ${compressonator_SOURCE_DIR}/applications/_libs/cmp_math
        ${compressonator_SOURCE_DIR}/cmp_core/source
        ${compressonator_SOURCE_DIR}/cmp_core/shaders
    )
    set_target_properties(cmp_compressonatorlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(cmp_compressonatorlib PRIVATE USE_ASPM_CODE=0)
    set(COMPRESSONATOR_FOUND TRUE CACHE BOOL "Compressonator found" FORCE)
else()
    message(WARNING "Compressonator sources not found at expected paths. TextureCompressor BCn support may be limited.")
    add_library(cmp_compressonatorlib INTERFACE)
    set(COMPRESSONATOR_FOUND FALSE CACHE BOOL "Compressonator found" FORCE)
endif()

# astc-encoder - build as static library
FetchContent_GetProperties(astcencoder)
if(NOT astcencoder_POPULATED)
    FetchContent_Populate(astcencoder)
endif()

# Only include library sources (astcenc_*), not CLI sources (astcenccli_*)
file(GLOB ASTCENC_SOURCES ${astcencoder_SOURCE_DIR}/Source/astcenc_*.cpp)
if(ASTCENC_SOURCES)
    add_library(astcenc STATIC ${ASTCENC_SOURCES})
    target_include_directories(astcenc PUBLIC ${astcencoder_SOURCE_DIR}/Source)
    set_target_properties(astcenc PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Enable SIMD acceleration based on host architecture
    include(CheckCXXCompilerFlag)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|amd64)")
        check_cxx_compiler_flag("-msse4.1" HAS_SSE41)
        check_cxx_compiler_flag("-mavx2" HAS_AVX2)
        if(HAS_AVX2)
            target_compile_definitions(astcenc PRIVATE ASTCENC_SSE=41 ASTCENC_AVX=2 ASTCENC_POPCNT=1 ASTCENC_F16C=1)
            target_compile_options(astcenc PRIVATE -mavx2 -mpopcnt -mf16c)
        elseif(HAS_SSE41)
            target_compile_definitions(astcenc PRIVATE ASTCENC_SSE=41)
            target_compile_options(astcenc PRIVATE -msse4.1)
        else()
            target_compile_definitions(astcenc PRIVATE ASTCENC_SSE=20)
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
        target_compile_definitions(astcenc PRIVATE ASTCENC_NEON=1)
    else()
        target_compile_definitions(astcenc PRIVATE ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_NEON=0)
    endif()

    # Do NOT define ASTCENC_DECOMPRESS_ONLY - it uses #if defined() checks
    # Leaving it undefined enables full compression support
    set(ASTCENC_FOUND TRUE CACHE BOOL "astc-encoder found" FORCE)
else()
    message(WARNING "astc-encoder sources not found. TextureCompressor ASTC support will be unavailable.")
    add_library(astcenc INTERFACE)
    set(ASTCENC_FOUND FALSE CACHE BOOL "astc-encoder found" FORCE)
endif()

# Catch2
FetchContent_MakeAvailable(Catch2)

# GLFW - for examples (disable docs/tests/examples)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)
