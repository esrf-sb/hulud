Durin
=====

Durin is an XDS-compatible plugin for reading detector data from HDF5 files
(NeXus and Dectris/Pilatus layouts).

See:
- https://www.nexusformat.org
- https://www.dectris.com/products/pilatus3
- https://strucbio.biologie.uni-konstanz.de/xdswiki

## What Durin Builds

After a build, the main outputs are:
- `build/durin-plugin.so` (the shared library loaded by XDS)
- `build/benchmark` (optional benchmark/validation tool)
- `build/libdurin_core.a` and `build/libbslz4_bundled.a` (internal static libs)

## Requirements

- C compiler (C99)
- CMake >= 3.15
- HDF5 with `C` and `HL` components
- pthreads and `dl` (on Linux/glibc this is typically available by default)

### Notes for manual HDF5 builds

If you build HDF5 yourself, use a configuration compatible with threaded use
and `H5DOread_chunk`, for example:

```bash
../configure \
  --enable-threadsafe \
  --enable-deprecated-symbols \
  --enable-hl \
  --enable-unsupported
```

## Build

### Standard CMake build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Install (optional)

```bash
cmake --install build --prefix /opt/durin
```

### Build with pixi (optional)

This repository includes a `pixi.toml` workspace:

```bash
pixi run build
pixi run test
pixi run clean
```

### Build with conda env (optional)

```bash
conda env create -f conda_env.yml
conda activate durin-build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## XDS Usage

In your `XDS.INP`, add:

```text
LIB=/path/to/durin/build/durin-plugin.so
NAME_TEMPLATE_OF_DATA_FRAMES=/data/run/image_??????.h5
```

XDS will resolve this template to the corresponding master file
(`..._master.h5`) and call the plugin on that master file.

Durin supports master/data layouts where the selected `NXdata` or `NXdetector`
group exposes either:
- `data`
- `data_000001`, `data_000002`, ...

External links and VDS-backed datasets are supported.

## Frame Indexing and Gaps

Durin preserves detector frame indexing semantics and supports non-contiguous frame ranges.

- XDS requests frames with a 1-based `frame_number`.
- Internally, Durin converts to 0-based indexing before lookup.
- For Pilatus `data_XXXXXX` blocks, if dataset attributes
  `image_nr_low` and `image_nr_high` are present, Durin uses them as the global frame range for that block (`low/high` are converted to 0-based).
- If those attributes are not present, blocks are packed sequentially.

This means the logical frame space can contain holes (gaps) between blocks.
When a requested frame falls into a gap, Durin returns a zero-filled frame
buffer (success, not an error), matching VDS fill-value behavior.

## Public Plugin API

Durin exports the standard 4-entry plugin ABI in `include/durin/plugin_api.h`:

```c
void plugin_open(const char* filename, int info[1024], int* error_flag);
void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                       int* number_of_frames, int info[1024], int* error_flag);
void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                     int info[1024], int* error_flag);
void plugin_close(int* error_flag);
```

Lifecycle:
1. `plugin_open`
2. `plugin_get_header`
3. `plugin_get_data` (for each frame)
4. `plugin_close`

Conventions:
- `error_flag == 0` means success, negative values are errors.
- `frame_number` in `plugin_get_data` is 1-based.
- `nx` and `ny` in `plugin_get_data` are ABI compatibility arguments.
  Durin currently reads frame geometry from internal file metadata.

`info[]` metadata indexes currently used by Durin:
- `info[0]`: vendor/customer ID (`DLS_CUSTOMER_ID`, value `0x01`)
- `info[1]`: major version
- `info[2]`: minor version
- `info[3]`: patch version
- `info[4]`: version timestamp marker
- `info[5]`: physical frame count (sum of mapped frames, optional)
- `info[6]`: logical frame count (index span including gaps, optional)

## Runtime Environment Variables

- `DURIN_DEBUG=1` enables verbose debug logging.
- `DURIN_DISABLE_MASKING=1` disables pixel masking/casting.
- `DURIN_PLUGIN_FASTPATH=1` enables fast-path behavior when compatible.

## Benchmark Tool

The benchmark executable can load the built plugin and run single-thread and
multi-thread frame reads.

Example:

```bash
./build/benchmark /path/to/master.h5 -l ./build/durin-plugin.so -t 16
```

Options:
- `-l, --lib <path>`: plugin shared library path
- `-n, --frames <N>`: limit number of frames
- `-t, --threads <N>`: number of threads for parallel read test
- `--no-mask`: disable masking/casting
- `--debug`: force `DURIN_DEBUG=1`
- `--no-debug`: force `DURIN_DEBUG=0`
