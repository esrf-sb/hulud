/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 * Refactored for clarity and safety.
 */

/* Enable POSIX features (realpath, etc.) */
#define _POSIX_C_SOURCE 200809L

#include "file.h"

#include <ctype.h>
#include <hdf5.h>
#include <hdf5_hl.h>
#include <limits.h> /* Required for PATH_MAX */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "err.h"
#include "filters.h"
#include "readers.h"
#include "vds_map.h"

/* --- GPFS/LIMA2 VDS OPTIMIZATION ---
 * Static cache to keep the Virtual Dataset (VDS) handle open.
 * In Lima2, "data_00001" is a VDS pointing to multiple raw files.
 * Opening this VDS is expensive on GPFS because HDF5 must stat() all
 * underlying raw files. We cache the handle to pay this cost once per block.
 */
static hid_t CACHED_DID = 0;                     // Stores the open HDF5 Dataset ID
static char CACHED_DS_NAME[MAX_NAME_LEN] = {0};  // Stores the name (e.g., "data_000001")

/* --- HELPER FUNCTIONS --- */

void reset_dataset_cache(void) {
    if (CACHED_DID > 0) {
        H5Dclose(CACHED_DID);
        CACHED_DID = 0;
    }
    memset(CACHED_DS_NAME, 0, sizeof(CACHED_DS_NAME));
}

double scale_from_units(const char* unit_string) {
    if (!unit_string) return 1.0;

    if (strcasecmp("m", unit_string) == 0 || strcasecmp("metres", unit_string) == 0 ||
        strcasecmp("meters", unit_string) == 0) {
        return 1.0;
    } else if (strcasecmp("cm", unit_string) == 0 || strcasecmp("centimetres", unit_string) == 0 ||
               strcasecmp("centimeters", unit_string) == 0) {
        return 0.01;
    } else if (strcasecmp("mm", unit_string) == 0 || strcasecmp("millimetres", unit_string) == 0 ||
               strcasecmp("millimeters", unit_string) == 0) {
        return 0.001;
    } else if (strcasecmp("um", unit_string) == 0 || strcasecmp("microns", unit_string) == 0 ||
               strcasecmp("micrometres", unit_string) == 0 ||
               strcasecmp("micrometers", unit_string) == 0) {
        return 0.000001;
    } else {
        fprintf(stderr, "Unrecognised unit string %s\n", unit_string);
        return 1.0;
    }
}

/* --- DESTRUCTORS --- */

void clear_det_visit_objects(struct det_visit_objects_t* objects) {
    if (objects->nxdata) {
        H5Oclose(objects->nxdata);
        objects->nxdata = 0;
    }
    if (objects->nxdetector) {
        H5Oclose(objects->nxdetector);
        objects->nxdetector = 0;
    }
}

void free_ds_desc(struct ds_desc_t* desc) {
    if (!desc) return;
    if (desc->det_g_id > 0) H5Gclose(desc->det_g_id);
    if (desc->data_g_id > 0) H5Gclose(desc->data_g_id);
    free(desc);
}

void free_nxs_desc(struct ds_desc_t* desc) { free_ds_desc(desc); }

void free_eiger_desc(struct ds_desc_t* desc) {
    struct eiger_ds_desc_t* e_desc = (struct eiger_ds_desc_t*)desc;
    if (e_desc) {
        free(e_desc->block_sizes);
    }
    free_ds_desc(desc);
}

void free_opt_eiger_desc(struct ds_desc_t* desc) {
    struct opt_eiger_ds_desc_t* opt_desc = (struct opt_eiger_ds_desc_t*)desc;
    if (!opt_desc) return;

    reset_dataset_cache();

    /* Cleanup VDS mapping resources */
    if (opt_desc->frame_map_entries) {
        for (int i = 0; i < opt_desc->n_map_entries; ++i) {
            struct FrameMapEntry* E = &opt_desc->frame_map_entries[i];
            if (E->src_dset_id > 0) H5Dclose(E->src_dset_id);
            if (E->src_file_id > 0) H5Fclose(E->src_file_id);
            free(E->src_file);
            free(E->src_dset);
            free(E->scratch);
        }
        free(opt_desc->frame_map_entries);
    }

    if (opt_desc->base.block_sizes) free(opt_desc->base.block_sizes);
    free_ds_desc(desc);
}

