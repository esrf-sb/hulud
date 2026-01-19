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
#include <unistd.h>

#include "err.h"
#include "filters.h"
#include "readers.h"
#include "vds_map.h"

int durin_debug_enabled = 0;

#define DEBUGF(fmt, ...)                                    \
    do {                                                    \
        if (durin_debug_enabled) {                          \
            fprintf(stderr, "[DURIN] " fmt, ##__VA_ARGS__); \
        }                                                   \
    } while (0)

/* Local strdup equivalent (POSIX strdup is not always available) */
static char* dup_cstr(const char* s) {
    size_t len = strlen(s) + 1;
    char* out = malloc(len);
    if (!out) return NULL;
    memcpy(out, s, len);
    return out;
}

static void get_object_path(hid_t id, char* buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    if (H5Iget_name(id, buf, buflen) < 0) buf[0] = '\0';
}

/* Return 1 if opening name as a dataset succeeds (follows external links) */
static int link_is_dataset(hid_t loc, const char* name) {
    hid_t d = -1;
    char gpath[MAX_PATH_LEN] = {0};
    get_object_path(loc, gpath, sizeof(gpath));
    H5E_BEGIN_TRY { d = H5Dopen2(loc, name, H5P_DEFAULT); }
    H5E_END_TRY;
    if (durin_debug_enabled) {
        fprintf(stderr, "[DURIN] link_is_dataset: %s/%s => %s\n", gpath[0] ? gpath : "(root)", name,
                (d >= 0 ? "dataset" : "missing"));
    }
    if (d < 0) return 0;
    H5Dclose(d);
    return 1;
}

/* Return an absolute path for an HDF5 file handle, following external links */
static char* canonical_file_path(hid_t fid, const char* base_dir) {
    char raw[MAX_PATH_LEN] = {0};
    char resolved[PATH_MAX] = {0};
    ssize_t len = H5Fget_name(fid, raw, sizeof(raw));

    /* 1) Absolute path already provided */
    if (len > 0 && raw[0] == '/') {
        if (realpath(raw, resolved)) return dup_cstr(resolved);
        DEBUGF("canonical_file_path: absolute path '%s'\n", resolved[0] ? resolved : raw);
        return dup_cstr(resolved[0] ? resolved : raw);
    }

    /* 2) Try resolving the raw string relative to cwd */
    if (len > 0 && realpath(raw, resolved)) {
        DEBUGF("canonical_file_path: cwd-resolved '%s'\n", resolved);
        return dup_cstr(resolved);
    }

    /* 3) If relative and we know the base directory of the master file, use it */
    if (len > 0 && base_dir && base_dir[0]) {
        char combined[PATH_MAX] = {0};
        size_t len_base = strnlen(base_dir, sizeof(combined) - 1);
        size_t len_raw = strnlen(raw, sizeof(combined) - 1);
        if (len_base + len_raw + 1 < sizeof(combined)) {
            char* dst = combined;
            memcpy(dst, base_dir, len_base);
            dst += len_base;
            if (len_base > 0 && base_dir[len_base - 1] != '/') {
                *dst++ = '/';
            }
            memcpy(dst, raw, len_raw);
            dst[len_raw] = '\0';
            if (realpath(combined, resolved)) {
                DEBUGF("canonical_file_path: base_dir '%s' + '%s' => '%s'\n", base_dir, raw,
                       resolved);
                return dup_cstr(resolved);
            }
        }
    }

#ifdef __linux__
    /* 4) Last resort: ask the kernel for the path of the open file descriptor */
    void* vfd_handle = NULL;
    if (H5Fget_vfd_handle(fid, H5P_DEFAULT, &vfd_handle) >= 0 && vfd_handle) {
        int fd = *(int*)vfd_handle;
        char proc_path[64] = {0};
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        if (realpath(proc_path, resolved)) {
            DEBUGF("canonical_file_path: procfd '%s'\n", resolved);
            return dup_cstr(resolved);
        }
    }
#endif

    /* Fall back to whatever we have */
    DEBUGF("canonical_file_path: fallback to raw path '%s'\n", raw);
    return dup_cstr(raw);
}

/* --- GPFS/LIMA2 VDS OPTIMIZATION ---
 * Static cache to keep the Virtual Dataset (VDS) handle open.
 * In Lima2, "data_00001" is a VDS pointing to multiple raw files.
 * Opening this VDS is expensive on GPFS because HDF5 must stat() all
 * underlying raw files. We cache the handle to pay this cost once per block.
 */
static hid_t CACHED_DID = 0;                     // Stores the open HDF5 Dataset ID
static char CACHED_DS_NAME[MAX_NAME_LEN] = {0};  // Stores the name (e.g., "data_000001")

/* --- HELPER FUNCTIONS --- */

/* Determine whether a dataset can be read via raw chunk offsets (no HDF5 filter plugins) */
static int populate_filter_flags(hid_t dset, struct FrameMapEntry* entry) {
    hid_t plist = H5Dget_create_plist(dset);
    if (plist < 0) return -1;

    H5D_layout_t layout = H5Pget_layout(plist);
    if (layout != H5D_CHUNKED) {
        entry->direct_read = 0;
        entry->has_bs_filter = 0;
        H5Pclose(plist);
        return -1;
    }

    int nf = H5Pget_nfilters(plist);
    if (nf == 0) {
        entry->direct_read = 1;
        entry->has_bs_filter = 0;
    } else {
        unsigned int flags = 0, config = 0;
        size_t n = BS_H5_N_PARAMS;
        H5Z_filter_t f = H5Pget_filter2(plist, 0, &flags, &n, entry->bs_params, 0, NULL, &config);
        if (f == BS_H5_FILTER_ID) {
            entry->direct_read = 1;
            entry->has_bs_filter = 1;
        } else {
            entry->direct_read = 0;
            entry->has_bs_filter = 0;
        }
    }
    H5Pclose(plist);
    return 0;
}

/* Build a minimal FrameMapEntry array for non-VDS, chunked Pilatus datasets */
static int build_direct_chunk_map(hid_t data_group_id, struct pilatus_ds_desc_t* base,
                                  struct opt_pilatus_ds_desc_t* opt) {
    int retval = 0;
    hsize_t frame_offset = 0;
    char master_dir[PATH_MAX] = {0};

    /* Cache the directory of the master file to resolve relative external links */
    hid_t master_fid = H5Iget_file_id(data_group_id);
    if (master_fid > 0) {
        char master_path[MAX_PATH_LEN] = {0};
        if (H5Fget_name(master_fid, master_path, sizeof(master_path)) > 0) {
            snprintf(master_dir, sizeof(master_dir), "%s", master_path);
            char* slash = strrchr(master_dir, '/');
            if (slash)
                *slash = '\0';
            else
                master_dir[0] = '\0';
            DEBUGF("direct_chunk_map: master dir '%s'\n", master_dir);
        }
        H5Fclose(master_fid);
    }

    opt->frame_map_entries = calloc(base->n_data_blocks, sizeof(struct FrameMapEntry));
    if (!opt->frame_map_entries) return -1;

    for (int block = 0; block < base->n_data_blocks; ++block) {
        char ds_name[MAX_NAME_LEN] = {0};
        if (base->block_names && base->block_names[block]) {
            snprintf(ds_name, sizeof(ds_name), "%s", base->block_names[block]);
        } else {
            snprintf(ds_name, sizeof(ds_name), "data_%06d", block + 1);
        }

        hid_t dset = H5Dopen2(data_group_id, ds_name, H5P_DEFAULT);
        if (dset < 0) {
            retval = -1;
            break;
        }

        struct FrameMapEntry* E = &opt->frame_map_entries[block];
        memset(E, 0, sizeof(*E));

        if (populate_filter_flags(dset, E) < 0 || !E->direct_read) {
            H5Dclose(dset);
            retval = -1;
            break;
        }

        /* Frame span for this dataset */
        if (!base->block_sizes || base->block_sizes[block] <= 0) {
            H5Dclose(dset);
            retval = -1;
            break;
        }
        hsize_t block_frames = (hsize_t)base->block_sizes[block];

        hsize_t start = frame_offset;
        if (base->block_starts) start = base->block_starts[block];

        E->vds_start = start;
        E->vds_end = start + block_frames - 1;
        E->src_start = 0;
        E->src_end = block_frames - 1;

        /* Dataset path within the file */
        char ds_path[MAX_NAME_LEN] = {0};
        if (H5Iget_name(dset, ds_path, sizeof(ds_path)) < 0) {
            H5Dclose(dset);
            retval = -1;
            break;
        }
        E->src_dset = dup_cstr(ds_path);

        /* Backing file path (resolved to avoid symlink surprises) */
        hid_t fid = H5Iget_file_id(dset);
        char* canonical_path = canonical_file_path(fid, master_dir);
        if (!canonical_path) {
            H5Fclose(fid);
            H5Dclose(dset);
            retval = -1;
            break;
        }
        E->src_file = canonical_path;
        DEBUGF("direct_chunk_map: %s -> file %s (block %d)\n", ds_path, E->src_file, block + 1);

        E->src_file_id = fid;  /* keep ref-counted handle for cleanup */
        E->src_dset_id = dset; /* keep open: used by build_parallel_index */

        frame_offset = E->vds_end + 1;
    }

    if (retval < 0) {
        /* Clean up anything we allocated/opened */
        if (opt->frame_map_entries) {
            for (int i = 0; i < base->n_data_blocks; ++i) {
                struct FrameMapEntry* E = &opt->frame_map_entries[i];
                if (E->src_dset_id > 0) H5Dclose(E->src_dset_id);
                if (E->src_file_id > 0) H5Fclose(E->src_file_id);
                free(E->src_file);
                free(E->src_dset);
            }
            free(opt->frame_map_entries);
            opt->frame_map_entries = NULL;
        }
    } else {
        opt->n_map_entries = base->n_data_blocks;
        opt->is_virtual = 0;
    }

    return retval;
}

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

void free_pilatus_desc(struct ds_desc_t* desc) {
    struct pilatus_ds_desc_t* e_desc = (struct pilatus_ds_desc_t*)desc;
    if (e_desc) {
        free(e_desc->block_sizes);
        free(e_desc->block_starts);
        if (e_desc->block_names) {
            for (int i = 0; i < e_desc->n_data_blocks; ++i) free(e_desc->block_names[i]);
            free(e_desc->block_names);
        }
    }
    free_ds_desc(desc);
}

void free_opt_pilatus_desc(struct ds_desc_t* desc) {
    struct opt_pilatus_ds_desc_t* opt_desc = (struct opt_pilatus_ds_desc_t*)desc;
    if (!opt_desc) return;

    reset_dataset_cache();

    if (opt_desc->fast_index) {
        free(opt_desc->fast_index);
        opt_desc->fast_index = NULL;
    }

    /* Cleanup VDS mapping resources */
    if (opt_desc->frame_map_entries) {
        for (int i = 0; i < opt_desc->n_map_entries; ++i) {
            struct FrameMapEntry* E = &opt_desc->frame_map_entries[i];
            if (E->src_dset_id > 0) H5Dclose(E->src_dset_id);
            if (E->src_file_id > 0) H5Fclose(E->src_file_id);
            if (E->raw_fd > 0) close(E->raw_fd);
            free(E->src_file);
            free(E->src_dset);
            free(E->scratch);
        }
        free(opt_desc->frame_map_entries);
    }

    if (opt_desc->base.block_sizes) free(opt_desc->base.block_sizes);
    if (opt_desc->base.block_starts) free(opt_desc->base.block_starts);
    if (opt_desc->base.block_names) {
        for (int i = 0; i < opt_desc->base.n_data_blocks; ++i) free(opt_desc->base.block_names[i]);
        free(opt_desc->base.block_names);
    }
    free_ds_desc(desc);
}

/* --- DATA ACCESS --- */

int get_nxs_dataset_dims(struct ds_desc_t* desc) {
    hid_t ds_id = 0, s_id = 0, t_id = 0;
    int retval = 0;
    int ndims = 0;

    /* Read dimensions and element width for a standard 3D Nexus dataset at "data". */
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

int get_frame_simple(const struct ds_desc_t* desc, const char* name, const hsize_t* frame_idx,
                     const hsize_t* frame_size, void* buffer) {
    int retval = 0;
    herr_t err = 0;
    hid_t ds_id = 0, s_id = 0, ms_id = 0, t_id = 0;

    ds_id = H5Dopen2(desc->data_g_id, name, H5P_DEFAULT);
    if (ds_id <= 0) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Unable to open dataset %.32s", name);
        DEBUGF("get_frame_simple: failed to open dataset '%s'\n", name);
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

int get_dectris_pilatus_frame(const struct ds_desc_t* desc, int n, void* buffer) {
    int retval = 0;
    struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)desc;
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
    int block = -1;
    hsize_t frame_offset = 0;

    if (pilatus_desc->block_starts) {
        for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) {
            hsize_t start = pilatus_desc->block_starts[i];
            hsize_t end = start + (hsize_t)pilatus_desc->block_sizes[i] - 1;
            if ((hsize_t)n >= start && (hsize_t)n <= end) {
                block = i;
                frame_offset = start;
                break;
            }
        }
    } else {
        hsize_t frame_count = 0;
        for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) {
            frame_count += (hsize_t)pilatus_desc->block_sizes[i];
            if (frame_count > (hsize_t)n) {
                block = i;
                frame_offset = frame_count - (hsize_t)pilatus_desc->block_sizes[i];
                break;
            }
        }
    }

    if (block < 0) {
        /* Frame falls into a gap (no backing dataset). Return a zero-filled frame. */
        memset(buffer, 0, (size_t)desc->data_width * (size_t)desc->dims[1] * (size_t)desc->dims[2]);
        goto done;
    }

    int idx = n - (int)frame_offset;
    frame_idx[0] = idx;

    if (pilatus_desc->block_names && pilatus_desc->block_names[block]) {
        snprintf(data_name, sizeof(data_name), "%s", pilatus_desc->block_names[block]);
    } else {
        snprintf(data_name, sizeof(data_name), "data_%06d", block + 1);
    }

    retval = pilatus_desc->frame_func(desc, data_name, frame_idx, frame_size, buffer);
    if (retval < 0) {
        ERROR_JUMP(retval, done, "");
    }
done:
    return retval;
}

