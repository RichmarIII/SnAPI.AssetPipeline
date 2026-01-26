add_library(TextureCompressorPlugin SHARED
    TextureCompressorPluginMain.cpp
    TextureCompressorPayloadSerializers.cpp
    TextureCompressorImporter.cpp
    TextureCompressorCooker.cpp
    TextureFormatSelector.cpp
    CompressorBackendBCn.cpp
    CompressorBackendASTC.cpp
    MipGenerator.cpp
)

target_include_directories(TextureCompressorPlugin
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(TextureCompressorPlugin
    PRIVATE
        SnAPI.AssetPipeline
        cereal
)

# Compressonator linkage
if(COMPRESSONATOR_FOUND)
    target_link_libraries(TextureCompressorPlugin PRIVATE cmp_compressonatorlib)
    target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_COMPRESSONATOR=1)
else()
    target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_COMPRESSONATOR=0)
endif()

# astc-encoder linkage
if(ASTCENC_FOUND)
    target_link_libraries(TextureCompressorPlugin PRIVATE astcenc)
    target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_ASTCENC=1)
else()
    target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_ASTCENC=0)
endif()

# FreeImage linkage
if(FREEIMAGE_FOUND OR TARGET FreeImage)
    target_link_libraries(TextureCompressorPlugin PRIVATE freeimage)
    target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_FREEIMAGE=1)
else()
    # Try to find system FreeImage
    find_library(FREEIMAGE_LIB freeimage FreeImage)
    find_path(FREEIMAGE_INCLUDE FreeImage.h)

    if(FREEIMAGE_LIB AND FREEIMAGE_INCLUDE)
        target_include_directories(TextureCompressorPlugin PRIVATE ${FREEIMAGE_INCLUDE})
        target_link_libraries(TextureCompressorPlugin PRIVATE ${FREEIMAGE_LIB})
        target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_FREEIMAGE=1)
    else()
        message(WARNING "FreeImage not found. TextureCompressor importer will have limited functionality.")
        target_compile_definitions(TextureCompressorPlugin PRIVATE HAS_FREEIMAGE=0)
    endif()
endif()

target_compile_definitions(TextureCompressorPlugin PRIVATE SNAPI_ASSETPIPELINE_EXPORTS)

set_target_properties(TextureCompressorPlugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TextureCompressorPlugin"
)
