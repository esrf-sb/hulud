/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 * Descriptor factory and lifecycle logic split from file.c.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <hdf5.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/err.h"
#include "core/internal.h"
#include "io/file.h"
#include "io/readers.h"
#include "vds/vds_map.h"

typedef int (*dataset_dims_reader_t)(struct ds_desc_t*);
typedef int (*pixel_info_reader_t)(const struct ds_desc_t*, double*, double*);
typedef int (*pixel_mask_reader_t)(const struct ds_desc_t*, int*);

struct descriptor_build_plan_t {
    hid_t detector_group_id;
    hid_t data_group_id;
    int is_pilatus;
    dataset_dims_reader_t dims_reader;
    pixel_info_reader_t pixel_info_reader;
    pixel_mask_reader_t pixel_mask_reader;
};

static void release_frame_map_entries(struct opt_pilatus_ds_desc_t* opt_desc, int close_raw_fd) {
    if (!opt_desc || !opt_desc->frame_map_entries) return;

    for (int i = 0; i < opt_desc->n_map_entries; ++i) {
        struct FrameMapEntry* entry = &opt_desc->frame_map_entries[i];
        if (entry->src_dset_id > 0) H5Dclose(entry->src_dset_id);
        if (entry->src_file_id > 0) H5Fclose(entry->src_file_id);
        if (close_raw_fd && entry->raw_fd > 0) close(entry->raw_fd);
        free(entry->src_file);
        free(entry->src_dset);
        free(entry->scratch);
    }

    free(opt_desc->frame_map_entries);
    opt_desc->frame_map_entries = NULL;
    opt_desc->n_map_entries = 0;
}

static void release_fast_path_buffers(struct opt_pilatus_ds_desc_t* opt_desc) {
    if (!opt_desc) return;
    if (opt_desc->fast_index) {
        free(opt_desc->fast_index);
        opt_desc->fast_index = NULL;
    }
}

static void free_ds_desc(struct ds_desc_t* desc) {
    if (!desc) return;
    if (desc->det_g_id > 0) H5Gclose(desc->det_g_id);
    if (desc->data_g_id > 0) H5Gclose(desc->data_g_id);
    free(desc);
}

static void free_nxs_desc(struct ds_desc_t* desc) { free_ds_desc(desc); }

static void free_pilatus_desc(struct ds_desc_t* desc) {
    struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)desc;
    if (pilatus_desc) {
        free(pilatus_desc->block_sizes);
        free(pilatus_desc->block_starts);
        if (pilatus_desc->block_names) {
            for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) {
                free(pilatus_desc->block_names[i]);
            }
            free(pilatus_desc->block_names);
        }
    }
    free_ds_desc(desc);
}

static void free_opt_pilatus_desc(struct ds_desc_t* desc) {
    struct opt_pilatus_ds_desc_t* opt_desc = (struct opt_pilatus_ds_desc_t*)desc;
    if (!opt_desc) return;

    reset_dataset_cache();
    release_fast_path_buffers(opt_desc);
    release_frame_map_entries(opt_desc, 1);

    free(opt_desc->base.block_sizes);
    free(opt_desc->base.block_starts);
    if (opt_desc->base.block_names) {
        for (int i = 0; i < opt_desc->base.n_data_blocks; ++i) {
            free(opt_desc->base.block_names[i]);
        }
        free(opt_desc->base.block_names);
    }

    free_ds_desc(desc);
}

