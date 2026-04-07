#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <hdf5.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <durin/plugin_api.h>

#define RAW_DATASET_PATH "/entry_0000/ESRF-ID00/pilatus4_4m_lima2/data"
#define INNER_VDS_PATH RAW_DATASET_PATH

static int set_string_attribute(hid_t obj_id, const char* name, const char* value) {
    int retval = -1;
    hid_t space_id = -1;
    hid_t type_id = -1;
    hid_t attr_id = -1;
    size_t len = strlen(value);

    space_id = H5Screate(H5S_SCALAR);
    if (space_id < 0) goto done;

    type_id = H5Tcopy(H5T_C_S1);
    if (type_id < 0) goto done;
    if (H5Tset_size(type_id, len) < 0) goto done;
    if (H5Tset_strpad(type_id, H5T_STR_NULLPAD) < 0) goto done;

    attr_id = H5Acreate2(obj_id, name, type_id, space_id, H5P_DEFAULT, H5P_DEFAULT);
    if (attr_id < 0) goto done;
    if (H5Awrite(attr_id, type_id, value) < 0) goto done;

    retval = 0;

done:
    if (attr_id >= 0) H5Aclose(attr_id);
    if (type_id >= 0) H5Tclose(type_id);
    if (space_id >= 0) H5Sclose(space_id);
    return retval;
}

static int set_int64_attribute(hid_t obj_id, const char* name, long long value) {
    int retval = -1;
    hid_t space_id = -1;
    hid_t attr_id = -1;

    space_id = H5Screate(H5S_SCALAR);
    if (space_id < 0) goto done;

    attr_id = H5Acreate2(obj_id, name, H5T_STD_I64LE, space_id, H5P_DEFAULT, H5P_DEFAULT);
    if (attr_id < 0) goto done;
    if (H5Awrite(attr_id, H5T_NATIVE_LLONG, &value) < 0) goto done;

    retval = 0;

done:
    if (attr_id >= 0) H5Aclose(attr_id);
    if (space_id >= 0) H5Sclose(space_id);
    return retval;
}

static int create_group_with_nx_class(hid_t file_id, const char* path, const char* nx_class) {
    int retval = -1;
    hid_t lcpl_id = -1;
    hid_t group_id = -1;

    lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    if (lcpl_id < 0) goto done;
    if (H5Pset_create_intermediate_group(lcpl_id, 1) < 0) goto done;

    group_id = H5Gcreate2(file_id, path, lcpl_id, H5P_DEFAULT, H5P_DEFAULT);
    if (group_id < 0) goto done;
    if (nx_class && set_string_attribute(group_id, "NX_class", nx_class) < 0) goto done;

    retval = 0;

done:
    if (group_id >= 0) H5Gclose(group_id);
    if (lcpl_id >= 0) H5Pclose(lcpl_id);
    return retval;
}

static int create_scalar_double_dataset(hid_t file_id, const char* path, double value) {
    int retval = -1;
    hid_t lcpl_id = -1;
    hid_t space_id = -1;
    hid_t dset_id = -1;

    lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    if (lcpl_id < 0) goto done;
    if (H5Pset_create_intermediate_group(lcpl_id, 1) < 0) goto done;

    space_id = H5Screate(H5S_SCALAR);
    if (space_id < 0) goto done;

    dset_id = H5Dcreate2(file_id, path, H5T_IEEE_F64LE, space_id, lcpl_id, H5P_DEFAULT, H5P_DEFAULT);
    if (dset_id < 0) goto done;
    if (H5Dwrite(dset_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value) < 0) goto done;

    retval = 0;

done:
    if (dset_id >= 0) H5Dclose(dset_id);
    if (space_id >= 0) H5Sclose(space_id);
    if (lcpl_id >= 0) H5Pclose(lcpl_id);
    return retval;
}

static int create_chunked_u8_dataset(const char* filename, const uint8_t* values, size_t nframes) {
    int retval = -1;
    hid_t file_id = -1;
    hid_t lcpl_id = -1;
    hid_t dcpl_id = -1;
    hid_t space_id = -1;
    hid_t dset_id = -1;
    hsize_t dims[3] = {(hsize_t)nframes, 1, 1};
    hsize_t chunk[3] = {1, 1, 1};

    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) goto done;

    if (create_group_with_nx_class(file_id, "/entry_0000/ESRF-ID00/pilatus4_4m_lima2", NULL) < 0) {
        goto done;
    }

    lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    if (lcpl_id < 0) goto done;
    if (H5Pset_create_intermediate_group(lcpl_id, 1) < 0) goto done;

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl_id < 0) goto done;
    if (H5Pset_chunk(dcpl_id, 3, chunk) < 0) goto done;

    space_id = H5Screate_simple(3, dims, NULL);
    if (space_id < 0) goto done;

    dset_id =
        H5Dcreate2(file_id, RAW_DATASET_PATH, H5T_STD_U8LE, space_id, lcpl_id, dcpl_id, H5P_DEFAULT);
    if (dset_id < 0) goto done;
    if (H5Dwrite(dset_id, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, values) < 0) goto done;

    retval = 0;