/* --- Pilatus dataset discovery (handles gaps using image_nr_low/high) --- */

struct data_block_info {
    char name[MAX_NAME_LEN];
    hsize_t frames;
    hsize_t start; /* 0-based global start index */
    hsize_t end;   /* 0-based inclusive end */
    int has_range;
};

struct data_block_list {
    struct data_block_info* blocks;
    size_t used;
    size_t cap;
    hsize_t dim1;
    hsize_t dim2;
    int data_width;
    int any_range;
};

static int ensure_block_capacity(struct data_block_list* list, size_t needed) {
    if (list->cap >= needed) return 0;
    size_t newcap = list->cap ? list->cap * 2 : 8;
    if (newcap < needed) newcap = needed;
    struct data_block_info* nb = realloc(list->blocks, newcap * sizeof(*nb));
    if (!nb) return -1;
    list->blocks = nb;
    list->cap = newcap;
    return 0;
}

static int data_suffix_index(const char* name) {
    if (!name || strncmp(name, "data_", 5) != 0) return -1;
    const char* num = name + 5;
    int idx = 0;
    if (*num == '\0') return -1;
    while (*num) {
        if (!isdigit((unsigned char)*num)) return -1;
        idx = (idx * 10) + (*num - '0');
        num++;
    }
    return idx;
}