/* --- DATA ACCESS --- */

int get_nxs_dataset_dims(struct ds_desc_t* desc) {
    hid_t ds_id = 0, s_id = 0, t_id = 0;
    int retval = 0;
    int ndims = 0;

    ds_id = H5Dopen2(desc->data_g_id, "data", H5P_DEFAULT);
    if (ds_id <= 0) {
        ERROR_JUMP(-1, done, "Unable to open 'data' dataset");
    }

    t_id = H5Dget_type(ds_id);
    if (t_id <= 0) {
        ERROR_JUMP(-1, close_dataset, "Error getting datatype");
    }

    size_t width = H5Tget_size(t_id);
    if (width <= 0) {
        ERROR_JUMP(-1, close_type, "Error getting type size");
    }

    s_id = H5Dget_space(ds_id);
    if (s_id <= 0) {
        ERROR_JUMP(-1, close_dataset, "Error getting dataspace");
    }

    ndims = H5Sget_simple_extent_ndims(s_id);
    if (ndims != 3) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Dataset rank is %d, expected 3", ndims);
        ERROR_JUMP(-1, close_space, message);
    }

    if (H5Sget_simple_extent_dims(s_id, desc->dims, NULL) < 0) {
        ERROR_JUMP(-1, close_space, "Error getting dataset dimensions");
    }

    desc->data_width = (int)width;

close_space:
    H5Sclose(s_id);
close_type:
    H5Tclose(t_id);
close_dataset:
    H5Dclose(ds_id);
done:
    return retval;
}

int get_frame_simple(const struct ds_desc_t* desc, const char* name, const hsize_t* frame_idx,
                     const hsize_t* frame_size, void* buffer) {
    int retval = 0;
    herr_t err = 0;
    hid_t ds_id = 0, s_id = 0, ms_id = 0, t_id = 0;

    ds_id = H5Dopen2(desc->data_g_id, name, H5P_DEFAULT);
    if (ds_id <= 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Unable to open dataset %.32s", name);
        ERROR_JUMP(-1, done, message);
    }
    s_id = H5Dget_space(ds_id);
    if (s_id <= 0) {
        ERROR_JUMP(-1, close_dataset, "Error getting dataspace");
    }
    t_id = H5Dget_type(ds_id);
    if (t_id <= 0) {
        ERROR_JUMP(-1, close_type, "Error retrieving datatype");
    }

    err = H5Sselect_hyperslab(s_id, H5S_SELECT_SET, frame_idx, NULL, frame_size, NULL);
    if (err < 0) {
        ERROR_JUMP(-1, close_space, "Error selecting hyperslab");
    }

    ms_id = H5Screate_simple(3, frame_size, frame_size);
    if (ms_id < 0) {
        ERROR_JUMP(-1, close_space, "Could not create dataspace");
    }

    err = H5Dread(ds_id, t_id, ms_id, s_id, H5P_DEFAULT, buffer);
    if (err < 0) {
        ERROR_JUMP(-1, close_mspace, "Error reading dataset");
    }

close_mspace:
    H5Sclose(ms_id);
close_space:
    H5Sclose(s_id);
close_type:
    H5Tclose(t_id);
close_dataset:
    H5Dclose(ds_id);
done:
    return retval;
}

