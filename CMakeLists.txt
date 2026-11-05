cmake_minimum_required(VERSION 3.15)
project(hulud C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(
    HULUD_PRODUCTION_PLUGIN
    "Build the plugin with the production-compatible ABI settings used for CVMFS deployment"
    OFF
)
set(
    HULUD_PLUGIN_VERSION_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hulud-plugin.map"
    CACHE FILEPATH
    "Linker version script used when HULUD_PRODUCTION_PLUGIN=ON"
)

if(NOT CMAKE_CONFIGURATION_TYPES AND (NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL ""))
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

if(HULUD_PRODUCTION_PLUGIN)
    if(NOT EXISTS "${HULUD_PLUGIN_VERSION_SCRIPT}")
        message(FATAL_ERROR "Production plugin version script not found: ${HULUD_PLUGIN_VERSION_SCRIPT}")
    endif()
    add_compile_definitions(H5_USE_110_API)
endif()

# --- Dependencies ---
if(HULUD_PRODUCTION_PLUGIN)
    find_package(HDF5 CONFIG REQUIRED COMPONENTS C HL)
else()
    find_package(HDF5 QUIET CONFIG COMPONENTS C HL)
    if(NOT HDF5_FOUND)
        find_package(HDF5 REQUIRED COMPONENTS C HL)
    endif()
endif()
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(HULUD_HDF5_LIBS)
if(TARGET HDF5::HDF5)
    list(APPEND HULUD_HDF5_LIBS HDF5::HDF5)
elseif(TARGET hdf5-static)
    list(APPEND HULUD_HDF5_LIBS hdf5-static)
elseif(TARGET hdf5-shared)
    list(APPEND HULUD_HDF5_LIBS hdf5-shared)
elseif(DEFINED HDF5_C_STATIC_LIBRARY AND HDF5_C_STATIC_LIBRARY)
    list(APPEND HULUD_HDF5_LIBS ${HDF5_C_STATIC_LIBRARY})
elseif(DEFINED HDF5_LIBRARIES AND HDF5_LIBRARIES)
    list(APPEND HULUD_HDF5_LIBS ${HDF5_LIBRARIES})
else()
    message(FATAL_ERROR "HDF5 C library target not found")
endif()

if(TARGET HDF5::HL)
    list(APPEND HULUD_HDF5_LIBS HDF5::HL)
elseif(TARGET hdf5_hl-static)
    list(APPEND HULUD_HDF5_LIBS hdf5_hl-static)
elseif(TARGET hdf5_hl-shared)
    list(APPEND HULUD_HDF5_LIBS hdf5_hl-shared)
elseif(DEFINED HDF5_HL_STATIC_LIBRARY AND HDF5_HL_STATIC_LIBRARY)
    list(APPEND HULUD_HDF5_LIBS ${HDF5_HL_STATIC_LIBRARY})
elseif(DEFINED HDF5_HL_LIBRARIES AND HDF5_HL_LIBRARIES)
    list(APPEND HULUD_HDF5_LIBS ${HDF5_HL_LIBRARIES})
else()
    message(FATAL_ERROR "HDF5 HL library target not found; required for H5DOread_chunk")
endif()

# Static HDF5 builds do not always propagate these support libraries via the
# imported HDF5 targets, so keep them explicit on Hulud targets.
set(HULUD_HDF5_SUPPORT_LIBS ZLIB::ZLIB)
if(CMAKE_DL_LIBS)
    list(APPEND HULUD_HDF5_SUPPORT_LIBS ${CMAKE_DL_LIBS})
endif()
if(NOT WIN32)
    list(APPEND HULUD_HDF5_SUPPORT_LIBS m)
endif()

include_directories(include)
include_directories(src)
include_directories(third_party/bslz4/src)

# --- 1. External Dependency (bslz4) ---
# We build bslz4 as a static library that we can link against.
file(GLOB BSLZ4_SRCS "third_party/bslz4/src/*.c")
add_library(bslz4_bundled STATIC ${BSLZ4_SRCS})
target_include_directories(bslz4_bundled PUBLIC "third_party/bslz4/src")

# --- 2. Hulud Core Library (Static) ---
add_library(hulud_core STATIC
    src/io/file.c
    src/io/file_descriptor.c
    src/compression/bitshuffle_lz4.c
    src/detector/nexus.c
    src/detector/pilatus.c
    src/core/utils.c
    src/vds/vds_map.c
    src/io/readers.c
    src/core/err.c
)
target_link_libraries(hulud_core PRIVATE ${HULUD_HDF5_LIBS} bslz4_bundled Threads::Threads)

# --- 3. Hulud XDS Plugin (Shared Library) ---
add_library(hulud-plugin SHARED
    src/api/plugin_api.c
    src/plugin/runtime.c
)
# XDS expects a specific name, e.g. hulud-plugin.so
set_target_properties(hulud-plugin PROPERTIES PREFIX "" OUTPUT_NAME "hulud-plugin")
if(HULUD_PRODUCTION_PLUGIN)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "HULUD_PRODUCTION_PLUGIN currently supports only ELF targets")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "HULUD_PRODUCTION_PLUGIN requires a GNU-like linker that supports --version-script")
    endif()
    target_link_options(
        hulud-plugin
        PRIVATE
            "LINKER:--version-script=${HULUD_PLUGIN_VERSION_SCRIPT}"
    )
    set_property(TARGET hulud-plugin APPEND PROPERTY LINK_DEPENDS "${HULUD_PLUGIN_VERSION_SCRIPT}")
endif()
target_link_libraries(
    hulud-plugin
    PRIVATE
        hulud_core
        ${HULUD_HDF5_LIBS}
        ${HULUD_HDF5_SUPPORT_LIBS}
        Threads::Threads
)

# --- 4. Benchmark Tool ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark.c")
    add_executable(benchmark tools/benchmark.c)
    target_link_libraries(benchmark PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_executable(test_vds_image_numbers test/test_vds_image_numbers.c)
target_link_libraries(
    test_vds_image_numbers
    PRIVATE
        hulud-plugin
        ${HULUD_HDF5_LIBS}
        ${HULUD_HDF5_SUPPORT_LIBS}
)
add_test(NAME test_vds_image_numbers COMMAND test_vds_image_numbers)

# --- Installation (Optional but good practice) ---
install(
    TARGETS hulud-plugin
    LIBRARY DESTINATION lib
)

if(TARGET benchmark)
    install(
        TARGETS benchmark
        RUNTIME DESTINATION bin
    )
endif()


file(GLOB_RECURSE ALL_SOURCE_FILES
    "src/*.c"
    "src/*.h"
    "src/*.cpp"
    "src/*.hpp"
)

add_custom_target(
    format
    COMMAND clang-format -style=file -i ${ALL_SOURCE_FILES}
    COMMENT "Formatting source code with clang-format"
)