static int compare_blocks_by_start(const void* a, const void* b) {
    const struct data_block_info* A = a;
    const struct data_block_info* B = b;
    if (A->start == B->start) return strcmp(A->name, B->name);
    return (A->start < B->start) ? -1 : 1;
}

static int compare_blocks_by_index(const void* a, const void* b) {
    const struct data_block_info* A = a;
    const struct data_block_info* B = b;
    int ia = data_suffix_index(A->name);
    int ib = data_suffix_index(B->name);
    if (ia >= 0 && ib >= 0 && ia != ib) return (ia < ib) ? -1 : 1;
    return strcmp(A->name, B->name);
}

static herr_t collect_data_block_cb(hid_t g_id, const char* name, const H5L_info_t* info,
                                    void* op_data) {
    (void)info;
    struct data_block_list* ctx = op_data;
    if (!name || strncmp(name, "data_", 5) != 0) return 0;
    /* Require data_ followed by digits */
    for (const char* p = name + 5; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return 0;
    }

    hid_t ds_id = H5Dopen2(g_id, name, H5P_DEFAULT);
    if (ds_id < 0) return 0;

    hid_t s_id = H5Dget_space(ds_id);
    if (s_id < 0) {
        H5Dclose(ds_id);
        return 0;
    }
    int ndims = H5Sget_simple_extent_ndims(s_id);
    if (ndims != 3) {
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return 0;
    }
    hsize_t dims[3] = {0};
    if (H5Sget_simple_extent_dims(s_id, dims, NULL) < 0) {
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return 0;
    }

    hid_t t_id = H5Dget_type(ds_id);
    if (t_id < 0) {
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return 0;
    }
    size_t width = H5Tget_size(t_id);

    if (ctx->data_width == 0) {
        ctx->data_width = (int)width;
    } else if (ctx->data_width != (int)width) {
        H5Tclose(t_id);
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return -1; /* inconsistent type size */
    }

    if (ctx->dim1 == 0 && ctx->dim2 == 0) {
        ctx->dim1 = dims[1];
        ctx->dim2 = dims[2];
    } else if (ctx->dim1 != dims[1] || ctx->dim2 != dims[2]) {
        H5Tclose(t_id);
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return -1; /* inconsistent geometry */
    }

    hsize_t start = 0;
    hsize_t end = dims[0] ? dims[0] - 1 : 0;
    int has_range = 0;
    if (H5Aexists(ds_id, "image_nr_low") > 0 && H5Aexists(ds_id, "image_nr_high") > 0) {
        long long low = 0, high = 0;
        hid_t a_low = H5Aopen(ds_id, "image_nr_low", H5P_DEFAULT);
        hid_t a_high = H5Aopen(ds_id, "image_nr_high", H5P_DEFAULT);
        if (a_low >= 0 && a_high >= 0 && H5Aread(a_low, H5T_NATIVE_LLONG, &low) >= 0 &&
            H5Aread(a_high, H5T_NATIVE_LLONG, &high) >= 0) {
            if (low > 0) start = (hsize_t)(low - 1);
            if (high > 0) end = (hsize_t)(high - 1);
            has_range = 1;
            ctx->any_range = 1;
        }
        if (a_low >= 0) H5Aclose(a_low);
        if (a_high >= 0) H5Aclose(a_high);
    }

    H5Tclose(t_id);
    H5Sclose(s_id);
    H5Dclose(ds_id);

    if (ensure_block_capacity(ctx, ctx->used + 1) < 0) return -1;
    struct data_block_info* dst = &ctx->blocks[ctx->used++];
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->name, name, sizeof(dst->name) - 1);
    dst->frames = dims[0];
    dst->start = start;
    dst->end = has_range ? end : (start + dims[0] - 1);
    dst->has_range = has_range;

    DEBUGF("discover data_*: %s frames=%llu start=%llu end=%llu via %s\n", name,
           (unsigned long long)dst->frames, (unsigned long long)dst->start,
           (unsigned long long)dst->end, has_range ? "image_nr_low/high" : "implicit");

    return 0;
}

