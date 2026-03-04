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
    struct FrameMapEntry* E = &b->entries[b->used++];
    memset(E, 0, sizeof(*E));

    E->vds_start = v_start;
    E->vds_end = v_start + span - 1;
    E->src_start = s_start;
    E->src_end = s_start + span - 1;
    E->src_file = dupstr(src_full);
    E->src_dset = dupstr(src_dset_name);
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

    return 0;
}

/* Clip a 1D VDS mapping (v0..v1 -> s0..s1) to a vspace range.
 * Assumes a contiguous 1:1 mapping along the first dimension (typical for Lima2 VDS slices).
 * Returns 0 if no overlap, 1 if clipped/kept.
 */
static int clip_mapping_1d(hsize_t v0, hsize_t v1, hsize_t s0, hsize_t s1, int has_clip,
                           hsize_t clip_start, hsize_t clip_end, hsize_t* out_v0, hsize_t* out_v1,
                           hsize_t* out_s0, hsize_t* out_s1) {
    if (!has_clip) {
        *out_v0 = v0;
        *out_v1 = v1;
        *out_s0 = s0;
        *out_s1 = s1;
        return 1;
    }

    if (clip_end < v0 || clip_start > v1) return 0;

    hsize_t nv0 = (clip_start > v0) ? clip_start : v0;
    hsize_t nv1 = (clip_end < v1) ? clip_end : v1;
    if (nv0 > nv1) return 0;

    hsize_t shift = nv0 - v0;
    hsize_t span = nv1 - nv0;
    hsize_t ns0 = s0 + shift;
    hsize_t ns1 = ns0 + span;

    *out_v0 = nv0;
    *out_v1 = nv1;
    *out_s0 = ns0;
    *out_s1 = ns1;
    return 1;
}

/* Recursive flatten of a mapping entry */
static int flatten_mapping(struct MapBuilder* b, const char* current_base_dir, hid_t plist,
                           size_t map_idx, hssize_t outer_v_offset,
                           struct opt_pilatus_ds_desc_t* opt, int* data_width_set, int has_clip,
                           hsize_t clip_start, hsize_t clip_end) {
    ssize_t fl = H5Pget_virtual_filename(plist, map_idx, NULL, 0);
    char* src_file_rel = calloc(fl + 1, 1);
    H5Pget_virtual_filename(plist, map_idx, src_file_rel, fl + 1);

    ssize_t dl = H5Pget_virtual_dsetname(plist, map_idx, NULL, 0);
    char* src_dset_name = calloc(dl + 1, 1);
    H5Pget_virtual_dsetname(plist, map_idx, src_dset_name, dl + 1);

    hid_t vsp = H5Pget_virtual_vspace(plist, map_idx);
    hid_t ssp = H5Pget_virtual_srcspace(plist, map_idx);
    hsize_t v0[3], v1[3], s0[3], s1[3];
    H5Sget_select_bounds(vsp, v0, v1);
    H5Sget_select_bounds(ssp, s0, s1);
    H5Sclose(vsp);
    H5Sclose(ssp);

    /* Clip to a subset of the current vspace if requested */
    hsize_t cv0 = v0[0], cv1 = v1[0], cs0 = s0[0], cs1 = s1[0];
    if (!clip_mapping_1d(v0[0], v1[0], s0[0], s1[0], has_clip, clip_start, clip_end, &cv0, &cv1,
                         &cs0, &cs1)) {
        free(src_file_rel);
        free(src_dset_name);
        return 0; /* No overlap with clip range */
    }

    /* FIX: Resolve path relative to current_base_dir */
    char* src_full = resolve_path(current_base_dir, src_file_rel);

    if (!src_full) {
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }

    hid_t src_f = H5Fopen(src_full, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (src_f < 0) {
        fprintf(stderr, "[DURIN] Error: Could not open source file: %s\n", src_full);
        free(src_full);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }
    hid_t src_d = H5Dopen2(src_f, src_dset_name, H5P_DEFAULT);
    if (src_d < 0) {
        H5Fclose(src_f);
        free(src_full);
        free(src_file_rel);
        free(src_dset_name);
        return -1;
    }

    hid_t sp = H5Dget_create_plist(src_d);
    H5D_layout_t lay = H5Pget_layout(sp);

    int retval = 0;
    if (lay == H5D_VIRTUAL) {
        size_t inner = 0;
        H5Pget_virtual_count(sp, &inner);

        char* new_base_dir = dupstr(src_full);
        char* dname = dirname(new_base_dir);  // Modifies new_base_dir in place

        for (size_t j = 0; j < inner; ++j) {
            /* Map inner VDS indices [cs0..cs1] into outer VDS indices.
             * outer_index = outer_v_offset + cv0 + (inner_index - cs0)
             * => inner offset for recursion is (outer_v_offset + cv0 - cs0)
             */
            hssize_t inner_offset = outer_v_offset + (hssize_t)cv0 - (hssize_t)cs0;
            /* Pass the new directory as the base for the inner mapping.
             * Clip inner mappings to [cs0..cs1] (source selection from outer mapping).
             */
            if (flatten_mapping(b, dname, sp, j, inner_offset, opt, data_width_set, 1, cs0, cs1) <
                0) {
                retval = -1;
                break;
            }
        }
        free(new_base_dir);
        H5Pclose(sp);
        H5Dclose(src_d);
        H5Fclose(src_f);
    } else {
        hssize_t v_start_s = outer_v_offset + (hssize_t)cv0;
        hssize_t v_end_s = outer_v_offset + (hssize_t)cv1;
        if (v_start_s < 0 || v_end_s < 0) {
            fprintf(stderr,
                    "[DURIN] Error: negative virtual offset after clipping (v_start=%lld, "
                    "v_end=%lld)\n",
                    (long long)v_start_s, (long long)v_end_s);
            H5Pclose(sp);
            H5Dclose(src_d);
            H5Fclose(src_f);
            free(src_full);
            free(src_file_rel);
            free(src_dset_name);
            return -1;
        }
        hsize_t v_start = (hsize_t)v_start_s;
        hsize_t v_end = (hsize_t)v_end_s;
        retval = append_leaf(b, src_full, src_dset_name, src_f, src_d, v_start, v_end, cs0, cs1,
                             opt, data_width_set);
        H5Pclose(sp);
        if (retval < 0) {
            H5Dclose(src_d);
            H5Fclose(src_f);
        }
    }

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
        /* Re-pack spans sequentially to avoid gaps/overlaps */
        hsize_t next_v = 0;
        size_t keep = 0;
        for (size_t i = 0; i < builder.used && next_v < opt->base.base.dims[0]; ++i) {
            struct FrameMapEntry* E = &builder.entries[i];
            hsize_t span = (E->src_end >= E->src_start) ? (E->src_end - E->src_start + 1) : 0;
            if (span == 0) continue;
            if (next_v + span > opt->base.base.dims[0]) {
                span = opt->base.base.dims[0] - next_v;
                E->src_end = E->src_start + span - 1;
            }
            E->vds_start = next_v;
            E->vds_end = next_v + span - 1;
            builder.entries[keep++] = *E;
            next_v += span;
        }
        builder.used = keep;
        opt->frame_map_entries = builder.entries;
        opt->n_map_entries = (int)builder.used;
        if (pilatus_desc_to_free) free(pilatus_desc_to_free);
    } else {
        free(builder.entries);
    }

done:
    free(blocks);
    return retval;
}
