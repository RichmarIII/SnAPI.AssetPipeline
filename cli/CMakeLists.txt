add_executable(snpak-cli main.cpp)

target_link_libraries(snpak-cli PRIVATE SnAPI.AssetPipeline)

set_target_properties(snpak-cli PROPERTIES
        OUTPUT_NAME "snpak"
)

install(TARGETS snpak-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
