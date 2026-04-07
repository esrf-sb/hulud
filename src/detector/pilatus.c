#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compression/bitshuffle_lz4.h"
#include "core/err.h"
#include "core/internal.h"

static int validate_frame_number(const struct ds_desc_t* desc, int frame_number) {
    if (frame_number < 0 || (hsize_t)frame_number >= desc->dims[0]) {
        char message[MAX_MSG_LEN];
        snprintf(message, sizeof(message), "Selected frame %d is out of range valid range [0, %d]",
                 frame_number, (int)desc->dims[0] - 1);
        push_error_stack(__file__, __func__, __line__, -1, message);
        return -1;
    }
    return 0;
}

static int locate_block_for_frame(const struct pilatus_ds_desc_t* pilatus_desc, int frame_number,
                                  int* block_index, hsize_t* block_start_frame) {
    *block_index = -1;
    *block_start_frame = 0;

    if (pilatus_desc->block_starts) {
        for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) {
            hsize_t start = pilatus_desc->block_starts[i];
            hsize_t end = start + (hsize_t)pilatus_desc->block_sizes[i] - 1;
            if ((hsize_t)frame_number >= start && (hsize_t)frame_number <= end) {
                *block_index = i;
                *block_start_frame = start;
                return 0;
            }
        }
    } else {
        hsize_t frame_count = 0;
        for (int i = 0; i < pilatus_desc->n_data_blocks; ++i) {
            frame_count += (hsize_t)pilatus_desc->block_sizes[i];
            if (frame_count > (hsize_t)frame_number) {
                *block_index = i;
                *block_start_frame = frame_count - (hsize_t)pilatus_desc->block_sizes[i];
                return 0;
            }
        }
    }
    return -1;
}

static void build_data_block_name(const struct pilatus_ds_desc_t* pilatus_desc, int block_index,
                                  char* output_name, size_t output_size) {
    if (pilatus_desc->block_names && pilatus_desc->block_names[block_index]) {
        snprintf(output_name, output_size, "%s", pilatus_desc->block_names[block_index]);
    } else {
        snprintf(output_name, output_size, "data_%06d", block_index + 1);
    }
}

int get_dectris_pilatus_frame(const struct ds_desc_t* desc, int n, void* buffer) {
    int retval = 0;
    struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)desc;
    char block_dataset_name[MAX_NAME_LEN] = {0};
    hsize_t frame_idx[3] = {0, 0, 0};
    hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};
    int block_index = -1;
    hsize_t block_start_frame = 0;

    if (validate_frame_number(desc, n) < 0) ERROR_JUMP(-1, done, "");

    if (locate_block_for_frame(pilatus_desc, n, &block_index, &block_start_frame) < 0) {
        memset(buffer, 0, (size_t)desc->data_width * (size_t)desc->dims[1] * (size_t)desc->dims[2]);
        goto done;
    }

    frame_idx[0] = (hsize_t)(n - (int)block_start_frame);
    build_data_block_name(pilatus_desc, block_index, block_dataset_name,
                          sizeof(block_dataset_name));

    retval = pilatus_desc->frame_func(desc, block_dataset_name, frame_idx, frame_size, buffer);
    if (retval < 0) ERROR_JUMP(retval, done, "");
done:
    return retval;
}

struct data_block_info {
    char name[MAX_NAME_LEN];
    hsize_t frames;
    hsize_t start;
    hsize_t end;
    int has_range;
};

struct data_block_list {
    struct data_block_info* blocks;
    size_t used;
    size_t cap;
    hsize_t dim1;
    hsize_t dim2;
    int data_width;
    int data_is_signed;
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
    for (const char* p = name + 5; *p; ++p)
        if (!isdigit((unsigned char)*p)) return 0;

    hid_t ds_id = H5Dopen2(g_id, name, H5P_DEFAULT);
    if (ds_id < 0) return 0;

    hid_t s_id = H5Dget_space(ds_id);
    if (s_id < 0) {
        H5Dclose(ds_id);
        return 0;
    }
    (void)H5Sget_simple_extent_ndims(s_id);
    hsize_t dims[3] = {0};
    H5Sget_simple_extent_dims(s_id, dims, NULL);
    hid_t t_id = H5Dget_type(ds_id);
    int width = 0;
    int data_is_signed = 0;

