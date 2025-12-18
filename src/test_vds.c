#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hdf5.h"

// Structure to hold a mapping entry (frame range -> source location)
typedef struct {
    hsize_t v_start;
    hsize_t v_end;
    char* src_file;
    char* src_dset;
    hsize_t src_start;
} FrameMapEntry;

int main() {
    const char* vds_filename =
        "/path/to/data/"
        "run_01_MXPressA/run_01_02_mesh/mesh-SIM2_S8-2025-11-24-05_1_2_1_master.h5";
    const char* vds_dataset = "/entry/data/data_000001";

    // Open the VDS file and dataset
    hid_t file_id = H5Fopen(vds_filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        fprintf(stderr, "Failed to open file %s\n", vds_filename);
        return 1;
    }
    hid_t dset_id = H5Dopen2(file_id, vds_dataset, H5P_DEFAULT);
    if (dset_id < 0) {
        fprintf(stderr, "Failed to open dataset %s\n", vds_dataset);
        H5Fclose(file_id);
        return 1;
    }

    // Get creation property list and verify virtual layout
    hid_t dcpl_id = H5Dget_create_plist(dset_id);
    if (H5Pget_layout(dcpl_id) != H5D_VIRTUAL) {
        fprintf(stderr, "Dataset is not a virtual dataset!\n");
        // Clean up and exit
        H5Pclose(dcpl_id);
        H5Dclose(dset_id);
        H5Fclose(file_id);
        return 1;
    }

    // Get total number of mappings
    size_t num_maps = 0;
    H5Pget_virtual_count(dcpl_id, &num_maps);
    printf("VDS has %zu mappings.\n", num_maps);

    // Prepare an array to hold mapping entries (allocate for worst-case: each mapping might yield
    // multiple blocks)
    FrameMapEntry* map_entries =
        malloc(num_maps * 10 * sizeof(FrameMapEntry));  // allocate 10x num_maps for safety
    size_t map_count = 0;

    for (size_t i = 0; i < num_maps; ++i) {
        // Get source file and dataset names
        ssize_t len = H5Pget_virtual_filename(dcpl_id, i, NULL, 0);
        char* src_filename = malloc(len + 1);
        H5Pget_virtual_filename(dcpl_id, i, src_filename, len + 1);

        len = H5Pget_virtual_dsetname(dcpl_id, i, NULL, 0);
        char* src_dsetname = malloc(len + 1);
        H5Pget_virtual_dsetname(dcpl_id, i, src_dsetname, len + 1);

        // Get the virtual and source dataspaces for this mapping
        hid_t vspace_id = H5Pget_virtual_vspace(dcpl_id, i);
        hid_t src_space_id = H5Pget_virtual_srcspace(dcpl_id, i);

        // Determine the selection type and get hyperslab blocks
        H5S_sel_type vsel_type = H5Sget_select_type(vspace_id);
        H5S_sel_type ssel_type = H5Sget_select_type(src_space_id);
        int ndims = H5Sget_simple_extent_ndims(vspace_id);

        // Get block lists for virtual and source selections (if hyperslab or regular selection)
        if (vsel_type == H5S_SEL_ALL) {
            // ALL selection means the entire virtual dataset hyperslab for this mapping.
            // We can get the extent bounds (min and max coords in each dimension).
            hsize_t v_start[H5S_MAX_RANK], v_end[H5S_MAX_RANK];
            H5Sget_select_bounds(vspace_id, v_start, v_end);
            // For source, handle ALL or hyperslab similarly:
            hsize_t s_start[H5S_MAX_RANK] = {0}, s_end[H5S_MAX_RANK] = {0};
            if (ssel_type == H5S_SEL_ALL) {
                // Entire source dataset
                hsize_t src_dims[H5S_MAX_RANK];
                H5Sget_simple_extent_dims(src_space_id, src_dims, NULL);
                // Source frames 0 through src_dims[0]-1 map to VDS v_start[0] through v_end[0]
                s_end[0] = src_dims[0] - 1;
            } else {
                // Source selection is hyperslab
                H5Sget_select_bounds(src_space_id, s_start, s_end);
            }
            // Record this mapping as one entry
            FrameMapEntry entry;
            entry.v_start = v_start[0];
            entry.v_end = v_end[0];
            entry.src_file = strdup(src_filename);
            entry.src_dset = strdup(src_dsetname);
            entry.src_start = s_start[0];
            map_entries[map_count++] = entry;
        } else if (vsel_type == H5S_SEL_HYPERSLABS || vsel_type == H5S_SEL_POINTS) {
            // Get all hyperblocks for virtual selection
            hsize_t nblocks = H5Sget_select_hyper_nblocks(vspace_id);
            if (nblocks < 1) {
                // No blocks (empty selection)
                H5Sclose(vspace_id);
                H5Sclose(src_space_id);
                free(src_filename);
                free(src_dsetname);
                continue;
            }
            hsize_t* vblocks = malloc(nblocks * 2 * ndims * sizeof(hsize_t));
            H5Sget_select_hyper_blocklist(vspace_id, 0, nblocks, vblocks);

            // Get hyperblocks for source selection if available
            hsize_t* sblocks = NULL;
            hsize_t nsrc_blocks = 0;
            if (ssel_type == H5S_SEL_ALL) {
                // Treat entire source as one block
                hsize_t src_dims[H5S_MAX_RANK];
                H5Sget_simple_extent_dims(src_space_id, src_dims, NULL);
                nsrc_blocks = 1;
                sblocks = malloc(2 * ndims * sizeof(hsize_t));
                // start coords = all zeros, end coords = dims-1
                for (int d = 0; d < ndims; ++d) {
                    sblocks[d] = 0;
                    sblocks[ndims + d] = (src_dims[d] > 0) ? src_dims[d] - 1 : 0;
                }
            } else {
                nsrc_blocks = H5Sget_select_hyper_nblocks(src_space_id);
                sblocks = malloc(nsrc_blocks * 2 * ndims * sizeof(hsize_t));
                H5Sget_select_hyper_blocklist(src_space_id, 0, nsrc_blocks, sblocks);
            }

            // There should be a corresponding source block for each virtual block (assuming valid
            // mapping). We'll iterate through blocks and create an entry for each.
            for (hsize_t b = 0; b < nblocks; ++b) {
                // Virtual block start and end
                hsize_t* v_start = &vblocks[b * 2 * ndims];
                hsize_t* v_end = &vblocks[b * 2 * ndims + ndims];
                // Find the matching source block (in most cases b < nsrc_blocks)
                // If source has one block (nsrc_blocks == 1) for multiple virtual blocks, it might
                // be a repeated pattern.
                hsize_t* s_start = (b < nsrc_blocks ? &sblocks[b * 2 * ndims] : &sblocks[0]);
                hsize_t* s_end =
                    (b < nsrc_blocks ? &sblocks[b * 2 * ndims + ndims] : &sblocks[ndims]);

                FrameMapEntry entry;
                entry.v_start = v_start[0];
                entry.v_end = v_end[0];
                entry.src_file = strdup(src_filename);
                entry.src_dset = strdup(src_dsetname);
                entry.src_start = s_start[0];
                // (We could also store entry.src_end = s_end[0] if needed)
                map_entries[map_count++] = entry;
            }

            free(vblocks);
            free(sblocks);
        }

        // Close the dataspaces and free name buffers for this mapping
        H5Sclose(vspace_id);
        H5Sclose(src_space_id);
        free(src_filename);
        free(src_dsetname);
    }

    // Optionally: sort map_entries by v_start (to ensure order)
    // Simple bubble sort or qsort could be used; assuming map_entries are not huge:
    for (size_t a = 0; a < map_count; ++a) {
        for (size_t b = a + 1; b < map_count; ++b) {
            if (map_entries[b].v_start < map_entries[a].v_start) {
                FrameMapEntry temp = map_entries[a];
                map_entries[a] = map_entries[b];
                map_entries[b] = temp;
            }
        }
    }

    // Detect gaps and overlaps
    if (map_count > 0) {
        // If first mapped frame is > 0, then [0, v_start-1] is a gap at the beginning
        if (map_entries[0].v_start > 0) {
            printf("Gap: frames [0 .. %llu] have no source (filled with fill value)\n",
                   map_entries[0].v_start - 1);
        }
        for (size_t idx = 0; idx < map_count - 1; ++idx) {
            hsize_t this_end = map_entries[idx].v_end;
            hsize_t next_start = map_entries[idx + 1].v_start;
            if (this_end + 1 < next_start) {
                // Gap between end of this segment and start of next
                printf("Gap: frames [%llu .. %llu] have no source (fill value)\n", this_end + 1,
                       next_start - 1);
            }
            if (this_end >= next_start) {
                // Overlap detected (shouldn't happen in valid VDS)
                printf("Warning: overlap between mappings at frames %llu and %llu\n", next_start,
                       this_end);
            }
        }
    }

    // Example: use the mapping to find a specific frame's source
    hsize_t query_frame = 10;  // example global frame index
    FrameMapEntry* found = NULL;
    for (size_t j = 0; j < map_count; ++j) {
        if (query_frame >= map_entries[j].v_start && query_frame <= map_entries[j].v_end) {
            found = &map_entries[j];
            break;
        }
    }
    if (found) {
        hsize_t offset = query_frame - found->v_start;
        hsize_t src_frame_index = found->src_start + offset;
        printf("Frame %llu is in file \"%s\" dataset \"%s\" at frame index %llu\n", query_frame,
               found->src_file, found->src_dset, src_frame_index);
        // Here you could open found->src_file and read frame src_frame_index from found->src_dset.
    } else {
        printf("Frame %llu is not covered by any source (gap/fill)\n", query_frame);
    }

    // Cleanup
    for (size_t k = 0; k < map_count; ++k) {
        free(map_entries[k].src_file);
        free(map_entries[k].src_dset);
    }
    free(map_entries);
    H5Pclose(dcpl_id);
    H5Dclose(dset_id);
    H5Fclose(file_id);
    return 0;
}
