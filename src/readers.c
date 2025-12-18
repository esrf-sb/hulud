/* Must be before any includes to enable pread in C99 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h> /* For open() */
#include <hdf5.h>
#include <hdf5_hl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* For pread() */

#include "err.h"
#include "file.h"
#include "filters.h"

/* Build the fast index of raw chunk offsets for every mapped frame. */
int build_parallel_index(struct opt_pilatus_ds_desc_t* opt) {
    int retval = 0;
    int total_frames = opt->base.base.dims[0];
    int* entry_fds = calloc((size_t)opt->n_map_entries, sizeof(int));
    opt->fast_index = calloc(total_frames, sizeof(struct ParallelFrameIndex));
    if (!opt->fast_index || !entry_fds) {
        free(entry_fds);
        free(opt->fast_index);
        opt->fast_index = NULL;
        return -1;
    }

    int i = 0;
    for (i = 0; i < opt->n_map_entries; ++i) {
        struct FrameMapEntry* E = &opt->frame_map_entries[i];

        /* Open the raw source file directly (OS level) */
        int fd = open(E->src_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[DURIN] Failed to open raw file for parallel read: %s\n", E->src_file);
            /* Cleanup will happen in free_opt_pilatus_desc */
            retval = -1;
            goto fail;
        }
        E->raw_fd = fd;
        entry_fds[i] = fd;

        hsize_t count = (E->vds_end - E->vds_start) + 1;

        for (hsize_t j = 0; j < count; ++j) {
            int global_frame = E->vds_start + j;
            hsize_t source_index = E->src_start + j;

            hsize_t chunk_offset[3] = {source_index, 0, 0};
            haddr_t byte_addr;
            hsize_t size;
            unsigned filter_mask;

            /* Use HDF5 to calculate the file offset */
            /* H5Dget_chunk_info_by_coord works on 1.10+ */
            if (H5Dget_chunk_info_by_coord(E->src_dset_id, chunk_offset, &filter_mask, &byte_addr,
                                           &size) < 0) {
                fprintf(stderr, "[DURIN] Failed to get chunk info for frame %d\n", global_frame);
                retval = -1;
                goto fail;
            }

            /* Store in fast index */
            opt->fast_index[global_frame].fd = fd;
            opt->fast_index[global_frame].byte_offset = (long long)byte_addr;
            opt->fast_index[global_frame].chunk_size = (size_t)size;
            opt->fast_index[global_frame].bs_params = E->bs_params;
            /* If the bitshuffle filter is disabled for this chunk, don't try to decompress */
            opt->fast_index[global_frame].has_bs_filter =
                E->has_bs_filter && ((filter_mask & 0x1u) == 0);
        }
    }
    free(entry_fds);
    return retval;

fail:
    for (int k = 0; k <= i && k < opt->n_map_entries; ++k) {
        if (entry_fds[k] > 0) close(entry_fds[k]);
        opt->frame_map_entries[k].raw_fd = -1;
    }
    free(entry_fds);
    free(opt->fast_index);
    opt->fast_index = NULL;
    return retval;
}

/* Thread-safe reader that bypasses HDF5 and uses pread() with the fast index. */
int get_virtual_frame_parallel(const struct ds_desc_t* d, const int f, void* buf) {
    const struct opt_pilatus_ds_desc_t* opt = (const void*)d;
    size_t expected = (size_t)d->data_width * (size_t)d->dims[1] * (size_t)d->dims[2];

    /* Bounds check */
    if (f < 0 || f >= d->dims[0]) return -1;

    struct ParallelFrameIndex* idx = &opt->fast_index[f];

    /* Check if index is valid (fd must be > 0) */
    if (idx->fd <= 0) {
        /* Gap in mapping: return a zero-filled frame */
        memset(buf, 0, expected);
        return 0;
    }

    /* Allocate LOCAL scratch buffer (Thread Safe) */
    void* local_scratch = malloc(idx->chunk_size);
    if (!local_scratch) return -1;

    /* Perform read */
    ssize_t bytes_read = pread(idx->fd, local_scratch, idx->chunk_size, idx->byte_offset);

    int retval = 0;
    if (bytes_read != (ssize_t)idx->chunk_size || idx->chunk_size == 0) {
        retval = -1;
    } else {
        if (idx->has_bs_filter) {
            /* Decompress Bitshuffle-LZ4 */
            retval =
                bslz4_decompress(idx->bs_params, idx->chunk_size, local_scratch, expected, buf);
        } else if (idx->chunk_size == expected) {
            /* No compression: copy bytes directly */
            memcpy(buf, local_scratch, expected);
            retval = 0;
        } else {
            fprintf(stderr, "[DURIN] Unsupported compression pipeline for frame %d\n", f);
            retval = -1;
        }
    }

    free(local_scratch);
    return retval;
}