    if (t_id < 0 || get_integer_type_properties(t_id, &width, &data_is_signed) < 0) {
        if (t_id >= 0) H5Tclose(t_id);
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return -1;
    }

    if (ctx->data_width == 0) {
        ctx->data_width = width;
        ctx->data_is_signed = data_is_signed;
    } else if (ctx->data_width != width || ctx->data_is_signed != data_is_signed) {
        H5Tclose(t_id);
        H5Sclose(s_id);
        H5Dclose(ds_id);
        return -1;
    }
    if (ctx->dim1 == 0) {
        ctx->dim1 = dims[1];
        ctx->dim2 = dims[2];
    }

    hsize_t start = 0, end = dims[0] ? dims[0] - 1 : 0;
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
    return 0;
}

int get_dectris_pilatus_dataset_dims(struct ds_desc_t* desc) {
    int retval = 0;
    struct pilatus_ds_desc_t* pilatus_desc = (struct pilatus_ds_desc_t*)desc;
    struct data_block_list ctx = {0};
    hsize_t total_frames = 0;

    if (H5Literate(desc->data_g_id, H5_INDEX_NAME, H5_ITER_NATIVE, NULL, collect_data_block_cb,
                   &ctx) < 0)
        ERROR_JUMP(-1, done, "Error iterating data_* datasets");
    if (ctx.used == 0) ERROR_JUMP(-1, done, "No data_* datasets found");

    if (ctx.any_range) {
        qsort(ctx.blocks, ctx.used, sizeof(*ctx.blocks), compare_blocks_by_start);
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
    pilatus_desc->block_sizes = calloc(ctx.used, sizeof(int));
    pilatus_desc->block_starts = calloc(ctx.used, sizeof(hsize_t));
    pilatus_desc->block_names = calloc(ctx.used, sizeof(char*));

    for (size_t i = 0; i < ctx.used; ++i) {
        pilatus_desc->block_sizes[i] = (int)ctx.blocks[i].frames;
        pilatus_desc->block_starts[i] = ctx.blocks[i].start;
        pilatus_desc->block_names[i] = dup_cstr(ctx.blocks[i].name);
        if (ctx.blocks[i].end + 1 > total_frames) total_frames = ctx.blocks[i].end + 1;
    }

    desc->dims[0] = total_frames;
    desc->dims[1] = ctx.dim1;
    desc->dims[2] = ctx.dim2;
    desc->data_width = ctx.data_width;
    desc->data_is_signed = ctx.data_is_signed;

done:
    free(ctx.blocks);
    return retval;
}

int get_dectris_pilatus_pixel_info(const struct ds_desc_t* desc, double* x_size, double* y_size) {
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/x_pixel_size", x_size) < 0) return -1;
    if (read_pixel_info(desc->det_g_id, "detectorSpecific/y_pixel_size", y_size) < 0) return -1;
    return 0;
}

int get_dectris_pilatus_pixel_mask(const struct ds_desc_t* desc, int* buffer) {
    return get_pixel_mask_impl(desc, desc->det_g_id, "detectorSpecific/pixel_mask", buffer);
}

static void resolve_master_dir_from_data_group(hid_t data_group_id, char* master_dir,
                                               size_t master_dir_len) {
    hid_t master_file_id = H5Iget_file_id(data_group_id);
    master_dir[0] = '\0';
    if (master_file_id <= 0) return;

    char master_path[MAX_PATH_LEN] = {0};
    if (H5Fget_name(master_file_id, master_path, sizeof(master_path)) > 0) {
        char resolved_path[MAX_PATH_LEN] = {0};
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
    H5Fclose(master_file_id);
}

static void cleanup_direct_chunk_map(struct opt_pilatus_ds_desc_t* opt_desc, int block_count) {
    if (!opt_desc || !opt_desc->frame_map_entries) return;
    for (int i = 0; i < block_count; ++i) {
        struct FrameMapEntry* entry = &opt_desc->frame_map_entries[i];
        if (entry->src_dset_id > 0) H5Dclose(entry->src_dset_id);
        if (entry->src_file_id > 0) H5Fclose(entry->src_file_id);
        free(entry->src_file);
        free(entry->src_dset);
    }
    free(opt_desc->frame_map_entries);
    opt_desc->frame_map_entries = NULL;
}

static int set_direct_read_flags(hid_t dataset_id, struct FrameMapEntry* entry) {
    hid_t plist_id = H5Dget_create_plist(dataset_id);
    if (plist_id < 0) return -1;

    H5D_layout_t layout = H5Pget_layout(plist_id);
    if (layout == H5D_CHUNKED) {
        int filter_count = H5Pget_nfilters(plist_id);
        if (filter_count == 0) {
            entry->direct_read = 1;
        } else {
            unsigned int flags = 0;
            unsigned int config = 0;
            size_t param_count = BS_H5_N_PARAMS;
            H5Z_filter_t filter_type = H5Pget_filter2(plist_id, 0, &flags, &param_count,
                                                      entry->bs_params, 0, NULL, &config);
            if (filter_type == BS_H5_FILTER_ID) {
                entry->direct_read = 1;
                entry->has_bs_filter = 1;
            }
        }
    }
    H5Pclose(plist_id);
    return 0;
}

static void get_block_dataset_name(const struct pilatus_ds_desc_t* base_desc, int block_index,
                                   char* dataset_name, size_t dataset_name_size) {
    if (base_desc->block_names && base_desc->block_names[block_index]) {
        snprintf(dataset_name, dataset_name_size, "%s", base_desc->block_names[block_index]);
    } else {
        snprintf(dataset_name, dataset_name_size, "data_%06d", block_index + 1);
    }
}

/* Helper for file.c to build direct chunk map for non-VDS Pilatus datasets */
int build_direct_chunk_map(hid_t data_group_id, struct pilatus_ds_desc_t* base,
                           struct opt_pilatus_ds_desc_t* opt) {
    int retval = 0;
    hsize_t frame_offset = 0;
    char master_dir[MAX_PATH_LEN] = {0};

    resolve_master_dir_from_data_group(data_group_id, master_dir, sizeof(master_dir));

    opt->frame_map_entries = calloc(base->n_data_blocks, sizeof(struct FrameMapEntry));
    if (!opt->frame_map_entries) return -1;

    for (int block = 0; block < base->n_data_blocks; ++block) {
        char ds_name[MAX_NAME_LEN] = {0};
        get_block_dataset_name(base, block, ds_name, sizeof(ds_name));

        hid_t dset = H5Dopen2(data_group_id, ds_name, H5P_DEFAULT);
        if (dset < 0) {
            retval = -1;
            break;
        }

        struct FrameMapEntry* entry = &opt->frame_map_entries[block];
        memset(entry, 0, sizeof(*entry));

        if (set_direct_read_flags(dset, entry) < 0) {
            H5Dclose(dset);
            retval = -1;
            break;
        }

        if (!entry->direct_read) {
            H5Dclose(dset);
            retval = -1;
            break;
        }

        if (!base->block_sizes || base->block_sizes[block] <= 0) {
            H5Dclose(dset);
            retval = -1;
            break;
        }
        hsize_t block_frames = (hsize_t)base->block_sizes[block];
        hsize_t start = (base->block_starts) ? base->block_starts[block] : frame_offset;

        entry->vds_start = start;
        entry->vds_end = start + block_frames - 1;
        entry->src_start = 0;
        entry->src_end = block_frames - 1;

        char ds_path[MAX_NAME_LEN] = {0};
        if (H5Iget_name(dset, ds_path, sizeof(ds_path)) < 0) {
            H5Dclose(dset);
            retval = -1;
            break;
        }
        entry->src_dset = dup_cstr(ds_path);

        hid_t fid = H5Iget_file_id(dset);
        char* canonical_path = canonical_file_path(fid, master_dir);
        if (!canonical_path) {
            H5Fclose(fid);
            H5Dclose(dset);
            retval = -1;
            break;
        }
        entry->src_file = canonical_path;
        entry->src_file_id = fid;
        entry->src_dset_id = dset;

        frame_offset = entry->vds_end + 1;
    }

    if (retval < 0) {
        cleanup_direct_chunk_map(opt, base->n_data_blocks);
    } else {
        opt->n_map_entries = base->n_data_blocks;
        opt->is_virtual = 0;
    }
    return retval;
}