static int select_data_source(const struct det_visit_objects_t* visit_result, hid_t* data_group_id,
                              dataset_dims_reader_t* dims_reader, int* is_pilatus) {
    hid_t detector_group_id = visit_result->nxdetector;

    int detector_has_data_000001 =
        (detector_group_id > 0) && link_is_dataset(detector_group_id, "data_000001");
    int detector_has_data = (detector_group_id > 0) && link_is_dataset(detector_group_id, "data");
    int nxdata_has_data_000001 =
        (visit_result->nxdata > 0) && link_is_dataset(visit_result->nxdata, "data_000001");
    int nxdata_has_data =
        (visit_result->nxdata > 0) && link_is_dataset(visit_result->nxdata, "data");

    if (detector_has_data_000001) {
        *data_group_id = detector_group_id;
        *dims_reader = &get_dectris_pilatus_dataset_dims;
        *is_pilatus = 1;
        DEBUGF("create_dataset_descriptor: using NXdetector/data_000001\n");
        return 0;
    }
    if (detector_has_data) {
        *data_group_id = detector_group_id;
        *dims_reader = &get_nxs_dataset_dims;
        *is_pilatus = 0;
        DEBUGF("create_dataset_descriptor: using NXdetector/data\n");
        return 0;
    }
    if (nxdata_has_data_000001) {
        *data_group_id = visit_result->nxdata;
        *dims_reader = &get_dectris_pilatus_dataset_dims;
        *is_pilatus = 1;
        DEBUGF("create_dataset_descriptor: using NXdata/data_000001\n");
        return 0;
    }
    if (nxdata_has_data) {
        *data_group_id = visit_result->nxdata;
        *dims_reader = &get_nxs_dataset_dims;
        *is_pilatus = 0;
        DEBUGF("create_dataset_descriptor: using NXdata/data\n");
        return 0;
    }
    return -1;
}

static int select_pixel_info_reader(hid_t detector_group_id, pixel_info_reader_t* reader) {
    if (detector_group_id > 0 && H5Lexists(detector_group_id, "x_pixel_size", H5P_DEFAULT) > 0) {
        *reader = &get_nxs_pixel_info;
        DEBUGF("create_dataset_descriptor: pixel info via x_pixel_size\n");
        return 0;
    }
    if (detector_group_id > 0 &&
        H5Lexists(detector_group_id, "detectorSpecific", H5P_DEFAULT) > 0) {
        *reader = &get_dectris_pilatus_pixel_info;
        DEBUGF("create_dataset_descriptor: pixel info via detectorSpecific\n");
        return 0;
    }
    return -1;
}

static void select_pixel_mask_reader(hid_t detector_group_id, pixel_mask_reader_t* reader) {
    if (detector_group_id > 0 && H5Lexists(detector_group_id, "pixel_mask", H5P_DEFAULT) > 0) {
        *reader = &get_nxs_pixel_mask;
        DEBUGF("create_dataset_descriptor: pixel mask via pixel_mask\n");
        return;
    }
    if (detector_group_id > 0 &&
        H5Lexists(detector_group_id, "detectorSpecific/pixel_mask", H5P_DEFAULT) > 0) {
        *reader = &get_dectris_pilatus_pixel_mask;
        DEBUGF("create_dataset_descriptor: pixel mask via detectorSpecific/pixel_mask\n");
        return;
    }
    *reader = &get_null_pixel_mask;
    DEBUGF("create_dataset_descriptor: no pixel mask found, using null mask\n");
}

static void resolve_master_directory(hid_t data_group_id, char* master_dir, size_t master_dir_len) {
    hid_t file_id = H5Iget_file_id(data_group_id);
    master_dir[0] = '\0';
    if (file_id < 0) return;

    char master_path[MAX_PATH_LEN] = {0};
    if (H5Fget_name(file_id, master_path, sizeof(master_path)) > 0) {
        char resolved_path[PATH_MAX] = {0};
        if (realpath(master_path, resolved_path) != NULL) {
            snprintf(master_dir, master_dir_len, "%s", resolved_path);
        } else {
            snprintf(master_dir, master_dir_len, "%s", master_path);
        }

        char* slash = strrchr(master_dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            master_dir[0] = '\0';
        }
    }

    H5Fclose(file_id);
}