int get_frame_from_chunk(const struct ds_desc_t* desc, const char* ds_name,
                         const hsize_t* frame_idx, const hsize_t* frame_size, void* buffer) {
    hid_t d_id = 0;
    hsize_t c_offset[3] = {frame_idx[0], 0, 0};
    uint32_t c_filter_mask = 0;
    hsize_t c_bytes = 0;
    void* c_buffer = NULL;
    int retval = 0;
    (void)frame_size; /* Unused parameter */

    if (frame_idx[1] != 0 || frame_idx[2] != 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message),
                 "Require frame selection starts at [n, 0, 0], not [n, %llu, %llu]",
                 (unsigned long long)frame_idx[1], (unsigned long long)frame_idx[2]);
        ERROR_JUMP(-1, done, message);
    }

    /* --- CACHING LOGIC START --- */
    if (CACHED_DID > 0 && strcmp(ds_name, CACHED_DS_NAME) == 0) {
        d_id = CACHED_DID;
    } else {
        if (CACHED_DID > 0) {
            H5Dclose(CACHED_DID);
            CACHED_DID = 0;
        }

        d_id = H5Dopen(desc->data_g_id, ds_name, H5P_DEFAULT);
        if (d_id < 0) {
            char message[MAX_MSG_LEN];
            snprintf(message, sizeof(message), "Error opening dataset %.32s", ds_name);
            ERROR_JUMP(-1, done, message);
        }

        CACHED_DID = d_id;
        strncpy(CACHED_DS_NAME, ds_name, sizeof(CACHED_DS_NAME) - 1);
    }
    /* --- CACHING LOGIC END --- */

    if (H5Dget_chunk_storage_size(d_id, c_offset, &c_bytes) < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Error reading chunk size from %.32s for frame %llu",
                 ds_name, (unsigned long long)frame_idx[0]);
        ERROR_JUMP(-1, done, message);
    }

    if (c_bytes == 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Target chunk %llu has zero size for dataset %.32s",
                 (unsigned long long)frame_idx[0], ds_name);
        ERROR_JUMP(-1, done, message);
    }

    c_buffer = buffer;

    if (H5DOread_chunk(d_id, H5P_DEFAULT, c_offset, &c_filter_mask, c_buffer) < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message),
                 "Error reading chunk %llu from dataset %.32s - size %llu bytes",
                 (unsigned long long)frame_idx[0], ds_name, (unsigned long long)c_bytes);
        ERROR_JUMP(-1, done, message);
    }

done:
    /* Logic in original code implies c_buffer might be allocated if filters were manual,
     * but here it's just assigned to buffer. Keeping original cleanup check logic. */
    if (c_buffer && (c_buffer != buffer)) free(c_buffer);
    return retval;
}

int get_nxs_frame(const struct ds_desc_t* desc, const int n, void* buffer) {
    int retval = 0;
    hsize_t frame_idx[3] = {n, 0, 0};
    hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};

    if (n < 0 || n >= desc->dims[0]) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Selected frame %d is out of range valid range [0, %d]",
                 n, (int)desc->dims[0] - 1);
        ERROR_JUMP(-1, done, message);
    }

    retval = get_frame_simple(desc, "data", frame_idx, frame_size, buffer);
    if (retval < 0) {
        ERROR_JUMP(retval, done, "");
    }
done:
    return retval;
}

int get_dectris_eiger_frame(const struct ds_desc_t* desc, int n, void* buffer) {
    int retval = 0;
    int block = 0;
    hsize_t frame_count = 0;
    struct eiger_ds_desc_t* eiger_desc = (struct eiger_ds_desc_t*)desc;
    char data_name[MAX_NAME_LEN] = {0};
    hsize_t frame_idx[3] = {0, 0, 0};
    hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};

    if (n < 0 || n >= desc->dims[0]) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Selected frame %d is out of range valid range [0, %d]",
                 n, (int)desc->dims[0] - 1);
        ERROR_JUMP(-1, done, message);
    }

    /* Find which data block this frame resides in */
    while ((frame_count += eiger_desc->block_sizes[block]) <= n) block++;

    int idx = n - (frame_count - eiger_desc->block_sizes[block]);
    frame_idx[0] = idx;

    snprintf(data_name, sizeof(data_name), "data_%06d", block + 1);

    retval = eiger_desc->frame_func(desc, data_name, frame_idx, frame_size, buffer);
    if (retval < 0) {
        ERROR_JUMP(retval, done, "");
    }
done:
    return retval;
}

