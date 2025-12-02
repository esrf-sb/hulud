#define _POSIX_C_SOURCE 200112L

#include <hdf5.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "err.h"
#include "plugin.h"

double get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec * 1e-6;
}

struct worker_ctx {
    int nx, ny, nbytes;
    int total_frames;
    int* info;
    pthread_mutex_t* frame_mutex;
    int* next_frame;
    uint64_t checksum;
    int err;
};

static void* worker_run(void* arg) {
    struct worker_ctx* ctx = arg;
    size_t frame_px = (size_t)ctx->nx * (size_t)ctx->ny;
    int* buffer = malloc(frame_px * sizeof(int));
    if (!buffer) {
        ctx->err = -1;
        return NULL;
    }

    while (1) {
        int frame = -1;
        pthread_mutex_lock(ctx->frame_mutex);
        if (*ctx->next_frame < ctx->total_frames) {
            frame = (*ctx->next_frame)++;
        }
        pthread_mutex_unlock(ctx->frame_mutex);
        if (frame < 0) break;

        int fn = frame + 1; /* plugin expects 1-based */
        int nx = ctx->nx, ny = ctx->ny;
        int local_err = 0;
        plugin_get_data(&fn, &nx, &ny, buffer, ctx->info, &local_err);

        if (local_err < 0) {
            ctx->err = local_err;
            break;
        }
        for (size_t j = 0; j < frame_px; ++j) ctx->checksum += buffer[j];
    }

    free(buffer);
    return NULL;
}

int main(int argc, char** argv) {
    char* filename = NULL;
    int limit_frames = -1;
    int threads = 16;
    int disable_masking = 0;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--frames") == 0) && (i + 1) < argc) {
            limit_frames = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) &&
                   (i + 1) < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-mask") == 0) {
            disable_masking = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s <master_file.h5> [options]\n", argv[0]);
            printf("  -n, --frames <N>   Limit number of frames to read\n");
            printf(
                "  -t, --threads <N>  Threads for parallel test (1 disables parallel section)\n");
            printf("      --no-mask      Disable masking/casting for benchmarking\n");
            return 0;
        } else if (!filename) {
            filename = argv[i];
        } else {
            printf("Unrecognised argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!filename) {
        printf("Usage: %s <master_file.h5> [options]\n", argv[0]);
        return 1;
    }

    if (threads < 1) threads = 1;
    if (disable_masking) {
        /* propagate to plugin to skip masking/casting work */
        setenv("DURIN_DISABLE_MASKING", "1", 1);
    }

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

    // 5. Benchmark Loop
    int total_frames = nframes;
    if (limit_frames > 0 && limit_frames < total_frames) {
        total_frames = limit_frames;
    }

    if (threads > 1) {
        printf("Parallel benchmark of %d frames with %d threads...\n", total_frames, threads);
        pthread_t* tids = calloc((size_t)threads, sizeof(pthread_t));
        struct worker_ctx* wctx = calloc((size_t)threads, sizeof(struct worker_ctx));
        pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
        int shared_next = 0;
        uint64_t checksum_p = 0;
        double tp_start = get_time();

        if (!tids || !wctx) {
            fprintf(stderr, "Failed to allocate worker structures\n");
            plugin_close(&err);
            free(tids);
            free(wctx);
            return -1;
        }

        for (int i = 0; i < threads; ++i) {
            wctx[i].nx = nx;
            wctx[i].ny = ny;
            wctx[i].nbytes = nbytes;
            wctx[i].total_frames = total_frames;
            wctx[i].info = info;
            wctx[i].frame_mutex = &frame_mutex;
            wctx[i].next_frame = &shared_next;
            wctx[i].checksum = 0;
            wctx[i].err = 0;
            pthread_create(&tids[i], NULL, worker_run, &wctx[i]);
        }
        for (int i = 0; i < threads; ++i) pthread_join(tids[i], NULL);
        double tp_end = get_time();
        for (int i = 0; i < threads; ++i) {
            checksum_p += wctx[i].checksum;
            if (wctx[i].err < 0) err = wctx[i].err;
        }

        double tp_elapsed = tp_end - tp_start;
        double fps_p = total_frames / tp_elapsed;
        double thr_p = (total_frames * frame_size_bytes) / (1024.0 * 1024.0 * tp_elapsed);

        printf("\n--- Results (%d threads) ---\n", threads);
        if (err < 0) {
            printf("Parallel run encountered an error\n");
        }
        printf("Time:       %.4f s\n", tp_elapsed);
        printf("Frames:     %d\n", total_frames);
        printf("Speed:      %.2f FPS\n", fps_p);
        printf("Throughput: %.2f MB/s\n", thr_p);
        printf("Checksum:   0x%016llx\n", (unsigned long long)checksum_p);

        free(tids);
        free(wctx);
    } else {
        printf("Single-thread benchmark of %d frames...\n", total_frames);
        int* buffer = malloc(frame_px * sizeof(int));
        if (!buffer) {
            fprintf(stderr, "Failed to allocate buffer (%zu ints)\n", frame_px);
            plugin_close(&err);
            return -1;
        }

        uint64_t checksum = 0;
        double t_start = get_time();

        for (int i = 0; i < total_frames; i++) {
            int frame_number = i + 1; /* plugin expects 1-based */
            plugin_get_data(&frame_number, &nx, &ny, buffer, info, &err);
            if (err < 0) {
                fprintf(stderr, "Error reading frame %d via plugin\n", i);
                dump_error_stack(stderr);
                break;
            }

            for (size_t j = 0; j < frame_px; ++j) checksum += buffer[j];
        }

        double t_end = get_time();
        double total_time = t_end - t_start;
        double fps = total_frames / total_time;
        double throughput = (total_frames * frame_size_bytes) / (1024.0 * 1024.0 * total_time);

        printf("\n--- Results (single-thread) ---\n");
        printf("Time:       %.4f s\n", total_time);
        printf("Frames:     %d\n", total_frames);
        printf("Speed:      %.2f FPS\n", fps);
        printf("Throughput: %.2f MB/s\n", throughput);
        printf("Checksum:   0x%016llx\n", (unsigned long long)checksum);

        free(buffer);
    }

    plugin_close(&err);

    return 0;
}
