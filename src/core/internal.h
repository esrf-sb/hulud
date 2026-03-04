#ifndef DURIN_INTERNAL_H
#define DURIN_INTERNAL_H

#include <hdf5.h>
#include "io/file.h"

/* Global debug flag */
extern int durin_debug_enabled;

#define DEBUGF(fmt, ...)                                    \
    do {                                                    \
        if (durin_debug_enabled) {                          \
            fprintf(stderr, "[DURIN] " fmt, ##__VA_ARGS__); \
        }                                                   \
    } while (0)

/* --- utils.c --- */
char* dup_cstr(const char* s);
void get_object_path(hid_t id, char* buf, size_t buflen);
int link_is_dataset(hid_t loc, const char* name);
char* canonical_file_path(hid_t fid, const char* base_dir);
double scale_from_units(const char* unit_string);

/* --- readers.c --- */
int get_frame_simple(const struct ds_desc_t* desc, const char* name, const hsize_t* frame_idx,
                     const hsize_t* frame_size, void* buffer);
int get_frame_from_chunk(const struct ds_desc_t* desc, const char* ds_name,
                         const hsize_t* frame_idx, const hsize_t* frame_size, void* buffer);
void reset_dataset_cache(void);

/* --- nexus.c --- */
int get_nxs_dataset_dims(struct ds_desc_t* desc);
int get_nxs_frame(const struct ds_desc_t* desc, const int n, void* buffer);
int get_nxs_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size);
int get_nxs_pixel_mask(const struct ds_desc_t* desc, int* buffer);
int get_null_pixel_mask(const struct ds_desc_t* desc, int* buffer);
int read_pixel_info(hid_t g_id, const char* path, double* size);
int get_pixel_mask_impl(const struct ds_desc_t* desc, hid_t g_id, const char* path, int* buffer);

/* --- pilatus.c --- */
int get_dectris_pilatus_dataset_dims(struct ds_desc_t* desc);
int get_dectris_pilatus_frame(const struct ds_desc_t* desc, int n, void* buffer);
int get_dectris_pilatus_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size);
int get_dectris_pilatus_pixel_mask(const struct ds_desc_t* desc, int* buffer);

/* Helper to build direct chunk map for non-VDS Pilatus */
int build_direct_chunk_map(hid_t data_group_id, struct pilatus_ds_desc_t* base,
                           struct opt_pilatus_ds_desc_t* opt);

#endif /* DURIN_INTERNAL_H */