/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#include "plugin/runtime.h"

#include <hulud/plugin_api.h>
#include <fenv.h>
#include <hdf5.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/file.h"

#define ERROR_OUTPUT stderr

typedef char hulud_requires_32bit_int[(sizeof(int) == sizeof(int32_t)) ? 1 : -1];

static hid_t file_id = 0;
static struct ds_desc_t* data_desc = NULL;
static int* mask_buffer = NULL;
static int fast_path = 0;
static pthread_mutex_t plugin_mutex = PTHREAD_MUTEX_INITIALIZER;
static int h5_atexit_configured = 0;
static int apply_mask_and_cast = 1;
extern int hulud_debug_enabled;
// True only when the selected reader does not touch the HDF5 API (parallel fast-path)
static int data_reader_thread_safe = 0;

struct thread_tmp_buffer {
    void* buffer;
    size_t size;
};
static pthread_once_t tmp_buffer_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t tmp_buffer_key;

struct fp_env_guard {
    fenv_t env;
    int active;
};

static struct fp_env_guard fp_env_guard_begin(void) {
    struct fp_env_guard guard;
    guard.active = (feholdexcept(&guard.env) == 0);
    return guard;
}

static void fp_env_guard_end(struct fp_env_guard* guard) {
    if (guard && guard->active) {
        fesetenv(&guard->env);
        guard->active = 0;
    }
}

static void destroy_thread_tmp(void* ptr) {
    struct thread_tmp_buffer* state = ptr;
    if (state) {
        free(state->buffer);
        free(state);
    }
}

static void init_tmp_buffer_key(void) { pthread_key_create(&tmp_buffer_key, destroy_thread_tmp); }

static void* get_thread_tmp_buffer(size_t needed) {
    pthread_once(&tmp_buffer_key_once, init_tmp_buffer_key);
    struct thread_tmp_buffer* state = pthread_getspecific(tmp_buffer_key);
    if (!state) {
        state = calloc(1, sizeof(*state));
        if (!state) return NULL;
        pthread_setspecific(tmp_buffer_key, state);
    }
    if (state->size < needed) {
        void* nb = realloc(state->buffer, needed);
        if (!nb) return NULL;
        state->buffer = nb;
        state->size = needed;
    }
    return state->buffer;
}

static int env_truthy(const char* v) { return v && v[0] != '0' && v[0] != '\0'; }

static void fill_info_array(int info[HULUD_PLUGIN_INFO_SIZE]) {
    info[0] = DLS_CUSTOMER_ID;
    info[1] = VERSION_MAJOR;
    info[2] = VERSION_MINOR;
    info[3] = VERSION_PATCH;
    info[4] = VERSION_TIMESTAMP;
}

static void apply_mask_to_int32(int32_t* buffer, const int* mask, int length) {
    if (!mask) return;
    for (int i = 0; i < length; ++i) {
        if (mask[i] & 0xFF) buffer[i] = -1;
        if (mask[i] & 30) buffer[i] = -2;
    }
}

static int convert_to_int32_and_mask(const void* in_buffer, int d_width, int data_is_signed,
                                     int32_t* out_buffer, int length, const int* mask) {
    int retval = 0;
    int i = 0;

    switch (d_width) {
        case 1:
            if (data_is_signed) {
                const int8_t* in = in_buffer;
                for (i = 0; i < length; ++i) out_buffer[i] = (int32_t)in[i];
            } else {
                const uint8_t* in = in_buffer;
                for (i = 0; i < length; ++i) out_buffer[i] = (int32_t)in[i];
            }
            break;
        case 2:
            if (data_is_signed) {
                const int16_t* in = in_buffer;
                for (i = 0; i < length; ++i) out_buffer[i] = (int32_t)in[i];
            } else {
                const uint16_t* in = in_buffer;
                for (i = 0; i < length; ++i) out_buffer[i] = (int32_t)in[i];
            }
            break;
        case 4:
            /*
             * The plugin ABI exposes int32_t pixels. For 32-bit detector data we preserve the
             * on-wire 32-bit bit pattern, which matches the historical behavior and keeps common
             * unsigned sentinels such as 0xFFFFFFFF visible as -1 to downstream consumers.
             */
            memcpy(out_buffer, in_buffer, (size_t)length * sizeof(*out_buffer));
            break;
        case 8:
            if (data_is_signed) {
                const int64_t* in = in_buffer;
                for (i = 0; i < length; ++i) {
                    if (in[i] < (int64_t)INT32_MIN || in[i] > (int64_t)INT32_MAX) {
                        ERROR_JUMP(-1, done, "Signed 64-bit pixel value exceeds int32_t range");
                    }
                    out_buffer[i] = (int32_t)in[i];
                }
            } else {
                const uint64_t* in = in_buffer;
                for (i = 0; i < length; ++i) {
                    if (in[i] > (uint64_t)INT32_MAX) {
                        ERROR_JUMP(-1, done, "Unsigned 64-bit pixel value exceeds int32_t range");
                    }
                    out_buffer[i] = (int32_t)in[i];
                }
            }
            break;
        default: {
            char message[128];
            sprintf(message, "Unsupported conversion of data width %d to int32_t", d_width);
            ERROR_JUMP(-1, done, message);
        }
    }

    apply_mask_to_int32(out_buffer, mask, length);
done:
    return retval;
}

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open an HDF5 master file and prepare the plugin state.
 * Fills version info, configures HDF5 error handling, and builds the detector
 * descriptor (including optional pixel mask).
 */