int get_dectris_eiger_dataset_dims(struct ds_desc_t* desc) {
    int retval = 0;
    int n_datas = 0;
    int n = 0;
    int data_width = 0;
    char ds_name[MAX_NAME_LEN] = {0};
    int* frame_counts = NULL;
    hsize_t dims[3] = {0};
    struct eiger_ds_desc_t* eiger_desc = (struct eiger_ds_desc_t*)desc;

    /* Count number of data_xxxxxx datasets */
    snprintf(ds_name, sizeof(ds_name), "data_%06d", n_datas + 1);
    while (H5Lexists(desc->data_g_id, ds_name, H5P_DEFAULT) > 0) {
        snprintf(ds_name, sizeof(ds_name), "data_%06d", ++n_datas + 1);
    }

    frame_counts = malloc(n_datas * sizeof(*frame_counts));
    if (!frame_counts) {
        ERROR_JUMP(-1, loop_end, "Out of memory allocating frame counts");
    }

    for (n = 0; n < n_datas; n++) {
        hid_t ds_id = 0, t_id = 0, s_id = 0;
        hsize_t block_dims[3] = {0};

        snprintf(ds_name, sizeof(ds_name), "data_%06d", n + 1);
        ds_id = H5Dopen2(desc->data_g_id, ds_name, H5P_DEFAULT);
        if (ds_id < 0) {
            char message[MAX_MSG_LEN];
            snprintf(message, sizeof(message), "Unable to open dataset %.16s", ds_name);
            ERROR_JUMP(-1, loop_end, message);
        }

        t_id = H5Dget_type(ds_id);
        if (t_id < 0) {
            ERROR_JUMP(-1, close_dataset, "Unable to get datatype");
        }

        s_id = H5Dget_space(ds_id);
        if (s_id < 0) {
            ERROR_JUMP(-1, close_type, "Unable to get dataspace");
        }

        size_t width = H5Tget_size(t_id);
        if (width <= 0) {
            ERROR_JUMP(-1, close_space, "Unable to get type size");
        }
        data_width = (int)width;

        int ndims = H5Sget_simple_extent_ndims(s_id);
        if (ndims != 3) {
            char message[MAX_MSG_LEN];
            snprintf(message, sizeof(message), "Dataset %.16s has rank %d, expected 3", ds_name,
                     ndims);
            ERROR_JUMP(-1, close_space, message);
        }

        if (H5Sget_simple_extent_dims(s_id, block_dims, NULL) < 0) {
            ERROR_JUMP(-1, close_space, "Unable to read dataset dimensions");
        }

        dims[1] = block_dims[1];
        dims[2] = block_dims[2];
        dims[0] += block_dims[0];
        frame_counts[n] = (int)block_dims[0];

    close_space:
        H5Sclose(s_id);
    close_type:
        H5Tclose(t_id);
    close_dataset:
        H5Dclose(ds_id);
    loop_end:
        if (retval < 0) break;
    }

    if (retval < 0) {
        free(frame_counts);
    } else {
        memcpy(desc->dims, dims, 3 * sizeof(*dims));
        desc->data_width = data_width;
        eiger_desc->n_data_blocks = n_datas;
        eiger_desc->block_sizes = frame_counts;
    }
    return retval;
}

/* --- PIXEL INFO & METADATA --- */

