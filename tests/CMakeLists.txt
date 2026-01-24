add_executable(SnAPI.AssetPipeline.Tests
    UuidTests.cpp
    PackFormatTests.cpp
    PipelineTests.cpp
    CorruptionTests.cpp
    SourceAssetTests.cpp
)

target_link_libraries(SnAPI.AssetPipeline.Tests
    PRIVATE
        SnAPI.AssetPipeline
        Catch2::Catch2WithMain
)

target_include_directories(SnAPI.AssetPipeline.Tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

include(CTest)
include(Catch)
catch_discover_tests(SnAPI.AssetPipeline.Tests)
