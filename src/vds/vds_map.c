#include "vds_map.h"

#include <hdf5.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compression/bitshuffle_lz4.h"
#include "core/err.h"

struct MapBuilder {
    struct FrameMapEntry* entries;
    size_t used;
    size_t cap;
};

struct IndexList {
    hsize_t* values;
    size_t count;
};

/* Internal helper to duplicate string*/
static char* dupstr(const char* s) {
    size_t len = strlen(s) + 1;
    char* out = malloc(len);
    if (!out) return NULL;
    memcpy(out, s, len);
    return out;
}

static int ensure_capacity(struct MapBuilder* b, size_t extra) {
    if (b->used + extra <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap * 2 + extra : extra;
    struct FrameMapEntry* tmp = realloc(b->entries, new_cap * sizeof(*tmp));
    if (!tmp) return -1;
    b->entries = tmp;
    b->cap = new_cap;
    return 0;
}

static char* resolve_path(const char* base_dir, const char* rel) {
    if (rel[0] == '/') return dupstr(rel);  // Already absolute

    if (!base_dir || !base_dir[0]) return dupstr(rel);  // No base, return relative

    size_t len = strlen(base_dir) + 1 + strlen(rel) + 1;
    char* full = malloc(len);
    if (!full) return NULL;

    if (base_dir[strlen(base_dir) - 1] == '/') {
        snprintf(full, len, "%s%s", base_dir, rel);
    } else {
        snprintf(full, len, "%s/%s", base_dir, rel);
    }
    return full;
}

static void set_direct_flags(struct FrameMapEntry* E, hid_t sp) {
    H5D_layout_t lay = H5Pget_layout(sp);
    if (lay != H5D_CHUNKED) {
        E->direct_read = 0;
        E->has_bs_filter = 0;
        return;
    }
    int nf = H5Pget_nfilters(sp);
    if (nf == 0) {
        E->direct_read = 1;
        E->has_bs_filter = 0;
        return;
    }
    unsigned fcfg, flg;
    size_t n = BS_H5_N_PARAMS;
    H5Z_filter_t f = H5Pget_filter2(sp, 0, &flg, &n, E->bs_params, 0, NULL, &fcfg);
    if (f == BS_H5_FILTER_ID) {
        E->direct_read = 1;
        E->has_bs_filter = 1;
    }
}

static int append_leaf(struct MapBuilder* b, const char* src_full, const char* src_dset_name,
                       hid_t src_f, hid_t src_d, hsize_t v_start, hsize_t v_end, hsize_t s_start,
                       hsize_t s_end, struct opt_pilatus_ds_desc_t* opt, int* data_width_set) {
    int src_file_ref_acquired = 0;
    int src_dset_ref_acquired = 0;
    if (v_start >= opt->base.base.dims[0]) return 0;
    hsize_t src_dims[3] = {0};
    hid_t spc = H5Dget_space(src_d);
    H5Sget_simple_extent_dims(spc, src_dims, NULL);
    H5Sclose(spc);

    hsize_t max_len_v = (v_end >= v_start) ? (v_end - v_start + 1) : 0;
    hsize_t max_len_s = (s_end >= s_start) ? (s_end - s_start + 1) : 0;
    hsize_t max_len_src = (src_dims[0] > s_start) ? (src_dims[0] - s_start) : 0;
    hsize_t max_len_dst =
        (opt->base.base.dims[0] > v_start) ? (opt->base.base.dims[0] - v_start) : 0;
    hsize_t span = max_len_v;
    if (max_len_s < span) span = max_len_s;
    if (max_len_src < span) span = max_len_src;
    if (max_len_dst < span) span = max_len_dst;
    if (span == 0) return 0;

    if (ensure_capacity(b, 1) < 0) return -1;
    struct FrameMapEntry* E = &b->entries[b->used];
    memset(E, 0, sizeof(*E));

    E->vds_start = v_start;
    E->vds_end = v_start + span - 1;
    E->src_start = s_start;
    E->src_end = s_start + span - 1;
    E->src_file = dupstr(src_full);
    E->src_dset = dupstr(src_dset_name);
    if (!E->src_file || !E->src_dset) goto fail;
    if (H5Iinc_ref(src_f) < 0) goto fail;
    src_file_ref_acquired = 1;
    if (H5Iinc_ref(src_d) < 0) goto fail;
    src_dset_ref_acquired = 1;
    E->src_file_id = src_f;
    E->src_dset_id = src_d;

    hid_t sp = H5Dget_create_plist(src_d);
    set_direct_flags(E, sp);
    H5Pclose(sp);

    if (!*data_width_set) {
        hid_t t_id = H5Dget_type(src_d);
        opt->base.base.data_width = H5Tget_size(t_id);
        H5Tclose(t_id);
        *data_width_set = 1;
    }

    b->used++;
    return 0;

fail:
    if (src_dset_ref_acquired) H5Idec_ref(src_d);
    if (src_file_ref_acquired) H5Idec_ref(src_f);
    free(E->src_file);
    free(E->src_dset);
    memset(E, 0, sizeof(*E));
    return -1;
}

static void free_index_list(struct IndexList* list) {
    if (!list) return;
    free(list->values);
    list->values = NULL;
    list->count = 0;
}

static void destroy_map_entries(struct FrameMapEntry* entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; ++i) {
        struct FrameMapEntry* entry = &entries[i];
        if (entry->src_dset_id > 0) H5Dclose(entry->src_dset_id);
        if (entry->src_file_id > 0) H5Fclose(entry->src_file_id);
        free(entry->src_file);
        free(entry->src_dset);
        free(entry->scratch);
    }
    free(entries);
}

