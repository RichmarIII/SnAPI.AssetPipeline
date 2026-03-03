add_library(TexturePlugin SHARED
    TexturePluginMain.cpp
    TextureImporter_FreeImage.cpp
    TextureCooker_RGBA8.cpp
    TexturePayloadSerializers.cpp
)

target_include_directories(TexturePlugin
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(TexturePlugin
    PRIVATE
        SnAPI.AssetPipeline
        cereal
)

# FreeImage linkage
if(TARGET FreeImage)
    target_link_libraries(TexturePlugin PRIVATE FreeImage)
    target_compile_definitions(TexturePlugin PRIVATE HAS_FREEIMAGE=1)
elseif(FREEIMAGE_FOUND)
    target_link_libraries(TexturePlugin PRIVATE freeimage)
    target_compile_definitions(TexturePlugin PRIVATE HAS_FREEIMAGE=1)
else()
    # Try to find system FreeImage
    find_library(FREEIMAGE_LIB freeimage FreeImage)
    find_path(FREEIMAGE_INCLUDE FreeImage.h)

    if(FREEIMAGE_LIB AND FREEIMAGE_INCLUDE)
        target_include_directories(TexturePlugin PRIVATE ${FREEIMAGE_INCLUDE})
        target_link_libraries(TexturePlugin PRIVATE ${FREEIMAGE_LIB})
        target_compile_definitions(TexturePlugin PRIVATE HAS_FREEIMAGE=1)
    else()
        message(WARNING "FreeImage not found. Texture plugin will have limited functionality.")
        target_compile_definitions(TexturePlugin PRIVATE HAS_FREEIMAGE=0)
    endif()
endif()

target_compile_definitions(TexturePlugin PRIVATE SNAPI_ASSETPIPELINE_EXPORTS)

set_target_properties(TexturePlugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TexturePlugin"
)
