#ifndef HULUD_PLUGIN_RUNTIME_H
#define HULUD_PLUGIN_RUNTIME_H

#include <hulud/plugin_api.h>

#ifdef __cplusplus
extern "C" {
#endif

void hulud_plugin_runtime_open(const char* filename, int info[HULUD_PLUGIN_INFO_SIZE],
                               int* error_flag);
void hulud_plugin_runtime_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                                     int* number_of_frames, int info[HULUD_PLUGIN_INFO_SIZE],
                                     int* error_flag);
void hulud_plugin_runtime_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                                   int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag);
void hulud_plugin_runtime_close(int* error_flag);

#ifdef __cplusplus
}
#endif

#endif