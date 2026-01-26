cmake_minimum_required(VERSION 3.20)
project(SnAPI.AssetPipeline VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(SNAPI_BUILD_CLI "Build CLI tool" ON)
option(SNAPI_BUILD_TESTS "Build tests" ON)
option(SNAPI_BUILD_PLUGINS "Build plugins" ON)
option(SNAPI_BUILD_EXAMPLES "Build examples" ON)

# Include dependencies
include(cmake/Dependencies.cmake)

# Main library sources
set(SNAPI_SOURCES
    src/Core/UuidImpl.cpp
    src/Core/PayloadRegistry.cpp
    src/Core/PipelineContext.cpp
    src/Hashing/XXHash.cpp
    src/Pack/Compression.cpp
    src/Pack/AssetPackReader.cpp
    src/Pack/AssetPackWriter.cpp
    src/Pack/MemoryMappedFile.cpp
    src/Pipeline/AssetPipeline.cpp
    src/Pipeline/PluginLoader.cpp
    src/Pipeline/IncrementalCache.cpp
    src/Runtime/AssetManager.cpp
    src/Runtime/AssetCache.cpp
    src/Runtime/AsyncLoader.cpp
    src/Runtime/SourceAssetResolver.cpp
    src/Runtime/AutoMountScanner.cpp
)

# Main library
add_library(SnAPI.AssetPipeline SHARED ${SNAPI_SOURCES})

target_include_directories(SnAPI.AssetPipeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(SnAPI.AssetPipeline
    PRIVATE
        stduuid
        xxhash
        lz4
        libzstd_static
        cereal
        sqlite3
)

target_compile_definitions(SnAPI.AssetPipeline
    PRIVATE
        SNAPI_ASSETPIPELINE_EXPORTS
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(SnAPI.AssetPipeline PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX)
    target_link_libraries(SnAPI.AssetPipeline PRIVATE dl)
endif()

# CLI tool
if(SNAPI_BUILD_CLI)
    add_subdirectory(cli)
endif()

# Plugins
if(SNAPI_BUILD_PLUGINS)
    add_subdirectory(plugins/Texture)
    add_subdirectory(plugins/TextureCompressor)
endif()

# Tests
if(SNAPI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(SNAPI_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS SnAPI.AssetPipeline
    EXPORT SnAPI.AssetPipelineTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT SnAPI.AssetPipelineTargets
    FILE SnAPI.AssetPipelineTargets.cmake
    NAMESPACE SnAPI::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SnAPI.AssetPipeline
)