int read_pixel_info(hid_t g_id, const char* path, double* size) {
    int retval = 0;
    herr_t err = 0;
    hid_t ds_id = 0;
    double value = 0;

    ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Error opening dataset %.32s", path);
        ERROR_JUMP(-1, done, message);
    }

    err = H5Dread(ds_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value);
    if (err < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Error reading dataset %.32s", path);
        ERROR_JUMP(-1, close_dataset, message);
    }

    if (H5Aexists(ds_id, "units") > 0) {
        hid_t a_id, t_id, mt_id;
        void* str_buffer = NULL;
        size_t str_size = 0;
        int is_vlen = 0;

        a_id = H5Aopen(ds_id, "units", H5P_DEFAULT);
        if (a_id < 0) {
            char message[MAX_MSG_LEN];
            snprintf(message, sizeof(message), "Error opening units attribute for %.32s", path);
            ERROR_JUMP(-1, close_dataset, message);
        }

        t_id = H5Aget_type(a_id);
        if (t_id < 0) {
            ERROR_JUMP(-1, close_attribute, "Error getting datatype");
        }

        if (H5Tis_variable_str(t_id) > 0) {
            is_vlen = 1;
            str_buffer = malloc(sizeof(char*));
        } else {
            str_size = H5Tget_size(t_id);
            str_buffer = malloc(str_size + 1);
        }

        if (str_buffer == NULL) {
            ERROR_JUMP(-1, close_datatype, "Unable to allocate string buffer");
        }

        mt_id = H5Tcopy(H5T_C_S1);
        if (mt_id < 0) {
            ERROR_JUMP(-1, free_string, "Error creating HDF5 String datatype");
        }

        err = H5Tset_size(mt_id, is_vlen ? H5T_VARIABLE : str_size);
        if (err < 0) {
            ERROR_JUMP(-1, close_mem_datatype, "Error setting datatype size");
        }

        err = H5Aread(a_id, mt_id, str_buffer);
        if (err < 0) {
            ERROR_JUMP(-1, close_mem_datatype, "Error reading units attribute");
        }

        if (!is_vlen && str_size > 0) {
            ((char*)str_buffer)[str_size] = '\0';
        }

        char* unit_str = is_vlen ? *(char**)str_buffer : (char*)str_buffer;
        double scale = scale_from_units(unit_str);
        value *= scale;

        if (is_vlen) {
            H5Dvlen_reclaim(mt_id, H5S_ALL, H5P_DEFAULT, str_buffer);
        }

    close_mem_datatype:
        H5Tclose(mt_id);
    free_string:
        free(str_buffer);
    close_datatype:
        H5Tclose(t_id);
    close_attribute:
        H5Aclose(a_id);
    }

close_dataset:
    H5Dclose(ds_id);
done:
    if (retval == 0) *size = value;
    return retval;
}

int get_nxs_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_dectris_eiger_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_pixel_mask_impl(hid_t g_id, const char* path, int* buffer) {
    int retval = 0;
    hid_t ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) {
        ERROR_JUMP(-1, done, "Error opening pixel_mask dataset");
    }

    if (H5Dread(ds_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer) < 0) {
        ERROR_JUMP(-1, close_dataset, "Error reading pixel_mask dataset");
    }

close_dataset:
    H5Dclose(ds_id);
done:
    return retval;
}

int get_nxs_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    return get_pixel_mask_impl(desc->det_g_id, "pixel_mask", buffer);
}

int get_dectris_eiger_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    return get_pixel_mask_impl(desc->det_g_id, "detectorSpecific/pixel_mask", buffer);
}

int get_null_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    hsize_t buffer_length = desc->dims[1] * desc->dims[2];
    memset(buffer, 0, sizeof(*buffer) * buffer_length);
    return 0;
}

/* --- DISCOVERY CALLBACK --- */