static int is_virtual_pilatus_dataset(hid_t data_group_id, int* is_virtual) {
    hid_t dataset_id = H5Dopen2(data_group_id, "data_000001", H5P_DEFAULT);
    if (dataset_id < 0) return -1;

    hid_t plist_id = H5Dget_create_plist(dataset_id);
    if (plist_id < 0) {
        H5Dclose(dataset_id);
        return -1;
    }

    H5D_layout_t layout = H5Pget_layout(plist_id);
    size_t map_count = 0;
    if (layout == H5D_VIRTUAL) H5Pget_virtual_count(plist_id, &map_count);

    *is_virtual = (layout == H5D_VIRTUAL && map_count > 0);

    H5Pclose(plist_id);
    H5Dclose(dataset_id);
    return 0;
}

static int create_virtual_pilatus_descriptor(hid_t data_group_id, struct ds_desc_t** output_desc) {
    struct opt_pilatus_ds_desc_t* opt_desc = calloc(1, sizeof(struct opt_pilatus_ds_desc_t));
    if (!opt_desc) {
        free(opt_desc);
        return -1;
    }

    opt_desc->base.frame_func = &get_frame_from_chunk;
    opt_desc->base.base.data_g_id = data_group_id;

    if (get_dectris_pilatus_dataset_dims((struct ds_desc_t*)&opt_desc->base) < 0) {
        free_opt_pilatus_desc((struct ds_desc_t*)opt_desc);
        return -1;
    }

    char master_dir[MAX_PATH_LEN] = {0};
    resolve_master_directory(data_group_id, master_dir, sizeof(master_dir));

    if (build_vds_mapping(data_group_id, master_dir, opt_desc, NULL) < 0) {
        free_opt_pilatus_desc((struct ds_desc_t*)opt_desc);
        return -1;
    }

    if (build_parallel_index(opt_desc) < 0) {
        free_opt_pilatus_desc((struct ds_desc_t*)opt_desc);
        return -1;
    }

    *output_desc = (struct ds_desc_t*)opt_desc;
    (*output_desc)->get_data_frame = &get_virtual_frame_parallel;
    return 0;
}

static int create_direct_pilatus_descriptor(hid_t detector_group_id, hid_t data_group_id,
                                            dataset_dims_reader_t dims_reader,
                                            struct ds_desc_t** output_desc) {
    int fast_path_ready = 0;
    struct pilatus_ds_desc_t* pilatus_desc = calloc(1, sizeof(struct pilatus_ds_desc_t));
    struct opt_pilatus_ds_desc_t* opt_desc = calloc(1, sizeof(struct opt_pilatus_ds_desc_t));

    if (!pilatus_desc || !opt_desc) {
        free(pilatus_desc);
        free(opt_desc);
        return -1;
    }

    pilatus_desc->frame_func = &get_frame_simple;
    opt_desc->base.frame_func = &get_frame_from_chunk;

    pilatus_desc->base.det_g_id = detector_group_id;
    pilatus_desc->base.data_g_id = data_group_id;
    if (dims_reader((struct ds_desc_t*)pilatus_desc) < 0) {
        free(pilatus_desc);
        free(opt_desc);
        return -1;
    }

    if (build_direct_chunk_map(data_group_id, pilatus_desc, opt_desc) == 0) {
        opt_desc->base = *pilatus_desc;
        if (build_parallel_index(opt_desc) == 0) {
            fast_path_ready = 1;
            free(pilatus_desc);
        } else {
            release_fast_path_buffers(opt_desc);
            release_frame_map_entries(opt_desc, 0);
        }
    }

    if (fast_path_ready) {
        *output_desc = (struct ds_desc_t*)opt_desc;
        (*output_desc)->get_data_frame = &get_virtual_frame_parallel;
        return 0;
    }

    free(opt_desc);
    *output_desc = (struct ds_desc_t*)pilatus_desc;
    (*output_desc)->get_data_frame = &get_dectris_pilatus_frame;
    return 0;
}

static int create_nexus_descriptor(hid_t detector_group_id, hid_t data_group_id,
                                   dataset_dims_reader_t dims_reader,
                                   struct ds_desc_t** output_desc) {
    struct ds_desc_t* desc = calloc(1, sizeof(struct nxs_ds_desc_t));
    if (!desc) return -1;

    desc->det_g_id = detector_group_id;
    desc->data_g_id = data_group_id;
    if (dims_reader(desc) < 0) {
        free(desc);
        return -1;
    }

    desc->get_data_frame = &get_nxs_frame;
    *output_desc = desc;
    return 0;
}