static int collect_selected_indices_1d(hid_t space, struct IndexList* out) {
    int retval = 0;
    int rank = H5Sget_simple_extent_ndims(space);
    hsize_t* dims = NULL;
    hssize_t nblocks = 0;
    hsize_t* blocklist = NULL;
    hsize_t total_indices = 0;
    size_t used = 0;

    memset(out, 0, sizeof(*out));
    if (rank <= 0) ERROR_JUMP(-1, done, "Invalid selection rank");

    dims = calloc((size_t)rank, sizeof(*dims));
    if (!dims) ERROR_JUMP(-1, done, "Out of memory for dataspace dimensions");
    if (H5Sget_simple_extent_dims(space, dims, NULL) < 0) {
        ERROR_JUMP(-1, done, "Failed to read dataspace dimensions");
    }

    nblocks = H5Sget_select_hyper_nblocks(space);
    if (nblocks < 0) ERROR_JUMP(-1, done, "Failed to get hyperslab block count");
    if (nblocks == 0) goto done;

    blocklist = calloc((size_t)nblocks * (size_t)rank * 2, sizeof(*blocklist));
    if (!blocklist) ERROR_JUMP(-1, done, "Out of memory for VDS hyperslab blocks");

    if (H5Sget_select_hyper_blocklist(space, 0, nblocks, blocklist) < 0) {
        ERROR_JUMP(-1, done, "Failed to get hyperslab block list");
    }

    for (hssize_t block = 0; block < nblocks; ++block) {
        size_t base = (size_t)block * (size_t)rank * 2;
        hsize_t start = blocklist[base];
        hsize_t end = blocklist[base + (size_t)rank];

        if (end < start || end >= dims[0]) {
            ERROR_JUMP(-1, done, "Invalid hyperslab bounds in frame dimension");
        }

        for (int dim = 1; dim < rank; ++dim) {
            hsize_t dim_start = blocklist[base + (size_t)dim];
            hsize_t dim_end = blocklist[base + (size_t)rank + (size_t)dim];

            if (dim_end < dim_start || dim_end >= dims[dim]) {
                ERROR_JUMP(-1, done, "Invalid hyperslab bounds in non-frame dimension");
            }
            if (dim_start != 0 || dim_end + 1 != dims[dim]) {
                ERROR_JUMP(-1, done, "Unsupported partial-frame VDS hyperslab");
            }
        }

        total_indices += (end - start) + 1;
    }

    if (total_indices == 0) goto done;
    if (total_indices > (hsize_t)SIZE_MAX) ERROR_JUMP(-1, done, "Too many VDS frame indices");

    out->values = calloc((size_t)total_indices, sizeof(*out->values));
    if (!out->values) ERROR_JUMP(-1, done, "Out of memory for VDS index list");

    for (hssize_t block = 0; block < nblocks; ++block) {
        size_t base = (size_t)block * (size_t)rank * 2;
        hsize_t start = blocklist[base];
        hsize_t end = blocklist[base + (size_t)rank];

        for (hsize_t idx = start; idx <= end; ++idx) {
            out->values[used++] = idx;
        }
    }

    if (used != (size_t)total_indices) ERROR_JUMP(-1, done, "Incomplete hyperslab frame expansion");
    out->count = used;

done:
    if (retval < 0) free_index_list(out);
    free(dims);
    free(blocklist);
    return retval;
}

