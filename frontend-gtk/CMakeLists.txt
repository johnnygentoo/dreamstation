# Compile gresource bundle (UI files, icons, CSS)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/io.github.johnnygentoo.DreamStation.gresource.xml)
set(GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/resources.c)

add_custom_command(
    OUTPUT  ${GRESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --target=${GRESOURCE_C}
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data ${GRESOURCE_XML}
    DEPENDS ${GRESOURCE_XML}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data)

add_executable(dreamstation
    src/main.cpp
    src/application.cpp
    src/config.cpp
    src/game_library.cpp
    src/disc_image.cpp
    src/image_io.cpp
    src/ui/main_window.cpp
    src/ui/render_widget.cpp
    src/ui/library_page.cpp
    src/ui/settings_window.cpp
    src/host/pad_input.cpp
    src/hotkeys.cpp
    src/play_stats.cpp
    src/vmu.cpp
    src/vmu_cpu.cpp
    src/ui/cover_downloader.cpp
    src/ui/cover_cache.cpp
    src/ui/controllers_window.cpp
    src/ui/pause_menu.cpp
    src/ui/vmu_manager.cpp
    src/ui/vmu_player.cpp
    src/ui/vmu_unit.cpp
    src/ui/vmu_window.cpp
    src/ui/netplay_dialog.cpp
    src/ui/osd_layer.cpp
    src/ui/post_chain.cpp
    src/ui/slang_preset.cpp
    src/ui/reshade_effect.cpp
    src/ui/post_shaders_builtin.cpp
    src/ui/main_window_states.cpp
    src/ui/alert.cpp
    ${GRESOURCE_C}
)

target_link_libraries(dreamstation PRIVATE
    ds::common ds::engine
    PkgConfig::GTKMM4 PkgConfig::ADWAITAMM PkgConfig::EPOXY PkgConfig::SDL3
    spdlog::spdlog tomlplusplus::tomlplusplus reshadefx
    ds_warnings)

# stb_image(_write), as carried by the engine fork, for reading and writing pictures.
set(DS_STB_DIR ${CMAKE_SOURCE_DIR}/engine/flycast/core/deps/stb)
target_include_directories(dreamstation PRIVATE ${DS_STB_DIR})
# stb is third-party code; our warning set is not for it.
set_source_files_properties(src/image_io.cpp PROPERTIES COMPILE_OPTIONS -w)

install(TARGETS dreamstation DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.johnnygentoo.DreamStation.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.johnnygentoo.DreamStation.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/icons/io.github.johnnygentoo.DreamStation.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
target_compile_definitions(dreamstation PRIVATE DS_VERSION="${PROJECT_VERSION}")

# Headless runs of the post-processing chain on a picture (not installed).
add_executable(post_probe
    tools/post_probe.cpp
    src/config.cpp
    src/image_io.cpp
    src/ui/post_chain.cpp
    src/ui/slang_preset.cpp
    src/ui/reshade_effect.cpp
    src/ui/post_shaders_builtin.cpp
)
target_include_directories(post_probe PRIVATE ${DS_STB_DIR})
target_link_libraries(post_probe PRIVATE
    ds::common PkgConfig::GTKMM4 PkgConfig::EPOXY
    spdlog::spdlog tomlplusplus::tomlplusplus reshadefx)

# Unit tests for the frontend's pure logic (no GTK main loop needed).
if(DS_BUILD_TESTS)
    add_executable(frontend_tests tests/vmu_test.cpp src/vmu.cpp tests/vmu_cpu_test.cpp src/vmu_cpu.cpp)
    target_link_libraries(frontend_tests PRIVATE Catch2::Catch2WithMain PkgConfig::GTKMM4)
    add_test(NAME frontend_tests COMMAND frontend_tests)
endif()