done:
    if (dset_id >= 0) H5Dclose(dset_id);
    if (space_id >= 0) H5Sclose(space_id);
    if (dcpl_id >= 0) H5Pclose(dcpl_id);
    if (lcpl_id >= 0) H5Pclose(lcpl_id);
    if (file_id >= 0) H5Fclose(file_id);
    return retval;
}

static int add_vds_mapping(hid_t dcpl_id, const hsize_t* virt_dims, const hsize_t* virt_start,
                           const hsize_t* virt_stride, const hsize_t* virt_count,
                           const hsize_t* virt_block, const char* src_file,
                           const hsize_t* src_dims, const hsize_t* src_start,
                           const hsize_t* src_stride, const hsize_t* src_count,
                           const hsize_t* src_block) {
    int retval = -1;
    hid_t vspace_id = -1;
    hid_t sspace_id = -1;

    vspace_id = H5Screate_simple(3, virt_dims, NULL);
    if (vspace_id < 0) goto done;
    if (H5Sselect_hyperslab(vspace_id, H5S_SELECT_SET, virt_start, virt_stride, virt_count,
                            virt_block) < 0) {
        goto done;
    }

    sspace_id = H5Screate_simple(3, src_dims, NULL);
    if (sspace_id < 0) goto done;
    if (H5Sselect_hyperslab(sspace_id, H5S_SELECT_SET, src_start, src_stride, src_count, src_block) <
        0) {
        goto done;
    }

    if (H5Pset_virtual(dcpl_id, vspace_id, src_file, INNER_VDS_PATH, sspace_id) < 0) goto done;

    retval = 0;

done:
    if (sspace_id >= 0) H5Sclose(sspace_id);
    if (vspace_id >= 0) H5Sclose(vspace_id);
    return retval;
}

static int create_inner_frame_master(const char* filename) {
    int retval = -1;
    hid_t file_id = -1;
    hid_t lcpl_id = -1;
    hid_t dcpl_id = -1;
    hid_t space_id = -1;
    hid_t dset_id = -1;
    hsize_t virt_dims[3] = {4, 1, 1};
    hsize_t raw_dims[3] = {2, 1, 1};

    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) goto done;

    if (create_group_with_nx_class(file_id, "/entry_0000/ESRF-ID00/pilatus4_4m_lima2", NULL) < 0) {
        goto done;
    }

    lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    if (lcpl_id < 0) goto done;
    if (H5Pset_create_intermediate_group(lcpl_id, 1) < 0) goto done;

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl_id < 0) goto done;
    if (H5Pset_layout(dcpl_id, H5D_VIRTUAL) < 0) goto done;

    {
        hsize_t virt_start[3] = {1, 0, 0};
        hsize_t virt_stride[3] = {2, 1, 1};
        hsize_t virt_count[3] = {2, 1, 1};
        hsize_t virt_block[3] = {1, 1, 1};
        hsize_t src_start[3] = {0, 0, 0};
        hsize_t src_stride[3] = {1, 1, 1};
        hsize_t src_count[3] = {1, 1, 1};
        hsize_t src_block[3] = {2, 1, 1};
        if (add_vds_mapping(dcpl_id, virt_dims, virt_start, virt_stride, virt_count, virt_block,
                            "frame_0.h5", raw_dims, src_start, src_stride, src_count, src_block) <
            0) {
            goto done;
        }
    }

    {
        hsize_t virt_start[3] = {0, 0, 0};
        hsize_t virt_stride[3] = {2, 1, 1};
        hsize_t virt_count[3] = {2, 1, 1};
        hsize_t virt_block[3] = {1, 1, 1};
        hsize_t src_start[3] = {0, 0, 0};
        hsize_t src_stride[3] = {1, 1, 1};
        hsize_t src_count[3] = {1, 1, 1};
        hsize_t src_block[3] = {2, 1, 1};
        if (add_vds_mapping(dcpl_id, virt_dims, virt_start, virt_stride, virt_count, virt_block,
                            "frame_1.h5", raw_dims, src_start, src_stride, src_count, src_block) <
            0) {
            goto done;
        }
    }

    space_id = H5Screate_simple(3, virt_dims, NULL);
    if (space_id < 0) goto done;

    dset_id =
        H5Dcreate2(file_id, INNER_VDS_PATH, H5T_STD_U8LE, space_id, lcpl_id, dcpl_id, H5P_DEFAULT);
    if (dset_id < 0) goto done;

    retval = 0;