static void filter_selected_pairs_by_virtual_range(struct IndexList* v_indices,
                                                   struct IndexList* s_indices, int has_clip,
                                                   hsize_t clip_start, hsize_t clip_end) {
    if (!has_clip || !v_indices || !s_indices) return;

    size_t keep = 0;
    for (size_t i = 0; i < v_indices->count; ++i) {
        hsize_t v = v_indices->values[i];
        if (v < clip_start || v > clip_end) continue;
        v_indices->values[keep] = v;
        s_indices->values[keep] = s_indices->values[i];
        ++keep;
    }
    v_indices->count = keep;
    s_indices->count = keep;
}

static int indices_are_contiguous(const struct IndexList* indices) {
    if (!indices || indices->count == 0) return 0;
    for (size_t i = 1; i < indices->count; ++i) {
        if (indices->values[i] != indices->values[i - 1] + 1) return 0;
    }
    return 1;
}

static int mapping_has_constant_translation(const struct IndexList* v_indices,
                                            const struct IndexList* s_indices,
                                            hssize_t* delta_out) {
    if (!v_indices || !s_indices || v_indices->count == 0 || s_indices->count == 0) return 0;

    hssize_t delta = (hssize_t)v_indices->values[0] - (hssize_t)s_indices->values[0];
    for (size_t i = 1; i < v_indices->count; ++i) {
        if (((hssize_t)v_indices->values[i] - (hssize_t)s_indices->values[i]) != delta) return 0;
    }
    *delta_out = delta;
    return 1;
}

static int append_selected_leaf_runs(struct MapBuilder* b, const char* src_full,
                                     const char* src_dset_name, hid_t src_f, hid_t src_d,
                                     hssize_t outer_v_offset, const struct IndexList* v_indices,
                                     const struct IndexList* s_indices,
                                     struct opt_pilatus_ds_desc_t* opt, int* data_width_set) {
    int retval = 0;

    if (!v_indices || !s_indices || v_indices->count != s_indices->count) {
        ERROR_JUMP(-1, done, "Mismatched VDS/source index lists");
    }
    if (v_indices->count == 0) goto done;

    for (size_t run_start = 0; run_start < v_indices->count;) {
        size_t run_end = run_start;
        while ((run_end + 1) < v_indices->count &&
               v_indices->values[run_end + 1] == (v_indices->values[run_end] + 1) &&
               s_indices->values[run_end + 1] == (s_indices->values[run_end] + 1)) {
            ++run_end;
        }

        hssize_t v_start_s = outer_v_offset + (hssize_t)v_indices->values[run_start];
        hssize_t v_end_s = outer_v_offset + (hssize_t)v_indices->values[run_end];
        if (v_start_s < 0 || v_end_s < 0) {
            ERROR_JUMP(-1, done, "Negative virtual offset after VDS expansion");
        }

        retval = append_leaf(b, src_full, src_dset_name, src_f, src_d, (hsize_t)v_start_s,
                             (hsize_t)v_end_s, s_indices->values[run_start],
                             s_indices->values[run_end], opt, data_width_set);
        if (retval < 0) goto done;

        run_start = run_end + 1;
    }

done:
    return retval;
}

