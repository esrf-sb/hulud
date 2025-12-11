/*
 * Corrected readers.c
 * Fix: Removed shared scratch buffers to fix Segmentation Fault in parallel benchmarks.
 */
#include <hdf5.h>
#include <hdf5_hl.h>
#include <stdio.h>
#include <stdlib.h>

#include "err.h"
#include "file.h"
#include "filters.h"

int get_virtual_frame(const struct ds_desc_t* d, const int f, void* buf) {
    const struct opt_eiger_ds_desc_t* opt = (const void*)d;

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

            /* THREAD SAFETY FIX: Always query chunk info locally.
             * Do NOT use shared E->cached_chunk_* variables.
             */
            haddr_t addr = 0;
            if (H5Dget_chunk_info_by_coord(E->src_dset_id, off, &mask, &addr, &sz) < 0 || sz == 0) {
                if (H5Dget_chunk_storage_size(E->src_dset_id, off, &sz) < 0 || sz == 0) {
                    used_fallback = 1;
                    goto regular_read;
                }
            }

            /* THREAD SAFETY FIX: Use local buffer, never E->scratch */
            void* tmp = buf;
            void* local_scratch = NULL;

            if (E->has_bs_filter) {
                local_scratch = malloc(sz);
                if (!local_scratch) {
                    ERROR_JUMP(-1, done, "Out of memory for chunk buffer");
                }
                tmp = local_scratch;
            }

            /* Read Raw Chunk */
            if (H5DOread_chunk(E->src_dset_id, H5P_DEFAULT, off, &mask, tmp) < 0) {
                if (local_scratch) free(local_scratch);
                ERROR_JUMP(-1, done, "H5DOread_chunk failed");
            }

            /* Decompress */
            if (E->has_bs_filter) {
                int dec = bslz4_decompress(E->bs_params, sz, tmp,
                                           d->data_width * d->dims[1] * d->dims[2], buf);
                free(local_scratch); /* Free immediately */

                if (dec < 0) {
                    ERROR_JUMP(-1, done, "Bitshuffle-LZ4 decompress failed");
                }
            }
            return 0;
        }

    regular_read:
        /* Fallback path */
        {
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
    }
    retval = found ? -1 : -2;
done:
    return retval;
}