herr_t det_visit_callback(hid_t root_id, const char* name, const H5O_info_t* info, void* op_data) {
    struct det_visit_objects_t* output_data = op_data;
    hid_t g_id;
    herr_t retval = 0;
    int found_via_nxclass = 0;

    if (info->type != H5O_TYPE_GROUP) return 0;

    g_id = H5Oopen(root_id, name, H5P_DEFAULT);
    if (g_id < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "H5OVisit callback: Unable to open group %.128s", name);
        ERROR_JUMP(-1, done, message);
    }

    /* Check for NX_class attribute */
    if (H5Aexists(g_id, "NX_class") > 0) {
        hid_t a_id = H5Aopen(g_id, "NX_class", H5P_DEFAULT);
        if (a_id > 0) {
            hid_t t_id = H5Aget_type(a_id);
            if (t_id >= 0 && H5Tget_class(t_id) == H5T_STRING) {
                void* buffer = NULL;
                size_t str_size = 0;
                int is_vlen = H5Tis_variable_str(t_id) > 0;

                if (is_vlen) {
                    buffer = malloc(sizeof(char*));
                } else {
                    str_size = H5Tget_size(t_id);
                    buffer = malloc(str_size + 1);
                }

                if (buffer && H5Aread(a_id, t_id, buffer) >= 0) {
                    if (!is_vlen) ((char*)buffer)[str_size] = '\0';
                    char* nxclass = is_vlen ? *(char**)buffer : (char*)buffer;

                    if (strcmp("NXdata", nxclass) == 0) {
                        found_via_nxclass = 1;
                        /* Logic for prioritizing /entry/data or groups with data_000001 */
                        char full_path[MAX_PATH_LEN] = {0};
                        ssize_t len = H5Iget_name(g_id, full_path, sizeof(full_path));
                        int is_entry_data =
                            (len >= 0 && strncmp(full_path, "/entry/data", 11) == 0);
                        int has_data = (H5Lexists(g_id, "data", H5P_DEFAULT) > 0) ||
                                       (H5Lexists(g_id, "data_000001", H5P_DEFAULT) > 0);

                        int replace = (output_data->nxdata <= 0) || is_entry_data;
                        if (!replace && !is_entry_data) {
                            /* If current held group is not /entry/data and doesn't have data, but
                             * this one does */
                            char curr_path[MAX_PATH_LEN] = {0};
                            H5Iget_name(output_data->nxdata, curr_path, sizeof(curr_path));
                            int curr_is_entry = (strncmp(curr_path, "/entry/data", 11) == 0);
                            int curr_has_data =
                                (H5Lexists(output_data->nxdata, "data", H5P_DEFAULT) > 0) ||
                                (H5Lexists(output_data->nxdata, "data_000001", H5P_DEFAULT) > 0);
                            if (!curr_is_entry && has_data && !curr_has_data) replace = 1;
                        }

                        if (replace) {
                            if (output_data->nxdata > 0) H5Gclose(output_data->nxdata);
                            output_data->nxdata = H5Gopen(root_id, name, H5P_DEFAULT);
                        }

                    } else if (strcmp("NXdetector", nxclass) == 0) {
                        found_via_nxclass = 1;
                        char full_path[MAX_PATH_LEN] = {0};
                        ssize_t len = H5Iget_name(g_id, full_path, sizeof(full_path));
                        int is_instr =
                            (len >= 0 && strncmp(full_path, "/entry/instrument/detector", 26) == 0);
                        int has_x = (H5Lexists(g_id, "x_pixel_size", H5P_DEFAULT) > 0);

                        int replace = (output_data->nxdetector <= 0);
                        if (!replace) {
                            char curr_path[MAX_PATH_LEN] = {0};
                            H5Iget_name(output_data->nxdetector, curr_path, sizeof(curr_path));
                            int curr_is_instr =
                                (strncmp(curr_path, "/entry/instrument/detector", 26) == 0);
                            int curr_has_x = (H5Lexists(output_data->nxdetector, "x_pixel_size",
                                                        H5P_DEFAULT) > 0);

                            if (is_instr && !curr_is_instr)
                                replace = 1;
                            else if ((is_instr == curr_is_instr) && has_x && !curr_has_x)
                                replace = 1;
                        }

                        if (replace) {
                            if (output_data->nxdetector > 0) H5Gclose(output_data->nxdetector);
                            output_data->nxdetector = H5Gopen(root_id, name, H5P_DEFAULT);
                        }
                    }

                    if (is_vlen) H5Dvlen_reclaim(t_id, H5S_ALL, H5P_DEFAULT, buffer);
                }
                free(buffer);
            }
            if (t_id >= 0) H5Tclose(t_id);
            H5Aclose(a_id);
        }
    }

    /* Fallback heuristic if NX_class missing/failed */
    if (!found_via_nxclass) {
        char full_path[MAX_PATH_LEN] = {0};
        ssize_t len = H5Iget_name(g_id, full_path, sizeof(full_path));
        int has_data01 = (H5Lexists(g_id, "data_000001", H5P_DEFAULT) > 0);
        int has_data = (H5Lexists(g_id, "data", H5P_DEFAULT) > 0);
        int is_entry_data = (len >= 0 && strncmp(full_path, "/entry/data", 11) == 0);

        if (has_data && (is_entry_data || output_data->nxdata <= 0)) {
            if (output_data->nxdata > 0) H5Gclose(output_data->nxdata);
            output_data->nxdata = H5Gopen(root_id, name, H5P_DEFAULT);
        }
        if (output_data->nxdetector <= 0 && has_data01) {
            output_data->nxdetector = H5Gopen(root_id, name, H5P_DEFAULT);
        }
    }

    if (H5Gclose(g_id) < 0) retval = -1;
