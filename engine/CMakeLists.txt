# The emulation engine: our Flycast fork (engine/flycast, GPL-2.0-or-later) compiled in
# as a static library, plus a toolkit-independent host wrapper (engine/host).

# The fork is built through its GUI-less shell configuration.
set(LIBRETRO        ON  CACHE BOOL "" FORCE)
set(LIBRETRO_STATIC ON  CACHE BOOL "" FORCE)
set(USE_VULKAN      OFF CACHE BOOL "" FORCE)   # the host renders with OpenGL only
set(USE_OPENGL      ON  CACHE BOOL "" FORCE)

# The fork carries a JIT and hand-tuned hot loops; keep our LTO and warning
# policy out of it.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(BUILD_TESTING OFF)

add_subdirectory(flycast EXCLUDE_FROM_ALL)

add_library(ds_engine STATIC host/src/engine.cpp host/src/arcade.cpp)
add_library(ds::engine ALIAS ds_engine)
target_include_directories(ds_engine
    PUBLIC  host/include
    PRIVATE flycast/core/deps/libretro-common/include
            # zstd as bundled with the fork (used for save-state compression)
            flycast/core/deps/libchdr/deps/zstd-1.5.6/lib)
target_link_libraries(ds_engine PRIVATE libzstd_static)
# Whole-archive: the engine registers its recompiler and backends through
# static objects that nothing references, so a normal static link drops them.
target_link_libraries(ds_engine PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,flycast_libretro>")
