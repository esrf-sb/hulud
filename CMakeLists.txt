cmake_minimum_required(VERSION 3.15)
project(durin C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND (NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL ""))
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- Dependencies ---
find_package(HDF5 REQUIRED COMPONENTS C HL)
find_package(Threads REQUIRED)

set(DURIN_HDF5_LIBS HDF5::HDF5)
if(TARGET HDF5::HL)
    list(APPEND DURIN_HDF5_LIBS HDF5::HL)
elseif(TARGET hdf5_hl-shared)
    list(APPEND DURIN_HDF5_LIBS hdf5_hl-shared)
elseif(DEFINED HDF5_HL_LIBRARIES AND HDF5_HL_LIBRARIES)
    list(APPEND DURIN_HDF5_LIBS ${HDF5_HL_LIBRARIES})
else()
    message(FATAL_ERROR "HDF5 HL library target not found; required for H5DOread_chunk")
endif()


include_directories(include)
include_directories(src)
include_directories(third_party/bslz4/src)

# --- 1. External Dependency (bslz4) ---
# We build bslz4 as a static library that we can link against.
file(GLOB BSLZ4_SRCS "third_party/bslz4/src/*.c")
add_library(bslz4_bundled STATIC ${BSLZ4_SRCS})
target_include_directories(bslz4_bundled PUBLIC "third_party/bslz4/src")

# --- 2. Durin Core Library (Static) ---
add_library(durin_core STATIC
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
target_link_libraries(durin_core PRIVATE ${DURIN_HDF5_LIBS} bslz4_bundled Threads::Threads)

# --- 3. Durin XDS Plugin (Shared Library) ---
add_library(durin-plugin SHARED
    src/api/plugin_api.c
    src/plugin/runtime.c
)
# XDS expects a specific name, e.g. durin-plugin.so
set_target_properties(durin-plugin PROPERTIES PREFIX "" OUTPUT_NAME "durin-plugin")
target_link_libraries(durin-plugin PRIVATE durin_core ${DURIN_HDF5_LIBS} Threads::Threads)

# --- 4. Benchmark Tool ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark.c")
    add_executable(benchmark tools/benchmark.c)
    # The benchmark tool needs to link to the core library and HDF5.
    target_link_libraries(benchmark PRIVATE durin_core ${DURIN_HDF5_LIBS} -ldl)
endif()

# --- Installation (Optional but good practice) ---
install(
    TARGETS durin-plugin
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