/* Fallback HDF5-based reader, used when the fast path is not available. */
int get_virtual_frame(const struct ds_desc_t* d, const int f, void* buf) {
    const struct opt_pilatus_ds_desc_t* opt = (const void*)d;

    int found = 0;
    int retval = -2; /* default: no mapping found */
    int used_fallback = 0;

    for (int i = 0; i < opt->n_map_entries; ++i) {
        struct FrameMapEntry* E = &opt->frame_map_entries[i];
        if (f < E->vds_start || f > E->vds_end) continue;
        found = 1;

        hsize_t li = E->src_start + (f - E->vds_start);
        hsize_t off[3] = {li, 0, 0}, size[3] = {1, d->dims[1], d->dims[2]};

        if (E->direct_read) {
            uint32_t mask = 0;
            hsize_t sz = 0;
            if (E->chunk_info_valid && E->cached_chunk_index == li) {
                sz = E->cached_chunk_bytes;
                mask = E->cached_chunk_mask;
            } else {
                haddr_t addr = 0;
                if (H5Dget_chunk_info_by_coord(E->src_dset_id, off, &mask, &addr, &sz) < 0 ||
                    sz == 0) {
                    if (H5Dget_chunk_storage_size(E->src_dset_id, off, &sz) < 0 || sz == 0) {
                        used_fallback = 1;
                        goto regular_read;
                    }
                }
                E->cached_chunk_bytes = sz;
                E->cached_chunk_mask = mask;
                E->cached_chunk_index = li;
                E->chunk_info_valid = 1;
            }
            void* tmp = buf;
            if (E->has_bs_filter) {
                if (!E->scratch || E->scratch_size < (size_t)sz) {
                    void* newbuf = realloc(E->scratch, sz);
                    if (!newbuf) {
                        ERROR_JUMP(-1, done, "Out of memory for chunk buffer");
                    }
                    E->scratch = newbuf;
                    E->scratch_size = (size_t)sz;
                }
                tmp = E->scratch;
            }
            if (H5DOread_chunk(E->src_dset_id, H5P_DEFAULT, off, &mask, tmp) < 0) {
                ERROR_JUMP(-1, done, "H5DOread_chunk failed");
            }
            if (E->has_bs_filter) {
                int dec = bslz4_decompress(E->bs_params, sz, tmp,
                                           d->data_width * d->dims[1] * d->dims[2], buf);
                if (dec < 0) {
                    ERROR_JUMP(-1, done, "Bitshuffle-LZ4 decompress failed");
                }
            }
            return 0;
        }

    regular_read:
        (void)used_fallback; /* Silence unused var warning */
        hid_t fs = H5Dget_space(E->src_dset_id);
        if (fs < 0) ERROR_JUMP(-1, done, "H5Dget_space failed");
        H5Sselect_hyperslab(fs, H5S_SELECT_SET, off, NULL, size, NULL);
        hid_t ms = H5Screate_simple(3, size, NULL);
        if (ms < 0) {
            H5Sclose(fs);
            ERROR_JUMP(-1, done, "H5Screate_simple failed");
        }
        hid_t t = H5Dget_type(E->src_dset_id);
        if (t < 0) {
            H5Sclose(ms);
            H5Sclose(fs);
            ERROR_JUMP(-1, done, "H5Dget_type failed");
        }
        if (H5Dread(E->src_dset_id, t, ms, fs, H5P_DEFAULT, buf) < 0) {
            H5Tclose(t);
            H5Sclose(ms);
            H5Sclose(fs);
            ERROR_JUMP(-1, done, "H5Dread failed");
        }
        H5Tclose(t);
        H5Sclose(ms);
        H5Sclose(fs);
        return 0;
    }
    retval = found ? -1 : -2;
done:
    if (retval == -2) {
        /* Gap in mapping: zero-fill the buffer to emulate VDS fill value */
        size_t expected = (size_t)d->data_width * (size_t)d->dims[1] * (size_t)d->dims[2];
        memset(buf, 0, expected);
        return 0;
    }
    return retval;
}
