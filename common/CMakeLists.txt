add_library(ds_common STATIC
    src/log.cpp
)
add_library(ds::common ALIAS ds_common)

target_include_directories(ds_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(ds_common PUBLIC spdlog::spdlog tomlplusplus::tomlplusplus)
