Hulud
=====

Hulud is an XDS plugin for reading detector data from HDF5 files
(NeXus and Dectris/Pilatus layouts).

## Why Hulud

Hulud is built for fast, parallel CPU reading of HDF5 Virtual Datasets (VDS),
such as those written by Lima2:

- **Native VDS support**, including nested, strided, gapped and compact
  mappings, resolved once at open time instead of on every frame.
- **Lock-free parallel reads**: frame chunks are read directly with `pread`,
  bypassing the HDF5 global lock, so many XDS threads can read concurrently.
- **In-plugin Bitshuffle/LZ4 decompression**, skipping the HDF5 filter pipeline.
- **High throughput on parallel file systems** such as GPFS.
- **Safe fallback**: when the fast path does not apply, Hulud falls back to
  standard (serialized) HDF5 reads.

## Origin

Hulud is derived from [durin](https://github.com/DiamondLightSource/durin)
by Diamond Light Source, and has since been extensively reworked at the ESRF.
The original copyright notice is kept in `LICENSE` (BSD 3-Clause).
Licenses of the bundled HDF5, Bitshuffle and LZ4 code are in
`THIRD_PARTY_NOTICES`.

## Prebuilt Plugin

Each release provides `hulud-vX.Y.Z-linux-x86_64.tar.gz`, containing
`hulud-plugin.so`, `LICENSE` and `THIRD_PARTY_NOTICES`. The plugin is a
standalone file (HDF5 is built in) that runs on any x86-64 Linux with:
- glibc >= 2.28 (RHEL/Rocky/Alma 8+, Ubuntu 20.04+, Debian 10+)
- `libz.so.1` (zlib)

`hulud-plugin.so` is all XDS needs. If you redistribute it, include `LICENSE`
and `THIRD_PARTY_NOTICES` with it.

## Build

### Portable plugin (recommended)

```bash
pixi run build-prod
```

Produces `build-prod/hulud-plugin.so`, the same portable build as the
releases: static HDF5 `1.14.3`, glibc 2.28 baseline, and only the four
`plugin_*` functions exported. `scripts/check-portable.sh` runs at the end and
fails the build if the plugin is not portable.

### Development build

Requires a C99 compiler, CMake >= 3.15 and HDF5 with the `C` and `HL`
components, built thread-safe:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Or with pixi: `pixi run build`, `pixi run test`.

Outputs: `build/hulud-plugin.so` and `build/benchmark`.

## XDS Usage

In your `XDS.INP`, add:

```text
LIB=/path/to/hulud-plugin.so
NAME_TEMPLATE_OF_DATA_FRAMES=/data/run/image_??????.h5
```

XDS resolves this template to the master file (`..._master.h5`) and calls the
plugin on it. The selected `NXdata` or `NXdetector` group must expose either
`data` or `data_000001`, `data_000002`, ... External links and VDS-backed
datasets are supported.

## Frame Indexing and Gaps

- XDS requests frames with a 1-based `frame_number`.
- For Pilatus `data_XXXXXX` blocks with `image_nr_low` / `image_nr_high`
  attributes, Hulud uses them as the block's global frame range.
  Without them, blocks are packed sequentially.
- The frame space can therefore contain gaps. A frame in a gap is returned
  zero-filled (success, not an error), matching VDS fill-value behavior.

## Public Plugin API

Hulud exports the standard 4-entry plugin ABI in `include/hulud/plugin_api.h`:

```c
void plugin_open(const char* filename, int info[1024], int* error_flag);
void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                       int* number_of_frames, int info[1024], int* error_flag);
void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                     int info[1024], int* error_flag);
void plugin_close(int* error_flag);
```

- Call order: `plugin_open`, `plugin_get_header`, `plugin_get_data` (per frame),
  `plugin_close`.
- `error_flag == 0` means success, negative values are errors.
- `nx` and `ny` in `plugin_get_data` are ignored; geometry comes from the file.

`info[]` indexes:
- `info[0]`: vendor/customer ID (`DLS_CUSTOMER_ID`, value `0x01`)
- `info[1..3]`: major, minor, patch version
- `info[4]`: version timestamp marker
- `info[5]`: physical frame count (sum of mapped frames, optional)
- `info[6]`: logical frame count (index span including gaps, optional)

## Runtime Environment Variables

- `HULUD_DEBUG=1` enables verbose debug logging.
- `HULUD_DISABLE_MASKING=1` disables pixel masking/casting.
- `HULUD_PLUGIN_FASTPATH=1` enables fast-path behavior when compatible.

## Benchmark Tool

`benchmark` loads the plugin and times single-thread and multi-thread frame
reads:

```bash
./build/benchmark /path/to/master.h5 -l ./build/hulud-plugin.so -t 16
```

Options:
- `-l, --lib <path>`: plugin shared library path
- `-n, --frames <N>`: limit number of frames
- `-t, --threads <N>`: number of threads for parallel read test
- `--no-mask`: disable masking/casting
- `--debug` / `--no-debug`: force `HULUD_DEBUG=1` / `0`

### Example result

This is only an example from one run; results depend on the machine, the file
system, the dataset and the cache state. VDS dataset on GPFS, 2280 frames of
2073 x 2180 pixels, stored **Bitshuffle/LZ4 compressed** and decompressed by
Hulud:

```bash
./build/benchmark /path/to/master.h5 -l ./build/hulud-plugin.so --no-mask -t 16
```

```text
Metadata Read: 0.5160s | Frames: 2280 | Size: 2073 x 2180 | Bytes: 4

[Test 1] Single Thread (Main Thread Only)...
  Time:         14.5014 s
  Speed:        157.23 FPS
  Throughput:   2710.45 MB/s

[Test 2] Multi Thread (16 threads)...
  Time:         1.2754 s
  Speed:        1787.69 FPS
  Throughput:   30818.31 MB/s
```

Throughput is measured on the decompressed frames, not on the compressed bytes
read from disk.