void hulud_plugin_runtime_open(const char* filename, int info[HULUD_PLUGIN_INFO_SIZE],
                               int* error_flag) {
    struct fp_env_guard fp_guard = fp_env_guard_begin();
    int retval = 0;
    *error_flag = 0;

    /* Open is still serialized */
    pthread_mutex_lock(&plugin_mutex);

    init_error_handling();

    if (!h5_atexit_configured) {
        h5_atexit_configured = 1;
        if (H5dont_atexit() < 0) {
            reset_error_stack();
        }
    }

    if (init_h5_error_handling() < 0) {
        ERROR_JUMP(-2, done, "Failed to configure HDF5 error handling");
    }

    fill_info_array(info);
    const char* fp = getenv("HULUD_PLUGIN_FASTPATH");
    fast_path = env_truthy(fp);

    /* Enable verbose debug logging if requested */
    hulud_debug_enabled = env_truthy(getenv("HULUD_DEBUG"));
    if (hulud_debug_enabled) {
        fprintf(stderr, "[HULUD] Debug logging enabled\n");
    }

    const char* disable_mask_env = getenv("HULUD_DISABLE_MASKING");
    apply_mask_and_cast = !env_truthy(disable_mask_env);

    file_id = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        char message[128] = {0};
        sprintf(message, "Could not open %.100s", filename);
        ERROR_JUMP(-4, done, message);
    }

    reset_error_stack();
    retval = get_detector_info(file_id, &data_desc);
    if (retval < 0) {
        ERROR_JUMP(-4, done, "");
    }

    /* Only the fast parallel reader avoids HDF5 calls and is thread-safe */
    data_reader_thread_safe =
        (data_desc && data_desc->get_data_frame == get_virtual_frame_parallel);

    if (apply_mask_and_cast) {
        mask_buffer = malloc(data_desc->dims[1] * data_desc->dims[2] * sizeof(int));
        if (mask_buffer) {
            retval = data_desc->get_pixel_mask(data_desc, mask_buffer);
            if (retval < 0) {
                fprintf(ERROR_OUTPUT,
                        "WARNING: Could not read pixel mask - no masking will be applied\n");
                dump_error_stack(ERROR_OUTPUT);
                free(mask_buffer);
                mask_buffer = NULL;
            }
        }
        retval = 0;
    }

done:
    *error_flag = retval;
    pthread_mutex_unlock(&plugin_mutex);
    if (retval < 0) {
        if ((data_desc) && (data_desc->free_desc)) {
            data_desc->free_desc(data_desc);
            data_desc = NULL;
        }
        dump_error_stack(ERROR_OUTPUT);
    }
    fp_env_guard_end(&fp_guard);
}

/**
 * Retrieve detector geometry and frame count from the open descriptor.
 */
