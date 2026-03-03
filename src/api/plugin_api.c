/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#include <durin/plugin_api.h>

#include "plugin/runtime.h"

void plugin_open(const char* filename, int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag) {
    durin_plugin_runtime_open(filename, info, error_flag);
}

void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                       int* number_of_frames, int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag) {
    durin_plugin_runtime_get_header(nx, ny, nbytes, qx, qy, number_of_frames, info, error_flag);
}

void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                     int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag) {
    durin_plugin_runtime_get_data(frame_number, nx, ny, data_array, info, error_flag);
}

void plugin_close(int* error_flag) { durin_plugin_runtime_close(error_flag); }
