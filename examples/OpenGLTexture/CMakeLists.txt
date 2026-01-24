# OpenGL Texture Example

# Build the plugin serializers as part of the example (links statically)
add_executable(OpenGLTextureExample
    main.cpp
    ${CMAKE_SOURCE_DIR}/plugins/Texture/TexturePayloadSerializers.cpp
)

target_link_libraries(OpenGLTextureExample
    PRIVATE
        SnAPI.AssetPipeline
        glfw
        cereal
)

# Link OpenGL
find_package(OpenGL REQUIRED)
target_link_libraries(OpenGLTextureExample PRIVATE OpenGL::GL)

# Copy any test assets if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test.png")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/test.png"
                   "${CMAKE_CURRENT_BINARY_DIR}/test.png" COPYONLY)
endif()
