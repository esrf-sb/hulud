/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#include "plugin.h"

#include <hdf5.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"
#include "filters.h"

#define ERROR_OUTPUT stderr

/* mask bits loosely based on what Neggia does */
#define COPY_AND_MASK(in, out, size, mask)       \
    {                                            \
        int i;                                   \
        if (mask) {                              \
            for (i = 0; i < size; ++i) {         \
                out[i] = in[i];                  \
                if (mask[i] & 0xFF) out[i] = -1; \
                if (mask[i] & 30) out[i] = -2;   \
            }                                    \
        } else {                                 \
            for (i = 0; i < size; i++) {         \
                out[i] = in[i];                  \
            }                                    \
        }                                        \
    }

#define APPLY_MASK(buffer, mask, size)              \
    {                                               \
        int i;                                      \
        if (mask) {                                 \
            for (i = 0; i < size; ++i) {            \
                if (mask[i] & 0xFF) buffer[i] = -1; \
                if (mask[i] & 30) buffer[i] = -2;   \
            }                                       \
        }                                           \
    }

static hid_t file_id = 0;
static struct ds_desc_t* data_desc = NULL;
static int* mask_buffer = NULL;
static int fast_path = 0;
static pthread_mutex_t plugin_mutex = PTHREAD_MUTEX_INITIALIZER;
static int h5_atexit_configured = 0;
static int apply_mask_and_cast = 1;

struct thread_tmp_buffer {
    void* buffer;
    size_t size;
};
static pthread_once_t tmp_buffer_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t tmp_buffer_key;

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

void fill_info_array(int info[1024]) {
    info[0] = DLS_CUSTOMER_ID;
    info[1] = VERSION_MAJOR;
    info[2] = VERSION_MINOR;
    info[3] = VERSION_PATCH;
    info[4] = VERSION_TIMESTAMP;
}

int convert_to_int_and_mask(void* in_buffer, int d_width, int* out_buffer, int length, int* mask) {
    int retval = 0;
    if (d_width == sizeof(signed char)) {
        signed char* in = in_buffer;
        COPY_AND_MASK(in, out_buffer, length, mask);
    } else if (d_width == sizeof(short)) {
        short* in = in_buffer;
        COPY_AND_MASK(in, out_buffer, length, mask);
    } else if (d_width == sizeof(int)) {
        int* in = in_buffer;
        COPY_AND_MASK(in, out_buffer, length, mask);
    } else if (d_width == sizeof(long int)) {
        long int* in = in_buffer;
        COPY_AND_MASK(in, out_buffer, length, mask);
    } else if (d_width == sizeof(long long int)) {
        long long int* in = in_buffer;
        COPY_AND_MASK(in, out_buffer, length, mask);
    } else {
        char message[128];
        sprintf(message, "Unsupported conversion of data width %d to %ld (int)", d_width,
                sizeof(int));
        ERROR_JUMP(-1, done, message);
    }
done:
    return retval;
}

#ifdef __cplusplus
extern "C" {
#endif

void plugin_open(const char* filename, int info[1024], int* error_flag) {
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
    const char* fp = getenv("DURIN_PLUGIN_FASTPATH");
    fast_path = env_truthy(fp);

    const char* disable_mask_env = getenv("DURIN_DISABLE_MASKING");
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
}

void plugin_get_header(int* nx, int* ny, int* nbytes, float* qx, float* qy, int* number_of_frames,
                       int info[1024], int* error_flag) {
    int err = 0;
    int retval = 0;
    double x_pixel_size, y_pixel_size;

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

done:
    *error_flag = retval;
    pthread_mutex_unlock(&plugin_mutex);
    if (retval < 0) {
        dump_error_stack(ERROR_OUTPUT);
    }
}

void plugin_get_data(int* frame_number, int* nx, int* ny, int* data_array, int info[1024],
                     int* error_flag) {
    int retval = 0;
    int frame_size_px = data_desc->dims[1] * data_desc->dims[2];

    /* * --- MULTI-THREADING CHANGE ---
     * Locks DISABLED to allow parallel execution.
     */
    /* pthread_mutex_lock(&plugin_mutex); */

    void* buffer = NULL;
    if ((int)sizeof(*data_array) == data_desc->data_width) {
        buffer = data_array;
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

    if (!apply_mask_and_cast) {
        if (buffer != data_array) {
            size_t pixel_bytes = (size_t)data_desc->data_width;
            size_t copy_bytes =
                (pixel_bytes < sizeof(*data_array)) ? pixel_bytes : sizeof(*data_array);
            memset(data_array, 0, (size_t)frame_size_px * sizeof(*data_array));
            char* dst = (char*)data_array;
            char* src = (char*)buffer;
            size_t dst_stride = sizeof(*data_array);
            for (int i = 0; i < frame_size_px; ++i) {
                memcpy(dst + ((size_t)i * dst_stride), src + ((size_t)i * pixel_bytes), copy_bytes);
            }
        }
        goto done;
    }

    /* Masking/Conversion Logic */
    if (fast_path && buffer == data_array && mask_buffer == NULL) {
        /* nothing further to do */
    } else if (buffer != data_array) {
        if (convert_to_int_and_mask(buffer, data_desc->data_width, data_array, frame_size_px,
                                    mask_buffer) < 0) {
            char message[64];
            sprintf(message, "Error converting data for frame %d", *frame_number);
            ERROR_JUMP(-2, done, message);
        }
    } else {
        APPLY_MASK(data_array, mask_buffer, frame_size_px);
    }

done:
    *error_flag = retval;
    if (retval < 0) {
        dump_error_stack(ERROR_OUTPUT);
    }
}

void plugin_close(int* error_flag) {
    pthread_mutex_lock(&plugin_mutex);
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
}

#ifdef __cplusplus
}
#endif
