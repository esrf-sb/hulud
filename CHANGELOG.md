# Changelog

All notable changes to the Hulud project (formerly Durin VDS) will be documented in this file.

## [Unreleased]
- **Breaking** - Project renamed from Durin to Hulud. The plugin is now `hulud-plugin.so` (update `LIB=` in `XDS.INP`), the public header moved to `include/hulud/plugin_api.h`, and the environment variables are now `HULUD_DEBUG`, `HULUD_DISABLE_MASKING` and `HULUD_PLUGIN_FASTPATH`.
- **Tool** - Strengthened `benchmark`'s per-frame verification from a plain additive sum to an FNV-1a 64-bit hash seeded with the requested frame index, so a swapped/mismatched frame changes the result instead of being masked by commutative addition.

## [1.4.6] - 2026-08-19
- **Bug** - Fixed VDS resolution for sources reached through an external HDF5 link: the raw-fd fast path now reads chunk bytes from the file HDF5 actually opened instead of the VDS-declared source path.
- **Bug** - Fixed a buffer overflow risk in the bitshuffle/LZ4 decompression error message by switching to `snprintf` with a correctly sized buffer.

## [1.4.5] - 2026-07-21
- **Bug** - Added support for compact VDS mappings whose source uses `SELECTION ALL`, including arbitrary, strided, and gapped virtual-to-source frame indices.
- **Performance** - Preserved constant-stride mapping runs and reused raw source file descriptors while building the direct per-frame index, reducing metadata loading overhead without changing the lock-free `pread` path.
- **Test** - Added regression coverage for irregular compact mappings such as `{0,2,5}` and `{1,3,4}` backed by unrestricted source selections.

## [1.4.4] - 2026-04-07
- **Reformating Code:** - Code restructuration and documentation
- **Build** - Adding pixi env. 
- **Bug** - Fixed bitshuffle/LZ4 decompression for chunks advertising `block_size=0` by using the bitshuffle default block size.
- **Bug** - Preserved logical frame numbering from `image_nr_low` / `image_nr_high` in the VDS fast path, so requested frame numbers map to the correct physical images.
- **Bug** - Hardened nested VDS frame-map expansion for interleaved Lima2-style mappings and removed the unsafe sequential re-packing of sparse mappings.
- **Test** - Added a regression test covering nested VDS frame selection with `STRIDE (2,1,1)` and non-contiguous logical frame numbering.

## [1.4.3] -  2026-02-10
- **Bug:** - Building error on apptainer

## [1.4.2] -  2026-01-19
### Known Issues
- **Bug:** - Enhance Pilatus dataset handling for non-contiguous frames
- **API** - Added to the benchmark a debug mode to follow the reading logic


## [1.4.1] -  2025-12-16
### Known Issues
- **Bug:** - External Links solver not using optmize reader
- **Reformating Code:** - Better comments. Removing benchmark replace with benchmark_plugin

## [1.4.0] -  2025-12-16
### Known Issues
- **Performance:** - New optimize reader (multiprocessing) for non vds/lima2 data.

## [1.3.1] -  2025-12-11
### Known Issues
 - **Relative Absolute Path** - Resolved Issue with relative path and symbolic links resolution
 - **Race competition** - Segmentation fault. Race competition on the scratch 

## [1.3.0] - 2025-12-02
### Added
- **Multi-threading support:** Implemented true parallel file reading.
- **Direct OS I/O:** Replaced the HDF5 library's internal chunk I/O with direct system calls (`pread`) and local memory buffers.

### Changed
- **Performance:** Bypassed the HDF5 Global Lock entirely during the read phase.
- **Throughput:** Significantly increased read bandwidth on GPFS systems by allowing concurrent access from multiple threads.

## [1.2.0] - 2025-11-28
### Added
- **"Direct Chunk" read path:** Implemented a new reading strategy for VDS datasets.

### Changed
- **Optimization:** Moved VDS mapping logic to the initialization phase (startup) rather than per-frame execution.
- **Decompression:** Manually handles Bitshuffle/LZ4 decompression after raw data transfer, bypassing the standard HDF5 filter pipeline overhead.

## [1.1.0]
- **[YANKED]**
*Internal release. Withdrawn due to instability.*

## [1.0.0] - 2025-11-25
### Added
- Initial implementation of the Durin plugin adapted for Lima2 VDS data.
- Native support for reading Virtual Datasets (VDS), resolving compatibility issues found in Neggia.

### Known Issues
- **Performance:** Read operations on GPFS are slow due to the standard HDF5 library performing repetitive mapping lookups and locking for every frame request.
