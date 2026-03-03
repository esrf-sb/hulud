cmake_minimum_required(VERSION 3.15)
project(durin C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Dependencies ---
find_package(HDF5 REQUIRED COMPONENTS C HL)
find_package(Threads REQUIRED)

# --- Include Directories ---
# Add public include dir for our library
include_directories(include)
include_directories(src)
# Add external dependency include dir
include_directories(bslz4/src)

# --- 1. External Dependency (bslz4) ---
# We build bslz4 as a static library that we can link against.
file(GLOB BSLZ4_SRCS "bslz4/src/*.c")
add_library(bslz4_bundled STATIC ${BSLZ4_SRCS})
target_include_directories(bslz4_bundled PUBLIC "bslz4/src")

# --- 2. Durin Core Library (Static) ---
# This library contains the core logic (file parsing, VDS mapping, etc.)
# It is static because it's an internal component for the plugin and benchmark tool.
add_library(durin_core STATIC
    src/io/file.c
    src/io/file_descriptor.c
    src/filters.c
    src/detector/nexus.c
    src/detector/pilatus.c
    src/core/utils.c
    src/vds/vds_map.c
    src/io/readers.c
    src/core/err.c
)
target_link_libraries(durin_core PRIVATE HDF5::HDF5 bslz4_bundled Threads::Threads)

# --- 3. Durin XDS Plugin (Shared Library) ---
# This is the main output for XDS.
add_library(durin-plugin SHARED
    src/plugin.c
)
# XDS expects a specific name, e.g. durin-plugin.so
set_target_properties(durin-plugin PROPERTIES PREFIX "" OUTPUT_NAME "durin-plugin")
target_link_libraries(durin-plugin PRIVATE durin_core)

# --- 4. Benchmark Tool ---
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark.c")
    add_executable(benchmark src/benchmark.c)
    # The benchmark tool needs to link to the core library and HDF5.
    target_link_libraries(benchmark PRIVATE durin_core HDF5::HDF5 -ldl)
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
