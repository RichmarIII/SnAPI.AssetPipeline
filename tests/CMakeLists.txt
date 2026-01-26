add_executable(SnAPI.AssetPipeline.Tests
    UuidTests.cpp
    PackFormatTests.cpp
    PipelineTests.cpp
    CorruptionTests.cpp
    SourceAssetTests.cpp
    TextureCompressorTests.cpp
    # TextureCompressor plugin sources compiled directly into tests
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/TextureCompressorPayloadSerializers.cpp
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/TextureFormatSelector.cpp
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/MipGenerator.cpp
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/CompressorBackendBCn.cpp
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/CompressorBackendASTC.cpp
    ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor/TextureCompressorCooker.cpp
)

target_link_libraries(SnAPI.AssetPipeline.Tests
    PRIVATE
        SnAPI.AssetPipeline
        Catch2::Catch2WithMain
        cereal
        cmp_compressonatorlib
        astcenc
)

target_include_directories(SnAPI.AssetPipeline.Tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/plugins/TextureCompressor
)

# Link real compression libraries for testing
target_compile_definitions(SnAPI.AssetPipeline.Tests
    PRIVATE
        HAS_COMPRESSONATOR=$<BOOL:${COMPRESSONATOR_FOUND}>
        HAS_ASTCENC=$<BOOL:${ASTCENC_FOUND}>
        HAS_FREEIMAGE=0
        SNAPI_ASSETPIPELINE_EXPORTS
)

include(CTest)
include(Catch)
catch_discover_tests(SnAPI.AssetPipeline.Tests)