int get_dectris_pilatus_dataset_dims(struct ds_desc_t* desc) {
    int retval = 0;
    struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)desc;
    struct data_block_list ctx = {0};
    hsize_t total_frames = 0;

    /* Discover all data_* datasets (even if numbering is sparse) */
    if (H5Literate(desc->data_g_id, H5_INDEX_NAME, H5_ITER_NATIVE, NULL, collect_data_block_cb,
                   &ctx) < 0) {
        ERROR_JUMP(-1, done, "Error while iterating data_* datasets");
    }

    if (ctx.used == 0) {
        ERROR_JUMP(-1, done, "Unable to locate any data_* datasets");
    }

    /* Order blocks: prefer image_nr_low/high if present, else numeric suffix */
    if (ctx.any_range) {
        qsort(ctx.blocks, ctx.used, sizeof(*ctx.blocks), compare_blocks_by_start);
        /* Fill in missing starts for blocks without image_nr_low */
        hsize_t next = 0;
        for (size_t i = 0; i < ctx.used; ++i) {
            if (!ctx.blocks[i].has_range) {
                ctx.blocks[i].start = next;
                ctx.blocks[i].end = next + ctx.blocks[i].frames - 1;
            }
            next = ctx.blocks[i].end + 1;
        }
    } else {
        qsort(ctx.blocks, ctx.used, sizeof(*ctx.blocks), compare_blocks_by_index);
        hsize_t next = 0;
        for (size_t i = 0; i < ctx.used; ++i) {
            ctx.blocks[i].start = next;
            ctx.blocks[i].end = next + ctx.blocks[i].frames - 1;
            next = ctx.blocks[i].end + 1;
        }
    }

    pilatus_desc->n_data_blocks = (int)ctx.used;
    pilatus_desc->block_sizes = calloc(ctx.used, sizeof(*pilatus_desc->block_sizes));
    pilatus_desc->block_starts = calloc(ctx.used, sizeof(*pilatus_desc->block_starts));
    pilatus_desc->block_names = calloc(ctx.used, sizeof(*pilatus_desc->block_names));
    if (!pilatus_desc->block_sizes || !pilatus_desc->block_starts || !pilatus_desc->block_names) {
        ERROR_JUMP(-1, done, "Out of memory allocating block metadata");
    }

    for (size_t i = 0; i < ctx.used; ++i) {
        if (ctx.blocks[i].frames > INT_MAX) {
            ERROR_JUMP(-1, done, "Frame count too large");
        }
        pilatus_desc->block_sizes[i] = (int)ctx.blocks[i].frames;
        pilatus_desc->block_starts[i] = ctx.blocks[i].start;
        pilatus_desc->block_names[i] = dup_cstr(ctx.blocks[i].name);
        if (!pilatus_desc->block_names[i]) {
            ERROR_JUMP(-1, done, "Out of memory copying dataset name");
        }
        if (ctx.blocks[i].end + 1 > total_frames) total_frames = ctx.blocks[i].end + 1;
        DEBUGF("block map[%zu]: name=%s frames=%llu start=%llu end=%llu\n", i,
               pilatus_desc->block_names[i], (unsigned long long)ctx.blocks[i].frames,
               (unsigned long long)ctx.blocks[i].start, (unsigned long long)ctx.blocks[i].end);
    }

    desc->dims[0] = total_frames;
    desc->dims[1] = ctx.dim1;
    desc->dims[2] = ctx.dim2;
    desc->data_width = ctx.data_width;

    DEBUGF("pilatus_dataset_dims: total frames %llu, dims [%llu %llu], width %d, blocks %d (image_nr aware: %s)\n",
           (unsigned long long)desc->dims[0], (unsigned long long)desc->dims[1],
           (unsigned long long)desc->dims[2], desc->data_width, pilatus_desc->n_data_blocks,
           ctx.any_range ? "yes" : "no");

