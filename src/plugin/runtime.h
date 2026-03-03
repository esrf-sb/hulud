#ifndef DURIN_PLUGIN_RUNTIME_H
#define DURIN_PLUGIN_RUNTIME_H

#include <durin/plugin_api.h>

#ifdef __cplusplus
extern "C" {
#endif

void durin_plugin_runtime_open(const char* filename, int info[DURIN_PLUGIN_INFO_SIZE],
                               int* error_flag);
void durin_plugin_runtime_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                                     int* number_of_frames, int info[DURIN_PLUGIN_INFO_SIZE],
                                     int* error_flag);
void durin_plugin_runtime_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                                   int info[DURIN_PLUGIN_INFO_SIZE], int* error_flag);
void durin_plugin_runtime_close(int* error_flag);

#ifdef __cplusplus
}
#endif

#endif