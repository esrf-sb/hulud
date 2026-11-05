/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#ifndef HULUD_PLUGIN_API_H
#define HULUD_PLUGIN_API_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file plugin_api.h
 * @brief Public C ABI exported by the Hulud plugin shared library.
 *
 * Hulud exposes the XDS/Neggia-compatible detector plugin callbacks.
 * The API is stateful and is expected to be used in this order:
 *   1. plugin_open()
 *   2. plugin_get_header()
 *   3. plugin_get_data() for each requested frame
 *   4. plugin_close()
 *
 * All pointer arguments must be valid (non-NULL). Status is returned through
 * `error_flag` where 0 means success and negative values indicate errors.
 */
enum {
    /** Size of the metadata buffer expected by this API. */
    HULUD_PLUGIN_INFO_SIZE = 1024,

    /** XDS customer/vendor identifier used for compatibility. */
    HULUD_PLUGIN_INFO_CUSTOMER_ID = 0,
    /** Plugin major version. */
    HULUD_PLUGIN_INFO_VERSION_MAJOR = 1,
    /** Plugin minor version. */
    HULUD_PLUGIN_INFO_VERSION_MINOR = 2,
    /** Plugin patch version. */
    HULUD_PLUGIN_INFO_VERSION_PATCH = 3,
    /** Plugin build timestamp/revision marker. */
    HULUD_PLUGIN_INFO_VERSION_TIMESTAMP = 4,
    /** Optional physical frame count (sum of real data blocks). */
    HULUD_PLUGIN_INFO_PHYSICAL_FRAMES = 5,
    /** Optional logical frame count (including mapped gaps). */
    HULUD_PLUGIN_INFO_LOGICAL_FRAMES = 6,
};

#define DLS_CUSTOMER_ID 0x01 /* pretend we're Dectris, otherwise XDS doesn't work */
#define VERSION_MAJOR 2
#define VERSION_MINOR 0
#define VERSION_PATCH 1
#define VERSION_TIMESTAMP -1 /* good enough for Dectris apparantely */

/** Function pointer signature for `plugin_open` (typically used with `dlsym`). */
typedef void (*plugin_open_fn)(const char* filename, int info[HULUD_PLUGIN_INFO_SIZE],
                               int* error_flag);
/** Function pointer signature for `plugin_get_header`. */
typedef void (*plugin_get_header_fn)(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                                     int* number_of_frames, int info[HULUD_PLUGIN_INFO_SIZE],
                                     int* error_flag);
/** Function pointer signature for `plugin_get_data`. */
typedef void (*plugin_get_data_fn)(int* frame_number, int* nx, int* ny, int* data_array,
                                   int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag);
/** Function pointer signature for `plugin_close`. */
typedef void (*plugin_close_fn)(int* error_flag);

/**
 * Open an HDF5 master file and initialize plugin runtime state.
 *
 * @param filename Path to the master HDF5 file.
 * @param info Metadata array of size `HULUD_PLUGIN_INFO_SIZE`.
 *             On success, entries at `HULUD_PLUGIN_INFO_*` indexes are
 *             populated; other entries are reserved for future use.
 * @param error_flag Output status code.
 */
void plugin_open(const char* filename, int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag);

/**
 * Retrieve image geometry and detector metadata for the opened file.
 *
 * @param nx Output image width in pixels.
 * @param ny Output image height in pixels.
 * @param nbytes Output source pixel width in bytes.
 * @param qx Output pixel size along X, in meters.
 * @param qy Output pixel size along Y, in meters.
 * @param number_of_frames Output logical number of readable frames.
 * @param info Metadata array. `HULUD_PLUGIN_INFO_PHYSICAL_FRAMES` and
 *             `HULUD_PLUGIN_INFO_LOGICAL_FRAMES` may be updated when available.
 * @param error_flag Output status code.
 */
void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                       int* number_of_frames, int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag);

/**
 * Read one frame into the caller-provided output buffer.
 *
 * @param frame_number 1-based frame index (as requested by XDS).
 * @param nx ABI compatibility parameter (currently ignored by Hulud).
 * @param ny ABI compatibility parameter (currently ignored by Hulud).
 * @param data_array Output frame buffer of at least `nx * ny` integers.
 * @param info Metadata array (reserved for compatibility and future use).
 * @param error_flag Output status code.
 */
void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                     int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag);

/**
 * Release all resources associated with the currently opened file/plugin state.
 *
 * @param error_flag Output status code.
 */
void plugin_close(int* error_flag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HULUD_PLUGIN_API_H */