done:
    if (retval < 0) {
        free(pilatus_desc->block_sizes);
        free(pilatus_desc->block_starts);
        if (pilatus_desc->block_names) {
            for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) free(pilatus_desc->block_names[i]);
            free(pilatus_desc->block_names);
        }
        pilatus_desc->block_sizes = NULL;
        pilatus_desc->block_starts = NULL;
        pilatus_desc->block_names = NULL;
    }
    free(ctx.blocks);
    return retval;
}

int read_pixel_info(hid_t g_id, const char* path, double* size) {
    int retval = 0;
    herr_t err = 0;
    hid_t ds_id = 0;
    double value = 0;

    ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) {
        // Quiet failure is better for optional paths, or log if strict
        return -1;
    }

    err = H5Dread(ds_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value);
    if (err < 0) {
        H5Dclose(ds_id);
        return -1;
    }

    /* --- ROBUST STRING READING WITH PADDING HANDLING --- */
    if (H5Aexists(ds_id, "units") > 0) {
        hid_t a_id, f_type, m_type;
        size_t s_size;
        char* str_buffer = NULL;

        a_id = H5Aopen(ds_id, "units", H5P_DEFAULT);

        /* 1. Get the size from the file */
        f_type = H5Aget_type(a_id);
        s_size = H5Tget_size(f_type);
        H5Tclose(f_type);

        if (s_size > 0) {
            /* 2. Allocate s_size + 1 (for our manual null terminator) */
            str_buffer = (char*)malloc(s_size + 1);
            if (str_buffer) {
                /* 3. Create a C-String type of the exact size */
                m_type = H5Tcopy(H5T_C_S1);
                H5Tset_size(m_type, s_size);

                /* * CRITICAL FIX:
                 * Tell HDF5 NOT to look for/add a null terminator during the read.
                 * Just copy the raw bytes.
                 */
                H5Tset_strpad(m_type, H5T_STR_NULLPAD);

                /* 4. Read the raw characters */
                err = H5Aread(a_id, m_type, str_buffer);

                if (err >= 0) {
                    /* 5. Manually null terminate */
                    str_buffer[s_size] = '\0';

                    /* 6. Parse */
                    double scale = scale_from_units(str_buffer);
                    value *= scale;
                } else {
                    fprintf(stderr, "Error reading units attribute content.\n");
                }

                H5Tclose(m_type);
                free(str_buffer);
            }
        }
        H5Aclose(a_id);
    }
    /* ------------------------------------------------ */

    H5Dclose(ds_id);
    *size = value;
    return retval;
}

