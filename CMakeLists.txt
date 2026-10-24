cmake_minimum_required(VERSION 3.25)

project(DreamStation
    VERSION     0.1.0
    DESCRIPTION "A Sega Dreamcast, Naomi and Atomiswave emulator for Linux"
    HOMEPAGE_URL "https://github.com/Johnnygentoo/dreamstation"
    LANGUAGES   C CXX
)

# ─── Global settings ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Binaries in one place, so the app finds its resources at run time.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ─── Options ─────────────────────────────────────────────────────────────────
option(DS_BUILD_TESTS       "Build unit tests"                ON)
option(DS_ENABLE_LTO        "Enable LTO in Release builds"    ON)
option(DS_ENABLE_SANITIZERS "ASan + UBSan (Debug only)"       OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(CompilerWarnings)
include(GNUInstallDirs)

# ─── Dependencies ────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
find_package(Threads   REQUIRED)
pkg_check_modules(GTKMM4    REQUIRED IMPORTED_TARGET gtkmm-4.0>=4.10)
pkg_check_modules(ADWAITAMM REQUIRED IMPORTED_TARGET libadwaitamm-1>=1.2)
pkg_check_modules(EPOXY     REQUIRED IMPORTED_TARGET epoxy)
pkg_check_modules(SDL3      REQUIRED IMPORTED_TARGET sdl3)

include(FetchContent)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE)
FetchContent_Declare(tomlpp
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(spdlog tomlpp)

if(DS_BUILD_TESTS)
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.6.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(catch2)
    enable_testing()
endif()

# ─── Parts ───────────────────────────────────────────────────────────────────
add_subdirectory(engine)                  # the Flycast fork and its host wrapper
add_subdirectory(common)
add_subdirectory(third_party/reshadefx)
add_subdirectory(frontend-gtk)
add_subdirectory(tools)

message(STATUS "DreamStation ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE}), tests: ${DS_BUILD_TESTS}")
