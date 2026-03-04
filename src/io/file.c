/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 * Detector discovery and selection logic.
 */

#include "io/file.h"

#include <hdf5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/err.h"
#include "core/internal.h"

int durin_debug_enabled = 0;

void clear_det_visit_objects(struct det_visit_objects_t* objects) {
    if (objects->nxdata > 0) {
        H5Oclose(objects->nxdata);
        objects->nxdata = 0;
    }
    if (objects->nxdetector > 0) {
        H5Oclose(objects->nxdetector);
        objects->nxdetector = 0;
    }
}

static ssize_t get_group_path(hid_t group_id, char* buffer, size_t buffer_size) {
    memset(buffer, 0, buffer_size);
    return H5Iget_name(group_id, buffer, buffer_size);
}

static int path_is_entry_data(const char* path, ssize_t len) {
    return (len >= 0 && strncmp(path, "/entry/data", 11) == 0);
}

static int path_is_instrument_detector(const char* path, ssize_t len) {
    return (len >= 0 && strncmp(path, "/entry/instrument/detector", 26) == 0);
}

static int group_has_data_links(hid_t group_id) {
    return (H5Lexists(group_id, "data", H5P_DEFAULT) > 0) ||
           (H5Lexists(group_id, "data_000001", H5P_DEFAULT) > 0);
}

static int group_has_data_000001(hid_t group_id) {
    return H5Lexists(group_id, "data_000001", H5P_DEFAULT) > 0;
}

static int group_has_x_pixel_size(hid_t group_id) {
    return H5Lexists(group_id, "x_pixel_size", H5P_DEFAULT) > 0;
}

static int read_nx_class_name(hid_t group_id, char* nx_class, size_t nx_class_size) {
    int read_ok = 0;
    hid_t attr_id = 0;
    hid_t type_id = 0;

    nx_class[0] = '\0';
    if (H5Aexists(group_id, "NX_class") <= 0) return 0;

    attr_id = H5Aopen(group_id, "NX_class", H5P_DEFAULT);
    if (attr_id < 0) return 0;

    type_id = H5Aget_type(attr_id);
    if (type_id < 0 || H5Tget_class(type_id) != H5T_STRING) goto done;

    if (H5Tis_variable_str(type_id) > 0) {
        char* value = NULL;
        if (H5Aread(attr_id, type_id, &value) >= 0 && value) {
            snprintf(nx_class, nx_class_size, "%s", value);
            H5Dvlen_reclaim(type_id, H5S_ALL, H5P_DEFAULT, &value);
            read_ok = 1;
        }
    } else {
        size_t str_size = H5Tget_size(type_id);
        char* value = calloc(str_size + 1, 1);
        if (value && H5Aread(attr_id, type_id, value) >= 0) {
            value[str_size] = '\0';
            snprintf(nx_class, nx_class_size, "%s", value);
            read_ok = 1;
        }
        free(value);
    }

done:
    if (type_id > 0) H5Tclose(type_id);
    if (attr_id > 0) H5Aclose(attr_id);
    return read_ok;
}

static void maybe_update_nxdata(hid_t root_id, const char* name, hid_t candidate_group,
                                struct det_visit_objects_t* output_data) {
    char candidate_path[MAX_PATH_LEN] = {0};
    ssize_t candidate_len = get_group_path(candidate_group, candidate_path, sizeof(candidate_path));
    int candidate_is_entry = path_is_entry_data(candidate_path, candidate_len);
    int candidate_has_data = group_has_data_links(candidate_group);
    int replace = (output_data->nxdata <= 0) || candidate_is_entry;

    DEBUGF("visit: NXdata at %s\n", candidate_path);

    if (!replace && !candidate_is_entry && output_data->nxdata > 0) {
        char current_path[MAX_PATH_LEN] = {0};
        ssize_t current_len =
            get_group_path(output_data->nxdata, current_path, sizeof(current_path));
        int current_is_entry = path_is_entry_data(current_path, current_len);
        int current_has_data = group_has_data_links(output_data->nxdata);
        if (!current_is_entry && candidate_has_data && !current_has_data) replace = 1;
    }

    if (replace) {
        if (output_data->nxdata > 0) H5Gclose(output_data->nxdata);
        output_data->nxdata = H5Gopen(root_id, name, H5P_DEFAULT);
    }
}