void hulud_plugin_runtime_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy,
                                     int* number_of_frames, int info[HULUD_PLUGIN_INFO_SIZE],
                                     int* error_flag) {
    struct fp_env_guard fp_guard = fp_env_guard_begin();
    int err = 0;
    int retval = 0;
    double x_pixel_size, y_pixel_size;
    int physical_frames = 0;

    pthread_mutex_lock(&plugin_mutex);

    err = data_desc->get_pixel_properties(data_desc, &x_pixel_size, &y_pixel_size);
    if (err < 0) {
        ERROR_JUMP(err, done, "Failed to retrieve pixel information");
    }

    *nx = data_desc->dims[2];
    *ny = data_desc->dims[1];
    *nbytes = data_desc->data_width;
    *number_of_frames = data_desc->dims[0];
    *qx = (float)x_pixel_size;
    *qy = (float)y_pixel_size;

    /* Report physical frame count (sum of real datasets) in info[5] for tooling */
    if (data_desc) {
        /* Pilatus descriptors embed block_sizes */
        struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)data_desc;
        if (pilatus_desc->block_sizes && pilatus_desc->n_data_blocks > 0) {
            hsize_t sum = 0;
            for (int i = 0; i < pilatus_desc->n_data_blocks; ++i)
                sum += (hsize_t)pilatus_desc->block_sizes[i];
            if (sum < INT_MAX) physical_frames = (int)sum;
        }
    }
    if (physical_frames > 0) {
        info[5] = physical_frames;   /* mapped frames */
        info[6] = *number_of_frames; /* logical frames (including gaps) */
    }

done:
    *error_flag = retval;
    pthread_mutex_unlock(&plugin_mutex);
    if (retval < 0) {
        dump_error_stack(ERROR_OUTPUT);
    }
    fp_env_guard_end(&fp_guard);
}

/**
 * Read a frame (1-based index) into the caller-provided buffer.
 * Uses the fast path when possible; otherwise protects HDF5 calls with a mutex.
 */
void hulud_plugin_runtime_get_data(int* frame_number, int* nx, int* ny, int* data_array,
                                   int info[HULUD_PLUGIN_INFO_SIZE], int* error_flag) {
    struct fp_env_guard fp_guard = fp_env_guard_begin();
    int retval = 0;
    int frame_size_px = data_desc->dims[1] * data_desc->dims[2];
    int need_lock = !data_reader_thread_safe;
    int32_t* out_buffer = (int32_t*)data_array;

    /* * --- MULTI-THREADING CHANGE ---
     * Locks DISABLED to allow parallel execution.
     */
    if (need_lock) pthread_mutex_lock(&plugin_mutex);

    void* buffer = NULL;
    if ((int)sizeof(*out_buffer) == data_desc->data_width) {
        buffer = out_buffer;
    } else {
        size_t needed = (size_t)data_desc->data_width * frame_size_px;
        buffer = get_thread_tmp_buffer(needed);
        if (!buffer) {
            ERROR_JUMP(-1, done, "Unable to allocate data buffer");
        }
    }

    /* 1-based index from XDS converted to 0-based */
    if (data_desc->get_data_frame(data_desc, (*frame_number) - 1, buffer) < 0) {
        char message[64] = {0};
        sprintf(message, "Failed to retrieve data for frame %d", *frame_number);
        ERROR_JUMP(-2, done, message);
    }

    if (fast_path && buffer == out_buffer && mask_buffer == NULL && apply_mask_and_cast) {
        /* nothing further to do */
    } else if (buffer != out_buffer) {
        if (convert_to_int32_and_mask(buffer, data_desc->data_width, data_desc->data_is_signed,
                                      out_buffer, frame_size_px,
                                      apply_mask_and_cast ? mask_buffer : NULL) < 0) {
            char message[64];
            sprintf(message, "Error converting data for frame %d", *frame_number);
            ERROR_JUMP(-2, done, message);
        }
    } else if (apply_mask_and_cast) {
        apply_mask_to_int32(out_buffer, mask_buffer, frame_size_px);
    }

done:
    *error_flag = retval;
    if (retval < 0) {
        dump_error_stack(ERROR_OUTPUT);
    }
    if (need_lock) pthread_mutex_unlock(&plugin_mutex);
    fp_env_guard_end(&fp_guard);
}

/**
 * Close all plugin resources and release HDF5 handles.
 */
void hulud_plugin_runtime_close(int* error_flag) {
    struct fp_env_guard fp_guard = fp_env_guard_begin();
    pthread_mutex_lock(&plugin_mutex);
    data_reader_thread_safe = 0;
    if (file_id) {
        if (H5Fclose(file_id) < 0) {
            *error_flag = -1;
        }
    }
    file_id = 0;

    if (mask_buffer) {
        free(mask_buffer);
        mask_buffer = NULL;
    }
    if (data_desc && data_desc->free_desc) {
        data_desc->free_desc(data_desc);
        data_desc = NULL;
    }
    pthread_mutex_unlock(&plugin_mutex);
    if (H5close() < 0) {
        *error_flag = -1;
    }
    fp_env_guard_end(&fp_guard);
}

#ifdef __cplusplus
}
#endif
