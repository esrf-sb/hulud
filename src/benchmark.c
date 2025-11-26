#include "err.h"    // Changed from "src/err.h"
#include "file.h"   // Changed from "src/file.h"
#include <hdf5.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

double get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec * 1e-6;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <master_file.h5> [num_frames_to_read]\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];
    int limit_frames = (argc >= 3) ? atoi(argv[2]) : -1;

    struct ds_desc_t *desc;
    hid_t fid = 0;
    void *buffer = NULL;
    int err = 0;
    
    init_error_handling();
    if (init_h5_error_handling() < 0) {
        fprintf(stderr, "Failed to init HDF5 error handling\n");
        return -1;
    }

    printf("Opening %s...\n", filename);
    fid = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) {
        fprintf(stderr, "Error opening file\n");
        return -1;
    }

    // 3. Get Detector Info (Parses VDS / Master file)
    double t_open_start = get_time();
    err = get_detector_info(fid, &desc);
    double t_open_end = get_time();
    
    if (err < 0) {
        fprintf(stderr, "Error getting detector info\n");
        dump_error_stack(stderr);
        return -1;
    }

    printf("Metadata Read Time: %.4f s\n", t_open_end - t_open_start);
    printf("Dataset: %llu frames, %llu x %llu pixels\n", 
           desc->dims[0], desc->dims[1], desc->dims[2]);

    // 4. Allocate Buffer (Single frame buffer)
    size_t frame_size_bytes = desc->dims[1] * desc->dims[2] * desc->data_width;
    buffer = malloc(frame_size_bytes);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer (%lu bytes)\n", frame_size_bytes);
        return -1;
    }

    // 5. Benchmark Loop
    int total_frames = desc->dims[0];
    if (limit_frames > 0 && limit_frames < total_frames) {
        total_frames = limit_frames;
    }

    printf("Benchmarking read of %d frames...\n", total_frames);
    
    double t_start = get_time();
    
    for (int i = 0; i < total_frames; i++) {
        // This is the core function durin uses to fetch a specific frame
        err = desc->get_data_frame(desc, i, buffer);
        if (err < 0) {
            fprintf(stderr, "Error reading frame %d\n", i);
            dump_error_stack(stderr);
            break;
        }
    }

    double t_end = get_time();
    double total_time = t_end - t_start;
    double fps = total_frames / total_time;
    double throughput = (total_frames * frame_size_bytes) / (1024.0 * 1024.0 * total_time);

    printf("\n--- Results ---\n");
    printf("Time:       %.4f s\n", total_time);
    printf("Frames:     %d\n", total_frames);
    printf("Speed:      %.2f FPS\n", fps);
    printf("Throughput: %.2f MB/s\n", throughput);

    free(buffer);
    if (desc->free_desc) desc->free_desc(desc);
    H5Fclose(fid);

    return 0;
}