static void maybe_update_nxdetector(hid_t root_id, const char* name, hid_t candidate_group,
                                    struct det_visit_objects_t* output_data) {
    char candidate_path[MAX_PATH_LEN] = {0};
    ssize_t candidate_len = get_group_path(candidate_group, candidate_path, sizeof(candidate_path));
    int candidate_is_instr = path_is_instrument_detector(candidate_path, candidate_len);
    int candidate_has_x = group_has_x_pixel_size(candidate_group);
    int replace = (output_data->nxdetector <= 0);

    DEBUGF("visit: NXdetector at %s\n", candidate_path);

    if (!replace && output_data->nxdetector > 0) {
        char current_path[MAX_PATH_LEN] = {0};
        ssize_t current_len =
            get_group_path(output_data->nxdetector, current_path, sizeof(current_path));
        int current_is_instr = path_is_instrument_detector(current_path, current_len);
        int current_has_x = group_has_x_pixel_size(output_data->nxdetector);

        if (candidate_is_instr && !current_is_instr) {
            replace = 1;
        } else if ((candidate_is_instr == current_is_instr) && candidate_has_x && !current_has_x) {
            replace = 1;
        }
    }

    if (replace) {
        if (output_data->nxdetector > 0) H5Gclose(output_data->nxdetector);
        output_data->nxdetector = H5Gopen(root_id, name, H5P_DEFAULT);
    }
}

static void apply_fallback_detector_selection(hid_t root_id, const char* name, hid_t group_id,
                                              struct det_visit_objects_t* output_data) {
    char group_path[MAX_PATH_LEN] = {0};
    ssize_t group_len = get_group_path(group_id, group_path, sizeof(group_path));
    int is_entry_data = path_is_entry_data(group_path, group_len);
    int has_data = H5Lexists(group_id, "data", H5P_DEFAULT) > 0;
    int has_data_000001 = group_has_data_000001(group_id);

    if (has_data && (is_entry_data || output_data->nxdata <= 0)) {
        if (output_data->nxdata > 0) H5Gclose(output_data->nxdata);
        output_data->nxdata = H5Gopen(root_id, name, H5P_DEFAULT);
    }
    if (output_data->nxdetector <= 0 && has_data_000001) {
        output_data->nxdetector = H5Gopen(root_id, name, H5P_DEFAULT);
    }
}

static herr_t det_visit_callback(hid_t root_id, const char* name, const H5O_info_t* info,
                                 void* op_data) {
    struct det_visit_objects_t* output_data = op_data;
    hid_t group_id = 0;
    herr_t retval = 0;
    int selected_by_nxclass = 0;
    char nx_class[MAX_NAME_LEN] = {0};

    if (info->type != H5O_TYPE_GROUP) return 0;

    group_id = H5Oopen(root_id, name, H5P_DEFAULT);
    if (group_id < 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "H5OVisit callback: Unable to open group %.128s", name);
        ERROR_JUMP(-1, done, message);
    }

    if (read_nx_class_name(group_id, nx_class, sizeof(nx_class))) {
        if (strcmp("NXdata", nx_class) == 0) {
            selected_by_nxclass = 1;
            maybe_update_nxdata(root_id, name, group_id, output_data);
        } else if (strcmp("NXdetector", nx_class) == 0) {
            selected_by_nxclass = 1;
            maybe_update_nxdetector(root_id, name, group_id, output_data);
        }
    }

    if (!selected_by_nxclass) {
        apply_fallback_detector_selection(root_id, name, group_id, output_data);
    }

    if (H5Gclose(group_id) < 0) retval = -1;

done:
    return retval;
}

int get_detector_info(const hid_t fid, struct ds_desc_t** desc) {
    int retval = 0;
    struct det_visit_objects_t objects = {0};

#if defined(H5Ovisit_vers) && (H5Ovisit_vers >= 3)
    if (H5Ovisit(fid, H5_INDEX_NAME, H5_ITER_INC, &det_visit_callback, &objects, H5O_INFO_BASIC) <
        0) {
#else
    if (H5Ovisit(fid, H5_INDEX_NAME, H5_ITER_INC, &det_visit_callback, &objects) < 0) {
#endif
        clear_det_visit_objects(&objects);
        ERROR_JUMP(-1, done, "Error during H5Ovisit callback");
    }

    if (objects.nxdata == 0) fprintf(stderr, "WARNING: Could not locate an NXdata entry\n");
    if (objects.nxdetector == 0) fprintf(stderr, "WARNING: Could not locate an NXdetector entry\n");

    if (durin_debug_enabled) {
        char nxdata_path[MAX_PATH_LEN] = {0};
        char nxdet_path[MAX_PATH_LEN] = {0};
        if (objects.nxdata > 0) get_object_path(objects.nxdata, nxdata_path, sizeof(nxdata_path));
        if (objects.nxdetector > 0)
            get_object_path(objects.nxdetector, nxdet_path, sizeof(nxdet_path));

        fprintf(stderr, "[DURIN] detector_info: NXdata=%s NXdetector=%s\n",
                nxdata_path[0] ? nxdata_path : "(none)", nxdet_path[0] ? nxdet_path : "(none)");
    }

    if ((retval = create_dataset_descriptor(desc, &objects)) < 0) {
        ERROR_JUMP(retval, done, "");
    }

done:
    return retval;
}