static int build_descriptor_plan(const struct det_visit_objects_t* visit_result,
                                 struct descriptor_build_plan_t* plan) {
    memset(plan, 0, sizeof(*plan));
    plan->detector_group_id = visit_result->nxdetector;
    plan->data_group_id = -1;

    if (select_data_source(visit_result, &plan->data_group_id, &plan->dims_reader,
                           &plan->is_pilatus) < 0) {
        return -1;
    }
    if (select_pixel_info_reader(plan->detector_group_id, &plan->pixel_info_reader) < 0) {
        return -2;
    }

    select_pixel_mask_reader(plan->detector_group_id, &plan->pixel_mask_reader);
    return 0;
}

static int create_descriptor_from_plan(const struct descriptor_build_plan_t* plan,
                                       struct ds_desc_t** output_desc,
                                       void (**free_reader)(struct ds_desc_t*)) {
    int is_virtual = 0;

    if (!plan->is_pilatus) {
        if (create_nexus_descriptor(plan->detector_group_id, plan->data_group_id, plan->dims_reader,
                                    output_desc) < 0) {
            return -1;
        }
        *free_reader = &free_nxs_desc;
        return 0;
    }

    if (is_virtual_pilatus_dataset(plan->data_group_id, &is_virtual) < 0) {
        return -2;
    }

    if (is_virtual) {
        fprintf(stderr, "[DURIN] VDS detected: using VDS bypass optimization\n");
        if (create_virtual_pilatus_descriptor(plan->data_group_id, output_desc) < 0) {
            return -3;
        }
        *free_reader = &free_opt_pilatus_desc;
        return 0;
    }

    if (create_direct_pilatus_descriptor(plan->detector_group_id, plan->data_group_id,
                                         plan->dims_reader, output_desc) < 0) {
        return -4;
    }

    *free_reader = ((*output_desc)->get_data_frame == &get_virtual_frame_parallel)
                       ? &free_opt_pilatus_desc
                       : &free_pilatus_desc;
    return 0;
}

static void configure_descriptor_interfaces(struct ds_desc_t* desc,
                                            const struct descriptor_build_plan_t* plan,
                                            void (*free_reader)(struct ds_desc_t*)) {
    desc->det_g_id = plan->detector_group_id;
    desc->data_g_id = plan->data_group_id;
    desc->get_pixel_properties = plan->pixel_info_reader;
    desc->get_pixel_mask = plan->pixel_mask_reader;
    desc->free_desc = free_reader;
}

int create_dataset_descriptor(struct ds_desc_t** desc, struct det_visit_objects_t* visit_result) {
    int retval = 0;
    int plan_status = 0;
    int create_status = 0;
    struct descriptor_build_plan_t plan = {0};
    void (*free_reader)(struct ds_desc_t*) = NULL;
    struct ds_desc_t* output_desc = NULL;

    plan_status = build_descriptor_plan(visit_result, &plan);
    if (plan_status == -1) {
        ERROR_JUMP(-1, done, "Could not locate DATA dataset");
    }
    if (plan_status == -2) {
        ERROR_JUMP(-1, done, "Could not locate x/y pixel_size");
    }

    create_status = create_descriptor_from_plan(&plan, &output_desc, &free_reader);
    if (create_status == -1) {
        ERROR_JUMP(-1, done, "Failed to read dataset properties");
    }
    if (create_status == -2) {
        ERROR_JUMP(-1, done, "Failed to inspect Pilatus dataset layout");
    }
    if (create_status == -3) {
        ERROR_JUMP(-1, done, "Failed to build VDS mapping");
    }
    if (create_status == -4) {
        ERROR_JUMP(-1, done, "Failed to build Pilatus descriptor");
    }

    configure_descriptor_interfaces(output_desc, &plan, free_reader);
    *desc = output_desc;

done:
    return retval;
}
