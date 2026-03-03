/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#ifndef DURIN_PLUGIN_API_H
#define DURIN_PLUGIN_API_H

#ifdef __cplusplus
extern "C" {
#endif

enum { DURIN_PLUGIN_INFO_SIZE = 1024 };

#define DLS_CUSTOMER_ID 0x01 /* pretend we're Dectris, otherwise XDS doesn't work */
#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define VERSION_PATCH 0
#define VERSION_TIMESTAMP -1 /* good enough for Dectris apparantely */

typedef void (*plugin_open_fn)(const char* filename, int info[DURIN_PLUGIN_INFO_SIZE],
                               int* error_flag);
typedef void (*plugin_get_header_fn)(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                                     int* number_of_frames, int info[DURIN_PLUGIN_INFO_SIZE],
                                     int* error_flag);
typedef void (*plugin_get_data_fn)(int* frame_number, int* nx, int* ny, int* data_array,
                                   int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag);
typedef void (*plugin_close_fn)(int* error_flag);

/** Open an HDF5 master file and initialize plugin state (thread-safe wrapper). */
void plugin_open(const char* filename, int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag);

/** Retrieve geometry/pixel size/frame count for the current file. */
void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                       int* number_of_frames, int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag);

/** Read a single frame (1-based index) into caller-provided buffer. */
void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                     int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag);

/** Release all resources associated with the open plugin/file. */
void plugin_close(int* error_flag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DURIN_PLUGIN_API_H */