done:
    return retval;
}

/* --- MAIN FACTORY LOGIC --- */

int create_dataset_descriptor(struct ds_desc_t** desc, struct det_visit_objects_t* visit_result) {
    int retval = 0;
    hid_t g_id, ds_id;
    int (*pxl_func)(const struct ds_desc_t*, double*, double*) = NULL;
    int (*pxl_mask_func)(const struct ds_desc_t*, int*) = NULL;
    int (*ds_prop_func)(struct ds_desc_t*) = NULL;
    void (*free_func)(struct ds_desc_t*) = NULL;
    struct ds_desc_t* output = NULL;

    /* Use NXdetector for pixel properties */
    g_id = visit_result->nxdetector;

    /* Determine where to read DATA from */
    ds_id = -1;

    /* Strategy:
       1. Check for data_000001 (Eiger style) in NXdetector
       2. Check for data (Standard) in NXdetector
       3. Check for data_000001 in NXdata
       4. Check for data in NXdata */

    if (g_id > 0 && H5Lexists(g_id, "data_000001", H5P_DEFAULT) > 0) {
        ds_id = g_id;
        ds_prop_func = &get_dectris_eiger_dataset_dims;
    } else if (g_id > 0 && H5Lexists(g_id, "data", H5P_DEFAULT) > 0) {
        ds_id = g_id;
        ds_prop_func = &get_nxs_dataset_dims;
    } else if (visit_result->nxdata &&
               H5Lexists(visit_result->nxdata, "data_000001", H5P_DEFAULT) > 0) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_dectris_eiger_dataset_dims;
    } else if (visit_result->nxdata && H5Lexists(visit_result->nxdata, "data", H5P_DEFAULT) > 0) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_nxs_dataset_dims;
    } else {
        ERROR_JUMP(-1, done, "Could not locate DATA dataset");
    }

    /* Strategy for Pixel Properties */
    if (g_id > 0 && H5Lexists(g_id, "x_pixel_size", H5P_DEFAULT) > 0) {
        pxl_func = &get_nxs_pixel_info;
    } else if (g_id > 0 && H5Lexists(g_id, "detectorSpecific", H5P_DEFAULT) > 0) {
        pxl_func = &get_dectris_eiger_pixel_info;
    } else {
        ERROR_JUMP(-1, done, "Could not locate x/y pixel_size");
    }

    /* Strategy for Pixel Mask */
    if (g_id > 0 && H5Lexists(g_id, "pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_nxs_pixel_mask;
    } else if (g_id > 0 && H5Lexists(g_id, "detectorSpecific/pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_dectris_eiger_pixel_mask;
    } else {
        pxl_mask_func = &get_null_pixel_mask;
    }

    /* --- INSTANTIATION --- */
    if (ds_prop_func == &get_dectris_eiger_dataset_dims) {
        /* Eiger / VDS handling */
        struct eiger_ds_desc_t* eiger_desc = calloc(1, sizeof(struct eiger_ds_desc_t));
        struct opt_eiger_ds_desc_t* opt = calloc(1, sizeof(struct opt_eiger_ds_desc_t));

        if (!eiger_desc || !opt) {
            free(eiger_desc);
            free(opt);
            ERROR_JUMP(-1, done, "Out of memory creating Eiger descriptors");
        }

        eiger_desc->frame_func = &get_frame_simple;
        opt->base.frame_func = &get_frame_from_chunk;

        /* Check if VDS optimization is needed */
        hid_t d = H5Dopen2(ds_id, "data_000001", H5P_DEFAULT);
        hid_t p = H5Dget_create_plist(d);
        H5D_layout_t L = H5Pget_layout(p);
        size_t mc = 0;
        if (L == H5D_VIRTUAL) H5Pget_virtual_count(p, &mc);
        H5Pclose(p);
        H5Dclose(d);

        if (L == H5D_VIRTUAL && mc > 0) {
            fprintf(stderr, "[DURIN] VDS detected: using VDS bypass optimization\n");

            char master_dir[MAX_PATH_LEN] = {0};
            hid_t fid_tmp = H5Iget_file_id(ds_id);
            if (fid_tmp >= 0) {
                char master_path[MAX_PATH_LEN] = {0};
                if (H5Fget_name(fid_tmp, master_path, sizeof(master_path)) > 0) {
                    /* FIX: Resolve Symlinks using realpath to handle autoPROC/XDS links */
                    char resolved_path[PATH_MAX] = {0};
                    if (realpath(master_path, resolved_path) != NULL) {
                        snprintf(master_dir, sizeof(master_dir), "%s", resolved_path);
                    } else {
                        snprintf(master_dir, sizeof(master_dir), "%s", master_path);
                    }

                    char* slash = strrchr(master_dir, '/');
                    if (slash)
                        *slash = '\0';
                    else
                        master_dir[0] = '\0';
                }
                H5Fclose(fid_tmp);
            }

            /* Build the optimized map */
            if (build_vds_mapping(ds_id, master_dir, opt, eiger_desc) < 0) {
                free(opt);
                /* eiger_desc is usually freed inside build_vds_mapping on success */
                ERROR_JUMP(-1, done, "Failed to build VDS mapping");
            }

            /* -----------------------------------------------------------
             * CRITICAL FIX FOR THREAD SAFETY
             * ----------------------------------------------------------- */

            /* 1. Build the parallel index (open FDs, pre-calculate offsets) */
            if (build_parallel_index(opt) < 0) {
                free(opt);
                ERROR_JUMP(-1, done, "Failed to build parallel index");
            }

            *(struct opt_eiger_ds_desc_t**)desc = opt;
            free_func = &free_opt_eiger_desc;
            output = (struct ds_desc_t*)opt;

            /* 2. Assign the THREAD-SAFE reader function */
            output->get_data_frame = &get_virtual_frame_parallel;

            /* ----------------------------------------------------------- */

        } else {
            /* Standard Eiger */
            free(opt);
            *(struct eiger_ds_desc_t**)desc = eiger_desc;
            free_func = &free_eiger_desc;
            output = (struct ds_desc_t*)eiger_desc;
            output->det_g_id = g_id;
            output->data_g_id = ds_id;
            ds_prop_func(output);
            output->get_data_frame = &get_dectris_eiger_frame;
        }
    } else {
        /* Standard Nexus */
        *desc = calloc(1, sizeof(struct nxs_ds_desc_t));
        if (!*desc) ERROR_JUMP(-1, done, "Out of memory");
        free_func = &free_nxs_desc;
        output = *desc;
        output->det_g_id = g_id;
        output->data_g_id = ds_id;
        ds_prop_func(output);
        output->get_data_frame = &get_nxs_frame;
    }

    output->det_g_id = g_id;
    output->data_g_id = ds_id;
    output->get_pixel_properties = pxl_func;
    output->get_pixel_mask = pxl_mask_func;
    output->free_desc = free_func;

done:
    return retval;
}

int get_detector_info(const hid_t fid, struct ds_desc_t** desc) {
    int retval = 0;
    struct det_visit_objects_t objects = {0};

    if (H5Ovisit(fid, H5_INDEX_NAME, H5_ITER_INC, &det_visit_callback, &objects) < 0) {
        clear_det_visit_objects(&objects);
        ERROR_JUMP(-1, done, "Error during H5Ovisit callback");
    }

    if (objects.nxdata == 0) fprintf(stderr, "WARNING: Could not locate an NXdata entry\n");
    if (objects.nxdetector == 0) fprintf(stderr, "WARNING: Could not locate an NXdetector entry\n");

    if ((retval = create_dataset_descriptor(desc, &objects)) < 0) {
        ERROR_JUMP(retval, done, "");
    };

done:
    return retval;
}