done:
    if (dset_id >= 0) H5Dclose(dset_id);
    if (space_id >= 0) H5Sclose(space_id);
    if (dcpl_id >= 0) H5Pclose(dcpl_id);
    if (lcpl_id >= 0) H5Pclose(lcpl_id);
    if (file_id >= 0) H5Fclose(file_id);
    return retval;
}

static int create_outer_block_dataset(hid_t file_id, const char* path, hsize_t src_start_frame,
                                      long long image_nr_low, long long image_nr_high) {
    int retval = -1;
    hid_t lcpl_id = -1;
    hid_t dcpl_id = -1;
    hid_t space_id = -1;
    hid_t dset_id = -1;
    hid_t vspace_id = -1;
    hid_t sspace_id = -1;
    hsize_t dims[3] = {2, 1, 1};
    hsize_t inner_dims[3] = {4, 1, 1};
    hsize_t virt_start[3] = {0, 0, 0};
    hsize_t virt_stride[3] = {1, 1, 1};
    hsize_t virt_count[3] = {1, 1, 1};
    hsize_t virt_block[3] = {2, 1, 1};
    hsize_t src_start[3] = {src_start_frame, 0, 0};
    hsize_t src_stride[3] = {1, 1, 1};
    hsize_t src_count[3] = {1, 1, 1};
    hsize_t src_block[3] = {2, 1, 1};

    lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    if (lcpl_id < 0) goto done;
    if (H5Pset_create_intermediate_group(lcpl_id, 1) < 0) goto done;

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl_id < 0) goto done;
    if (H5Pset_layout(dcpl_id, H5D_VIRTUAL) < 0) goto done;

    vspace_id = H5Screate_simple(3, dims, NULL);
    if (vspace_id < 0) goto done;
    if (H5Sselect_hyperslab(vspace_id, H5S_SELECT_SET, virt_start, virt_stride, virt_count,
                            virt_block) < 0) {
        goto done;
    }

    sspace_id = H5Screate_simple(3, inner_dims, NULL);
    if (sspace_id < 0) goto done;
    if (H5Sselect_hyperslab(sspace_id, H5S_SELECT_SET, src_start, src_stride, src_count, src_block) <
        0) {
        goto done;
    }
    if (H5Pset_virtual(dcpl_id, vspace_id, "frame_master.h5", INNER_VDS_PATH, sspace_id) < 0) {
        goto done;
    }

    space_id = H5Screate_simple(3, dims, NULL);
    if (space_id < 0) goto done;

    dset_id = H5Dcreate2(file_id, path, H5T_STD_U8LE, space_id, lcpl_id, dcpl_id, H5P_DEFAULT);
    if (dset_id < 0) goto done;
    if (set_int64_attribute(dset_id, "image_nr_low", image_nr_low) < 0) goto done;
    if (set_int64_attribute(dset_id, "image_nr_high", image_nr_high) < 0) goto done;

    retval = 0;

done:
    if (sspace_id >= 0) H5Sclose(sspace_id);
    if (vspace_id >= 0) H5Sclose(vspace_id);
    if (dset_id >= 0) H5Dclose(dset_id);
    if (space_id >= 0) H5Sclose(space_id);
    if (dcpl_id >= 0) H5Pclose(dcpl_id);
    if (lcpl_id >= 0) H5Pclose(lcpl_id);
    return retval;
}

static int create_outer_master(const char* filename) {
    int retval = -1;
    hid_t file_id = -1;

    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) goto done;

    if (create_group_with_nx_class(file_id, "/entry", "NXentry") < 0) goto done;
    if (create_group_with_nx_class(file_id, "/entry/data", "NXdata") < 0) goto done;
    if (create_group_with_nx_class(file_id, "/entry/instrument", "NXinstrument") < 0) goto done;
    if (create_group_with_nx_class(file_id, "/entry/instrument/detector", "NXdetector") < 0) {
        goto done;
    }

    if (create_scalar_double_dataset(file_id, "/entry/instrument/detector/x_pixel_size", 1.0) < 0) {
        goto done;
    }
    if (create_scalar_double_dataset(file_id, "/entry/instrument/detector/y_pixel_size", 1.0) < 0) {
        goto done;
    }

    if (create_outer_block_dataset(file_id, "/entry/data/data_000001", 0, 1, 2) < 0) goto done;
    if (create_outer_block_dataset(file_id, "/entry/data/data_000002", 2, 10, 11) < 0) goto done;

    retval = 0;

