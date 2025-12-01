#include <hdf5.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "err.h"
#include "plugin.h"

double get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec * 1e-6;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <master_file.h5> [num_frames_to_read]\n", argv[0]);
        return 1;
    }

    char* filename = argv[1];
    int limit_frames = (argc >= 3) ? atoi(argv[2]) : -1;

    int info[1024] = {0};
    int err = 0;

    init_error_handling();
    if (init_h5_error_handling() < 0) {
        fprintf(stderr, "Failed to init HDF5 error handling\n");
        return -1;
    }

    printf("Opening %s...\n", filename);
    double t_open_start = get_time();
    plugin_open(filename, info, &err);
    double t_open_end = get_time();
    if (err < 0) {
        fprintf(stderr, "Error opening file via plugin\n");
        dump_error_stack(stderr);
        return -1;
    }

    int nx = 0, ny = 0, nbytes = 0, nframes = 0;
    float qx = 0, qy = 0;
    plugin_get_header(&nx, &ny, &nbytes, &qx, &qy, &nframes, info, &err);
    if (err < 0) {
        fprintf(stderr, "Error getting header via plugin\n");
        dump_error_stack(stderr);
        return -1;
    }

    fprintf(stderr, "[DURIN] benchmark: dims=%d x %d x %d, data_width=%d\n", nframes, ny, nx,
            nbytes);

    printf("Metadata Read Time: %.4f s\n", t_open_end - t_open_start);
    printf("Dataset: %d frames, %d x %d pixels\n", nframes, ny, nx);

    size_t frame_size_bytes = (size_t)nx * (size_t)ny * (size_t)nbytes;
    size_t frame_px = (size_t)nx * (size_t)ny;
    int* buffer = malloc(frame_px * sizeof(int));
    if (!buffer) {
        fprintf(stderr, "Failed to allocate buffer (%zu ints)\n", frame_px);
        return -1;
    }

    // 5. Benchmark Loop
    int total_frames = nframes;
    if (limit_frames > 0 && limit_frames < total_frames) {
        total_frames = limit_frames;
    }

    printf("Benchmarking read of %d frames...\n", total_frames);
    /* simple checksum to force touching the data (verifies real reads) */
    uint64_t checksum = 0;

    double t_start = get_time();

    for (int i = 0; i < total_frames; i++) {
        // This is the core function durin uses to fetch a specific frame
        // fprintf(stderr,"[DEBUG] plugin_get_data: reading frame %d/%d\n",i, (int)(total_frames -
        // 1));
        int frame_number = i + 1; /* plugin expects 1-based */
        plugin_get_data(&frame_number, &nx, &ny, buffer, info, &err);
        if (err < 0) {
            fprintf(stderr, "Error reading frame %d via plugin\n", i);
            dump_error_stack(stderr);
            break;
        }

        /* Accumulate a checksum over the frame to ensure data is actually read */
        for (size_t j = 0; j < frame_px; ++j) checksum += buffer[j];
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
    printf("Checksum:   0x%016llx\n", (unsigned long long)checksum);

    free(buffer);
    plugin_close(&err);

    return 0;
}
