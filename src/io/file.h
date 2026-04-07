#ifndef FILE_H
#define FILE_H

#include <hdf5.h>
#include <stdint.h>

#include "compression/bitshuffle_lz4.h" /* BS_H5_N_PARAMS and filter constants */
#include "core/err.h"                   /* ERROR_JUMP and error APIs */

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 4096
#endif
#ifndef MAX_NAME_LEN
#define MAX_NAME_LEN 256
#endif
#ifndef MAX_MSG_LEN
#define MAX_MSG_LEN 512
#endif

struct ds_desc_t;
struct pilatus_ds_desc_t;
struct opt_pilatus_ds_desc_t;

extern int durin_debug_enabled;

/* Stores pre-calculated file location for a single frame to allow lock-free reading */
struct ParallelFrameIndex {
    int fd;                   // OS File Descriptor
    long long byte_offset;    // Byte offset
    size_t chunk_size;        // Compressed size in bytes
    unsigned int* bs_params;  // Pointer to compression params (borrowed from MapEntry)
    int has_bs_filter;        // bitshuffle-lz4 decompression
};

struct FrameMapEntry {
    hsize_t vds_start;
    hsize_t vds_end;
    hsize_t src_start;
    hsize_t src_end;
    char* src_file;
    char* src_dset;
    hid_t src_file_id;
    hid_t src_dset_id;
    int direct_read;
    int has_bs_filter;
    unsigned int bs_params[BS_H5_N_PARAMS];
    hsize_t cached_chunk_bytes;
    uint32_t cached_chunk_mask;
    int chunk_info_valid;
    hsize_t cached_chunk_index;
    void* scratch;
    size_t scratch_size;
    int raw_fd;
};

struct det_visit_objects_t;
struct nxs_ds_desc_t;

struct det_visit_objects_t {
    hid_t nxdata;
    hid_t nxdetector;
};

struct ds_desc_t {
    hsize_t dims[3];
    int data_width;
    int data_is_signed;
    hid_t det_g_id;
    hid_t data_g_id;

    int (*get_data_frame)(const struct ds_desc_t*, int, void*);
    int (*get_pixel_properties)(const struct ds_desc_t*, double*, double*);
    int (*get_pixel_mask)(const struct ds_desc_t*, int*);
    void (*free_desc)(struct ds_desc_t*);
};

struct pilatus_ds_desc_t {
    struct ds_desc_t base;
    int n_data_blocks;
    int* block_sizes;
    hsize_t* block_starts; /* 0-based global frame index for each block (uses image_nr_low when
                              available) */
    char** block_names;    /* dataset names (e.g. data_000123) kept for non-contiguous numbering */
    int (*frame_func)(const struct ds_desc_t*, const char*, const hsize_t*, const hsize_t*, void*);
};

struct nxs_ds_desc_t {
    struct ds_desc_t base;
};

struct opt_pilatus_ds_desc_t {
    struct pilatus_ds_desc_t base;

    int is_virtual;
    int n_map_entries;
    struct FrameMapEntry* frame_map_entries;

    struct ParallelFrameIndex* fast_index;
};

int get_virtual_frame(const struct ds_desc_t* d, const int f, void* buf);
int build_parallel_index(struct opt_pilatus_ds_desc_t* opt);
int get_virtual_frame_parallel(const struct ds_desc_t* d, const int f, void* buf);

int get_detector_info(hid_t file_id, struct ds_desc_t** desc);
void clear_det_visit_objects(struct det_visit_objects_t* objects);
int create_dataset_descriptor(struct ds_desc_t** desc, struct det_visit_objects_t* visit_result);

#endif