/* Recursive flatten of a mapping entry */
static int flatten_mapping(struct MapBuilder* b, const char* current_base_dir, hid_t plist,
                           size_t map_idx, hssize_t outer_v_offset,
                           struct opt_pilatus_ds_desc_t* opt, int* data_width_set, int has_clip,
                           hsize_t clip_start, hsize_t clip_end) {
    int retval = 0;
    ssize_t fl = H5Pget_virtual_filename(plist, map_idx, NULL, 0);
    char* src_file_rel = calloc(fl + 1, 1);
    H5Pget_virtual_filename(plist, map_idx, src_file_rel, fl + 1);

    ssize_t dl = H5Pget_virtual_dsetname(plist, map_idx, NULL, 0);
    char* src_dset_name = calloc(dl + 1, 1);
    H5Pget_virtual_dsetname(plist, map_idx, src_dset_name, dl + 1);

    hid_t vsp = H5Pget_virtual_vspace(plist, map_idx);
    hid_t ssp = H5Pget_virtual_srcspace(plist, map_idx);
    struct IndexList v_indices = {0};
    struct IndexList s_indices = {0};
    if (collect_selected_indices_1d(vsp, &v_indices) < 0 ||
        collect_selected_indices_1d(ssp, &s_indices) < 0) {
        H5Sclose(vsp);
        H5Sclose(ssp);
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }
    H5Sclose(vsp);
    H5Sclose(ssp);

    if (v_indices.count != s_indices.count) {
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }

    filter_selected_pairs_by_virtual_range(&v_indices, &s_indices, has_clip, clip_start, clip_end);
    if (v_indices.count == 0) {
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_file_rel);
        free(src_dset_name);
        return 0;
    }

    char* src_full = resolve_path(current_base_dir, src_file_rel);

    if (!src_full) {
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }

    hid_t src_f = H5Fopen(src_full, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (src_f < 0) {
        fprintf(stderr, "[DURIN] Error: Could not open source file: %s\n", src_full);
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_full);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }
    hid_t src_d = H5Dopen2(src_f, src_dset_name, H5P_DEFAULT);
    if (src_d < 0) {
        H5Fclose(src_f);
        free_index_list(&v_indices);
        free_index_list(&s_indices);
        free(src_full);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }

    hid_t sp = H5Dget_create_plist(src_d);
    H5D_layout_t lay = H5Pget_layout(sp);

    if (lay == H5D_VIRTUAL) {
        size_t inner = 0;
        hssize_t inner_delta = 0;
        H5Pget_virtual_count(sp, &inner);

        if (!indices_are_contiguous(&s_indices) ||
            !mapping_has_constant_translation(&v_indices, &s_indices, &inner_delta)) {
            fprintf(stderr, "[DURIN] Unsupported nested VDS mapping pattern for %s\n", src_dset_name);
            retval = -1;
        } else {
            char* new_base_dir = dupstr(src_full);
            char* dname = dirname(new_base_dir);  // Modifies new_base_dir in place
            hssize_t inner_offset = outer_v_offset + inner_delta;
            hsize_t inner_clip_start = s_indices.values[0];
            hsize_t inner_clip_end = s_indices.values[s_indices.count - 1];

            for (size_t j = 0; j < inner; ++j) {
                if (flatten_mapping(b, dname, sp, j, inner_offset, opt, data_width_set, 1,
                                    inner_clip_start, inner_clip_end) < 0) {
                    retval = -1;
                    break;
                }
            }
            free(new_base_dir);
        }
        H5Pclose(sp);
        H5Dclose(src_d);
        H5Fclose(src_f);
    } else {
        retval = append_selected_leaf_runs(b, src_full, src_dset_name, src_f, src_d, outer_v_offset,
                                           &v_indices, &s_indices, opt, data_width_set);
        H5Pclose(sp);
        H5Dclose(src_d);
        H5Fclose(src_f);
    }

    free_index_list(&v_indices);
    free_index_list(&s_indices);
    free(src_full);
    free(src_file_rel);
    free(src_dset_name);
    return retval;
}

static int compare_frame_map_local(const void* a, const void* b) {
    const struct FrameMapEntry* A = a;
    const struct FrameMapEntry* B = b;
    return (A->vds_start > B->vds_start) - (A->vds_start < B->vds_start);
}

