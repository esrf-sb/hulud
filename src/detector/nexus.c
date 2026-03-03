#include "core/internal.h"
#include "core/err.h"
#include <stdlib.h>
#include <string.h>

int get_nxs_dataset_dims(struct ds_desc_t* desc) {
    hid_t ds_id = 0, s_id = 0, t_id = 0;
    int retval = 0;
    int ndims = 0;

    ds_id = H5Dopen2(desc->data_g_id, "data", H5P_DEFAULT);
    if (ds_id <= 0) {
        ERROR_JUMP(-1, done, "Unable to open 'data' dataset");
    }
    DEBUGF("nxs_dataset_dims: opened 'data'\n");

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
    DEBUGF("nxs_dataset_dims: dims [%llu %llu %llu], width %d\n", (unsigned long long)desc->dims[0],
           (unsigned long long)desc->dims[1], (unsigned long long)desc->dims[2], desc->data_width);

close_space:
    H5Sclose(s_id);
close_type:
    H5Tclose(t_id);
close_dataset:
    H5Dclose(ds_id);
done:
    return retval;
}

int get_nxs_frame(const struct ds_desc_t* desc, const int n, void* buffer) {
    int retval = 0;
    hsize_t frame_idx[3] = {n, 0, 0};
    hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};

    if (n < 0 || (hsize_t)n >= desc->dims[0]) {
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

int read_pixel_info(hid_t g_id, const char* path, double* size) {
    int retval = 0;
    herr_t err = 0;
    hid_t ds_id = 0;
    double value = 0;

    ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) return -1;

    err = H5Dread(ds_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value);
    if (err < 0) {
        H5Dclose(ds_id);
        return -1;
    }

    if (H5Aexists(ds_id, "units") > 0) {
        hid_t a_id, f_type, m_type;
        size_t s_size;
        char* str_buffer = NULL;
        a_id = H5Aopen(ds_id, "units", H5P_DEFAULT);
        f_type = H5Aget_type(a_id);
        s_size = H5Tget_size(f_type);
        H5Tclose(f_type);
        if (s_size > 0) {
            str_buffer = (char*)malloc(s_size + 1);
            if (str_buffer) {
                m_type = H5Tcopy(H5T_C_S1);
                H5Tset_size(m_type, s_size);
                H5Tset_strpad(m_type, H5T_STR_NULLPAD);
                err = H5Aread(a_id, m_type, str_buffer);
                if (err >= 0) {
                    str_buffer[s_size] = '\0';
                    double scale = scale_from_units(str_buffer);
                    value *= scale;
                }
                H5Tclose(m_type);
                free(str_buffer);
            }
        }
        H5Aclose(a_id);
    }
    H5Dclose(ds_id);
    *size = value;
    return retval;
}

int get_nxs_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_pixel_mask_impl(const struct ds_desc_t* desc, hid_t g_id, const char* path, int* buffer) {
    int retval = 0;
    size_t expected_rows = (size_t)desc->dims[1];
    size_t expected_cols = (size_t)desc->dims[2];
    size_t expected_elems = expected_rows * expected_cols;

    hid_t ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) ERROR_JUMP(-1, done, "Error opening pixel_mask dataset");

    hid_t s_id = H5Dget_space(ds_id);
    if (s_id < 0) ERROR_JUMP(-1, close_dataset, "Error getting pixel_mask dataspace");

    int rank = H5Sget_simple_extent_ndims(s_id);
    hsize_t dims[3] = {0, 0, 0};
    H5Sget_simple_extent_dims(s_id, dims, NULL);

    hsize_t start_d[3] = {0}, count_d[3] = {1, 0, 0};
    hsize_t start_m[3] = {0}, count_m[3] = {1, 0, 0};
    hid_t mem_space = H5S_ALL;

    if (rank == 2) {
        hsize_t read_rows = (dims[0] < expected_rows) ? dims[0] : (hsize_t)expected_rows;
        hsize_t read_cols = (dims[1] < expected_cols) ? dims[1] : (hsize_t)expected_cols;
        H5Sselect_hyperslab(s_id, H5S_SELECT_SET, start_d, NULL, (hsize_t[]){read_rows, read_cols}, NULL);
        hsize_t mem_dims[2] = {(hsize_t)expected_rows, (hsize_t)expected_cols};
        mem_space = H5Screate_simple(2, mem_dims, NULL);
        H5Sselect_hyperslab(mem_space, H5S_SELECT_SET, start_m, NULL, (hsize_t[]){read_rows, read_cols}, NULL);
    } else if (rank == 3) {
        if (dims[0] == 0) ERROR_JUMP(-1, close_space, "pixel_mask has zero frames");
        hsize_t read_rows = (dims[1] < expected_rows) ? dims[1] : (hsize_t)expected_rows;
        hsize_t read_cols = (dims[2] < expected_cols) ? dims[2] : (hsize_t)expected_cols;
        count_d[1] = read_rows; count_d[2] = read_cols;
        H5Sselect_hyperslab(s_id, H5S_SELECT_SET, start_d, NULL, count_d, NULL);
        hsize_t mem_dims[3] = {1, (hsize_t)expected_rows, (hsize_t)expected_cols};
        mem_space = H5Screate_simple(3, mem_dims, NULL);
        count_m[1] = read_rows; count_m[2] = read_cols;
        H5Sselect_hyperslab(mem_space, H5S_SELECT_SET, start_m, NULL, count_m, NULL);
    } else {
        ERROR_JUMP(-1, close_space, "Unsupported pixel_mask rank");
    }

    memset(buffer, 0, expected_elems * sizeof(int));
    if (H5Dread(ds_id, H5T_NATIVE_INT, mem_space, s_id, H5P_DEFAULT, buffer) < 0) {
        ERROR_JUMP(-1, close_memspace, "Error reading pixel_mask dataset");
    }

close_memspace:
    if (mem_space != H5S_ALL) H5Sclose(mem_space);
close_space:
    H5Sclose(s_id);
close_dataset:
    H5Dclose(ds_id);
done:
    return retval;
}

int get_nxs_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    return get_pixel_mask_impl(desc, desc->det_g_id, "pixel_mask", buffer);
}

int get_null_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    hsize_t buffer_length = desc->dims[1] * desc->dims[2];
    memset(buffer, 0, sizeof(*buffer) * buffer_length);
    return 0;
}