int get_nxs_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_dectris_pilatus_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_pixel_mask_impl(const struct ds_desc_t* desc, hid_t g_id, const char* path, int* buffer) {
    int retval = 0;
    size_t expected_rows = (size_t)desc->dims[1];
    size_t expected_cols = (size_t)desc->dims[2];
    size_t expected_elems = expected_rows * expected_cols;

    hid_t ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
    if (ds_id < 0) {
        ERROR_JUMP(-1, done, "Error opening pixel_mask dataset");
    }
    DEBUGF("pixel_mask: opening %s\n", path);

    hid_t s_id = H5Dget_space(ds_id);
    if (s_id < 0) {
        ERROR_JUMP(-1, close_dataset, "Error getting pixel_mask dataspace");
    }

    int rank = H5Sget_simple_extent_ndims(s_id);
    hsize_t dims[3] = {0, 0, 0};
    if (rank < 0 || rank > 3) {
        ERROR_JUMP(-1, close_space, "Unsupported pixel_mask rank");
    }
    if (H5Sget_simple_extent_dims(s_id, dims, NULL) < 0) {
        ERROR_JUMP(-1, close_space, "Error getting pixel_mask dimensions");
    }
    DEBUGF("pixel_mask: rank %d dims [%llu %llu %llu], expected [%zu %zu]\n", rank,
           (unsigned long long)dims[0], (unsigned long long)dims[1], (unsigned long long)dims[2],
           expected_rows, expected_cols);

    /* Prepare selections */
    hsize_t start_d[3] = {0, 0, 0};
    hsize_t count_d[3] = {1, 0, 0};
    hsize_t start_m[3] = {0, 0, 0};
    hsize_t count_m[3] = {1, 0, 0};
    hid_t mem_space = H5S_ALL;

    if (rank == 2) {
        /* Dataset: [rows, cols] */
        hsize_t read_rows = (dims[0] < expected_rows) ? dims[0] : (hsize_t)expected_rows;
        hsize_t read_cols = (dims[1] < expected_cols) ? dims[1] : (hsize_t)expected_cols;

        if (H5Sselect_hyperslab(s_id, H5S_SELECT_SET, start_d, NULL,
                                (hsize_t[]){read_rows, read_cols}, NULL) < 0) {
            ERROR_JUMP(-1, close_space, "Error selecting pixel_mask hyperslab");
        }

        hsize_t mem_dims[2] = {(hsize_t)expected_rows, (hsize_t)expected_cols};
        mem_space = H5Screate_simple(2, mem_dims, NULL);
        if (mem_space < 0) {
            ERROR_JUMP(-1, close_space, "Error creating pixel_mask memspace");
        }

        if (H5Sselect_hyperslab(mem_space, H5S_SELECT_SET, start_m, NULL,
                                (hsize_t[]){read_rows, read_cols}, NULL) < 0) {
            ERROR_JUMP(-1, close_memspace, "Error selecting pixel_mask mem hyperslab");
        }

    } else if (rank == 3) {
        /* Dataset: [frames, rows, cols] – take first frame */
        if (dims[0] == 0) {
            ERROR_JUMP(-1, close_space, "pixel_mask has zero frames");
        }

        hsize_t read_rows = (dims[1] < expected_rows) ? dims[1] : (hsize_t)expected_rows;
        hsize_t read_cols = (dims[2] < expected_cols) ? dims[2] : (hsize_t)expected_cols;

        count_d[1] = read_rows;
        count_d[2] = read_cols;
        if (H5Sselect_hyperslab(s_id, H5S_SELECT_SET, start_d, NULL, count_d, NULL) < 0) {
            ERROR_JUMP(-1, close_space, "Error selecting pixel_mask frame hyperslab");
        }

        hsize_t mem_dims[3] = {1, (hsize_t)expected_rows, (hsize_t)expected_cols};
        mem_space = H5Screate_simple(3, mem_dims, NULL);
        if (mem_space < 0) {
            ERROR_JUMP(-1, close_space, "Error creating pixel_mask memspace");
        }

        count_m[1] = read_rows;
        count_m[2] = read_cols;
        if (H5Sselect_hyperslab(mem_space, H5S_SELECT_SET, start_m, NULL, count_m, NULL) < 0) {
            ERROR_JUMP(-1, close_memspace, "Error selecting pixel_mask mem hyperslab");
        }
    } else {
        ERROR_JUMP(-1, close_space, "Unsupported pixel_mask rank");
    }

    /* Zero-fill full frame, then read subset */
    memset(buffer, 0, expected_elems * sizeof(int));

    if (H5Dread(ds_id, H5T_NATIVE_INT, mem_space, s_id, H5P_DEFAULT, buffer) < 0) {
        ERROR_JUMP(-1, close_memspace, "Error reading pixel_mask dataset");
    }
    DEBUGF("pixel_mask: read %s successfully\n", path);

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

int get_dectris_pilatus_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    return get_pixel_mask_impl(desc, desc->det_g_id, "detectorSpecific/pixel_mask", buffer);
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
                        DEBUGF("visit: NXdata at %s\n", full_path);
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
                        DEBUGF("visit: NXdetector at %s\n", full_path);
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

    /* Build the appropriate descriptor (Pilatus VDS/direct, or standard NXS). */
    /* Use NXdetector for pixel properties */
    g_id = visit_result->nxdetector;

    /* Determine where to read DATA from */
    ds_id = -1;

    /* Strategy:
       1. Check for data_000001 (Pilatus style) in NXdetector
       2. Check for data (Standard) in NXdetector
       3. Check for data_000001 in NXdata
       4. Check for data in NXdata */

    int g_has_data000001 = (g_id > 0) && link_is_dataset(g_id, "data_000001");
    int g_has_data = (g_id > 0) && link_is_dataset(g_id, "data");
    int nx_has_data000001 =
        (visit_result->nxdata && link_is_dataset(visit_result->nxdata, "data_000001"));
    int nx_has_data = (visit_result->nxdata && link_is_dataset(visit_result->nxdata, "data"));

    if (g_has_data000001) {
        ds_id = g_id;
        ds_prop_func = &get_dectris_pilatus_dataset_dims;
        DEBUGF("create_dataset_descriptor: using NXdetector/data_000001\n");
    } else if (g_has_data) {
        ds_id = g_id;
        ds_prop_func = &get_nxs_dataset_dims;
        DEBUGF("create_dataset_descriptor: using NXdetector/data\n");
    } else if (nx_has_data000001) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_dectris_pilatus_dataset_dims;
        DEBUGF("create_dataset_descriptor: using NXdata/data_000001\n");
    } else if (nx_has_data) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_nxs_dataset_dims;
        DEBUGF("create_dataset_descriptor: using NXdata/data\n");
    } else {
        ERROR_JUMP(-1, done, "Could not locate DATA dataset");
    }

    /* Strategy for Pixel Properties */
    if (g_id > 0 && H5Lexists(g_id, "x_pixel_size", H5P_DEFAULT) > 0) {
        pxl_func = &get_nxs_pixel_info;
        DEBUGF("create_dataset_descriptor: pixel info via x_pixel_size\n");
    } else if (g_id > 0 && H5Lexists(g_id, "detectorSpecific", H5P_DEFAULT) > 0) {
        pxl_func = &get_dectris_pilatus_pixel_info;
        DEBUGF("create_dataset_descriptor: pixel info via detectorSpecific\n");
    } else {
        ERROR_JUMP(-1, done, "Could not locate x/y pixel_size");
    }

    /* Strategy for Pixel Mask */
    if (g_id > 0 && H5Lexists(g_id, "pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_nxs_pixel_mask;
        DEBUGF("create_dataset_descriptor: pixel mask via pixel_mask\n");
    } else if (g_id > 0 && H5Lexists(g_id, "detectorSpecific/pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_dectris_pilatus_pixel_mask;
        DEBUGF("create_dataset_descriptor: pixel mask via detectorSpecific/pixel_mask\n");
    } else {
        pxl_mask_func = &get_null_pixel_mask;
        DEBUGF("create_dataset_descriptor: no pixel mask found, using null mask\n");
    }

    /* --- INSTANTIATION --- */
    if (ds_prop_func == &get_dectris_pilatus_dataset_dims) {
        /* Pilatus / VDS handling */
        struct pilatus_ds_desc_t* pilatus_desc = calloc(1, sizeof(struct pilatus_ds_desc_t));
        struct opt_pilatus_ds_desc_t* opt = calloc(1, sizeof(struct opt_pilatus_ds_desc_t));

        if (!pilatus_desc || !opt) {
            free(pilatus_desc);
            free(opt);
            ERROR_JUMP(-1, done, "Out of memory creating Pilatus descriptors");
        }

        pilatus_desc->frame_func = &get_frame_simple;
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
            if (build_vds_mapping(ds_id, master_dir, opt, pilatus_desc) < 0) {
                free(opt);
                /* pilatus_desc is usually freed inside build_vds_mapping on success */
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

            *(struct opt_pilatus_ds_desc_t**)desc = opt;
            free_func = &free_opt_pilatus_desc;
            output = (struct ds_desc_t*)opt;

            /* 2. Assign the THREAD-SAFE reader function */
            output->get_data_frame = &get_virtual_frame_parallel;

            /* ----------------------------------------------------------- */

        } else {
            /* Standard Pilatus (non-VDS) */
            int fast_ready = 0;

            /* Populate sizes before attempting a fast-path */
            pilatus_desc->base.det_g_id = g_id;
            pilatus_desc->base.data_g_id = ds_id;
            ds_prop_func((struct ds_desc_t*)pilatus_desc);

            if (build_direct_chunk_map(ds_id, pilatus_desc, opt) == 0) {
                opt->base = *pilatus_desc; /* copy dims/block_sizes ownership to opt */

                if (build_parallel_index(opt) == 0) {
                    fast_ready = 1;
                    free(pilatus_desc); /* block_sizes now owned by opt->base */
                } else {
                    /* Drop partial fast-path state without freeing block_sizes */
                    if (opt->fast_index) {
                        free(opt->fast_index);
                        opt->fast_index = NULL;
                    }
                    if (opt->frame_map_entries) {
                        for (int i = 0; i < opt->n_map_entries; ++i) {
                            struct FrameMapEntry* E = &opt->frame_map_entries[i];
                            if (E->src_dset_id > 0) H5Dclose(E->src_dset_id);
                            if (E->src_file_id > 0) H5Fclose(E->src_file_id);
                            free(E->src_file);
                            free(E->src_dset);
                            free(E->scratch);
                        }
                        free(opt->frame_map_entries);
                        opt->frame_map_entries = NULL;
                        opt->n_map_entries = 0;
                    }
                }
            }

            if (fast_ready) {
                *(struct opt_pilatus_ds_desc_t**)desc = opt;
                free_func = &free_opt_pilatus_desc;
                output = (struct ds_desc_t*)opt;
                output->get_data_frame = &get_virtual_frame_parallel;
            } else {
                free(opt);
                *(struct pilatus_ds_desc_t**)desc = pilatus_desc;
                free_func = &free_pilatus_desc;
                output = (struct ds_desc_t*)pilatus_desc;
                output->get_data_frame = &get_dectris_pilatus_frame;
            }

            output->det_g_id = g_id;
            output->data_g_id = ds_id;
        }
    } else {
        /* Standard Nexus */
        *desc = calloc(1, sizeof(struct nxs_ds_desc_t));
        if (!*desc) ERROR_JUMP(-1, done, "Out of memory");
        free_func = &free_nxs_desc;
        output = *desc;
        output->det_g_id = g_id;
        output->data_g_id = ds_id;
        retval = ds_prop_func(output);
        if (retval < 0) {
            ERROR_JUMP(retval, done, "Failed to read dataset properties");
        }
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

    /* Discover detector/data groups and build a descriptor for the dataset. */
    if (H5Ovisit(fid, H5_INDEX_NAME, H5_ITER_INC, &det_visit_callback, &objects) < 0) {
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
    };

done:
    return retval;
}