done:
    if (file_id >= 0) H5Fclose(file_id);
    return retval;
}

static int expect_frame_value(int* info, int nx, int ny, int frame_number, int expected_value) {
    int err = 0;
    int frame = frame_number;
    int buffer[1] = {0};

    plugin_get_data(&frame, &nx, &ny, buffer, info, &err);
    if (err < 0) {
        fprintf(stderr, "plugin_get_data failed for frame %d\n", frame_number);
        return -1;
    }
    if (buffer[0] != expected_value) {
        fprintf(stderr, "frame %d: expected %d, got %d\n", frame_number, expected_value, buffer[0]);
        return -1;
    }
    return 0;
}

static void cleanup_tmp_files(const char* dir_path) {
    char path[4096];

    snprintf(path, sizeof(path), "%s/frame_0.h5", dir_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/frame_1.h5", dir_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/frame_master.h5", dir_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s/master.h5", dir_path);
    unlink(path);
    rmdir(dir_path);
}

int main(void) {
    int retval = 1;
    int err = 0;
    int opened = 0;
    int info[DURIN_PLUGIN_INFO_SIZE] = {0};
    int nx = 0, ny = 0, nbytes = 0, nframes = 0;
    float qx = 0.0f, qy = 0.0f;
    char tmp_dir[] = "/tmp/durin-vds-map-XXXXXX";
    char frame0_path[4096];
    char frame1_path[4096];
    char frame_master_path[4096];
    char master_path[4096];
    uint8_t frame0_values[2] = {20, 21};
    uint8_t frame1_values[2] = {10, 11};

    if (!mkdtemp(tmp_dir)) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        goto done;
    }

    snprintf(frame0_path, sizeof(frame0_path), "%s/frame_0.h5", tmp_dir);
    snprintf(frame1_path, sizeof(frame1_path), "%s/frame_1.h5", tmp_dir);
    snprintf(frame_master_path, sizeof(frame_master_path), "%s/frame_master.h5", tmp_dir);
    snprintf(master_path, sizeof(master_path), "%s/master.h5", tmp_dir);

    if (create_chunked_u8_dataset(frame0_path, frame0_values, 2) < 0) goto done;
    if (create_chunked_u8_dataset(frame1_path, frame1_values, 2) < 0) goto done;
    if (create_inner_frame_master(frame_master_path) < 0) goto done;
    if (create_outer_master(master_path) < 0) goto done;

    plugin_open(master_path, info, &err);
    if (err < 0) {
        fprintf(stderr, "plugin_open failed\n");
        goto done;
    }
    opened = 1;

    plugin_get_header(&nx, &ny, &nbytes, &qx, &qy, &nframes, info, &err);
    if (err < 0) {
        fprintf(stderr, "plugin_get_header failed\n");
        goto done;
    }

    if (nx != 1 || ny != 1 || nbytes != 1) {
        fprintf(stderr, "unexpected geometry: nx=%d ny=%d nbytes=%d\n", nx, ny, nbytes);
        goto done;
    }
    if (nframes != 11) {
        fprintf(stderr, "unexpected logical frame count: %d\n", nframes);
        goto done;
    }
    if (info[DURIN_PLUGIN_INFO_PHYSICAL_FRAMES] != 4 ||
        info[DURIN_PLUGIN_INFO_LOGICAL_FRAMES] != 11) {
        fprintf(stderr, "unexpected info counts: physical=%d logical=%d\n",
                info[DURIN_PLUGIN_INFO_PHYSICAL_FRAMES], info[DURIN_PLUGIN_INFO_LOGICAL_FRAMES]);
        goto done;
    }

    if (expect_frame_value(info, nx, ny, 1, 10) < 0) goto done;
    if (expect_frame_value(info, nx, ny, 2, 20) < 0) goto done;
    if (expect_frame_value(info, nx, ny, 3, 0) < 0) goto done;
    if (expect_frame_value(info, nx, ny, 9, 0) < 0) goto done;
    if (expect_frame_value(info, nx, ny, 10, 11) < 0) goto done;
    if (expect_frame_value(info, nx, ny, 11, 21) < 0) goto done;

    retval = 0;

done:
    if (opened) {
        int close_err = 0;
        plugin_close(&close_err);
        if (retval == 0 && close_err < 0) retval = 1;
    }
    cleanup_tmp_files(tmp_dir);
    return retval;
}