int build_vds_mapping(hid_t ds_id, const char* master_dir, struct opt_pilatus_ds_desc_t* opt,
                      struct pilatus_ds_desc_t* pilatus_desc_to_free) {
    int retval = 0;
    struct MapBuilder builder = {0};
    struct VdsBlockInfo {
        hsize_t frame_offset;
        hsize_t block_frames;
        size_t map_count;
    }* blocks = NULL;
    int n_blocks = 0;
    size_t total_maps = 0;
    hsize_t total_frames = 0;
    hsize_t dims1 = 0, dims2 = 0;
    char ds_name[MAX_NAME_LEN] = {0};
    int use_named_blocks =
        opt && opt->base.block_names && opt->base.block_starts && opt->base.n_data_blocks > 0;
    int named_count = use_named_blocks ? opt->base.n_data_blocks : 0;

    /* Discover blocks */
    for (int idx = 0;; idx++) {
        if (use_named_blocks) {
            if (idx >= named_count) break;
            snprintf(ds_name, sizeof(ds_name), "%s", opt->base.block_names[idx]);
        } else {
            snprintf(ds_name, sizeof(ds_name), "data_%06d", idx + 1);
            if (H5Lexists(ds_id, ds_name, H5P_DEFAULT) <= 0) break;
        }
        hid_t dset = H5Dopen2(ds_id, ds_name, H5P_DEFAULT);
        if (dset < 0) {
            retval = -1;
            goto done;
        }
        hid_t plist = H5Dget_create_plist(dset);
        H5D_layout_t layout = H5Pget_layout(plist);
        size_t maps = 0;
        if (layout == H5D_VIRTUAL) H5Pget_virtual_count(plist, &maps);
        if (layout != H5D_VIRTUAL || maps == 0) {
            H5Pclose(plist);
            H5Dclose(dset);
            retval = -1;
            goto done;
        }
        hid_t space = H5Dget_space(dset);
        hsize_t block_dims[3] = {0};
        H5Sget_simple_extent_dims(space, block_dims, NULL);
        H5Sclose(space);
        if (idx == 0) {
            dims1 = block_dims[1];
            dims2 = block_dims[2];
        }
        hsize_t block_offset = (use_named_blocks && idx < opt->base.n_data_blocks)
                                   ? opt->base.block_starts[idx]
                                   : total_frames;
        if (block_offset + block_dims[0] > total_frames)
            total_frames = block_offset + block_dims[0];
        struct VdsBlockInfo* tmp = realloc(blocks, (n_blocks + 1) * sizeof(*tmp));
        if (!tmp) {
            H5Pclose(plist);
            H5Dclose(dset);
            retval = -1;
            goto done;
        }
        blocks = tmp;
        blocks[n_blocks].frame_offset = block_offset;
        blocks[n_blocks].block_frames = block_dims[0];
        blocks[n_blocks].map_count = maps;
        n_blocks++;
        total_maps += maps;
        H5Pclose(plist);
        H5Dclose(dset);
    }

    if (n_blocks == 0) {
        retval = -1;
        goto done;
    }
    opt->base.base.dims[0] = total_frames;
    opt->base.base.dims[1] = dims1;
    opt->base.base.dims[2] = dims2;
    opt->is_virtual = 1;

    /* Build mappings */
    if (ensure_capacity(&builder, total_maps ? total_maps : 1) < 0) {
        retval = -1;
        goto done;
    }
    int data_width_set = 0;

    for (int b = 0; b < n_blocks; ++b) {
        if (use_named_blocks) {
            snprintf(ds_name, sizeof(ds_name), "%s", opt->base.block_names[b]);
        } else {
            snprintf(ds_name, sizeof(ds_name), "data_%06d", b + 1);
        }
        hid_t dset = H5Dopen2(ds_id, ds_name, H5P_DEFAULT);
        hid_t plist = H5Dget_create_plist(dset);
        size_t maps = blocks[b].map_count;
        for (size_t i = 0; i < maps; ++i) {
            hsize_t outer_offset = blocks[b].frame_offset;
            /* FIX: Pass master_dir as the initial base directory */
            if (flatten_mapping(&builder, master_dir, plist, i, (hssize_t)outer_offset, opt,
                                &data_width_set, 0, 0, 0) < 0) {
                retval = -1;
                break;
            }
        }
        H5Pclose(plist);
        H5Dclose(dset);
        if (retval < 0) break;
    }

    if (retval == 0) {
        qsort(builder.entries, builder.used, sizeof(struct FrameMapEntry), compare_frame_map_local);
        for (size_t i = 1; i < builder.used; ++i) {
            if (builder.entries[i].vds_start <= builder.entries[i - 1].vds_end) {
                retval = -1;
                break;
            }
        }
        if (retval == 0) {
            opt->frame_map_entries = builder.entries;
            opt->n_map_entries = (int)builder.used;
            if (pilatus_desc_to_free) free(pilatus_desc_to_free);
        } else {
            destroy_map_entries(builder.entries, builder.used);
        }
    } else {
        destroy_map_entries(builder.entries, builder.used);
    }

done:
    free(blocks);
    return retval;
}
