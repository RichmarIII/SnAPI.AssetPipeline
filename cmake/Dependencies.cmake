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

# Catch2
FetchContent_MakeAvailable(Catch2)

# GLFW - for examples (disable docs/tests/examples)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)
