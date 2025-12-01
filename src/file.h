#ifndef FILE_H
#define FILE_H

#include <hdf5.h>
#include <stdint.h>
#include "filters.h"   // for BS_H5_N_PARAMS and bitshuffle params
#include "err.h"       // for ERROR_JUMP and error APIs


/* Forward declarations */
struct ds_desc_t;
struct eiger_ds_desc_t;
struct opt_eiger_ds_desc_t;

/* Frame map entry to support VDS bypass */
struct FrameMapEntry {
    hsize_t vds_start;
    hsize_t vds_end;
    hsize_t src_start;
    hsize_t src_end;
    char   *src_file;
    char   *src_dset;
    hid_t  src_file_id;
    hid_t  src_dset_id;
    int    direct_read;
    int    has_bs_filter;
    unsigned int bs_params[BS_H5_N_PARAMS];
};


struct det_visit_objects_t;  // Needed by file.c
struct nxs_ds_desc_t;        // Used but not declared globally

/* Objects found during object traversal */
struct det_visit_objects_t {
    hid_t nxdata;
    hid_t nxdetector;
};


/* Base descriptor available everywhere */
struct ds_desc_t {
    hsize_t dims[3];
    int data_width;
    hid_t det_g_id;
    hid_t data_g_id;

    int (*get_data_frame)(const struct ds_desc_t *, int, void *);
    int (*get_pixel_properties)(const struct ds_desc_t *, double *, double *);
    int (*get_pixel_mask)(const struct ds_desc_t *, int *);
    void (*free_desc)(struct ds_desc_t *);
};


/* Used for standard Eiger & chunk-read path */
struct eiger_ds_desc_t {
    struct ds_desc_t base;
    int n_data_blocks;
    int *block_sizes;
    int (*frame_func)(const struct ds_desc_t*, const char*, const hsize_t*,
                      const hsize_t*, void*);
};

struct nxs_ds_desc_t {
    struct ds_desc_t base;
};

/* Extended Eiger descriptor with VDS bypass */
struct opt_eiger_ds_desc_t {
    struct eiger_ds_desc_t base;

    int is_virtual;
    int n_map_entries;
    struct FrameMapEntry *frame_map_entries;
};
/* Per-frame reader from readers.c */
int get_virtual_frame(const struct ds_desc_t *d, const int f, void *buf);
int get_detector_info(hid_t file_id, struct ds_desc_t **desc);
void clear_det_visit_objects(struct det_visit_objects_t *objects);
int create_dataset_descriptor(struct ds_desc_t **desc,
                              struct det_visit_objects_t *visit_result);

#endif
