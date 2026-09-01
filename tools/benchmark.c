#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <durin/plugin_api.h>

/* --- 2. Global Symbol Pointers --- */
static plugin_open_fn ptr_plugin_open = NULL;
static plugin_get_header_fn ptr_plugin_get_header = NULL;
static plugin_get_data_fn ptr_plugin_get_data = NULL;
static plugin_close_fn ptr_plugin_close = NULL;

/* --- 3. Helpers --- */
double get_time() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec * 1e-6;
}

void* load_symbol(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym) {
        fprintf(stderr, "Error resolving symbol '%s': %s\n", name, dlerror());
        exit(1);
    }
    return sym;
}

/* FNV-1a 64-bit, seeded with the requested frame index. Any change to the
 * frame content OR a mismatch between the requested and returned frame
 * (e.g. frame 1 and 3 swapped) changes the resulting hash. */
static uint64_t hash_frame(int frame_number, const int* buffer, size_t frame_px) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV offset basis */
    const uint8_t* seed_bytes = (const uint8_t*)&frame_number;
    for (size_t i = 0; i < sizeof(frame_number); ++i) {
        h ^= seed_bytes[i];
        h *= 0x100000001b3ULL; /* FNV prime */
    }
    const uint8_t* p = (const uint8_t*)buffer;
    size_t len = frame_px * sizeof(int);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

struct worker_ctx {
    int nx, ny, nbytes;
    int total_frames;
    int* info;
    pthread_mutex_t* frame_mutex;
    int* next_frame;
    uint64_t checksum;
    int empty_frames; /* Counter for empty frames */
    int err;
};

/* --- 4. Parallel Worker --- */
static void* worker_run(void* arg) {
    struct worker_ctx* ctx = arg;
    size_t frame_px = (size_t)ctx->nx * (size_t)ctx->ny;

    /* Allocate buffer per thread */
    int* buffer = malloc(frame_px * sizeof(int));
    if (!buffer) {
        ctx->err = -1;
        return NULL;
    }

    while (1) {
        int frame = -1;

        /* Grab next frame index */
        pthread_mutex_lock(ctx->frame_mutex);
        if (*ctx->next_frame < ctx->total_frames) {
            frame = (*ctx->next_frame)++;
        }
        pthread_mutex_unlock(ctx->frame_mutex);

        if (frame < 0) break;  // No more work

        int fn = frame + 1; /* Plugin expects 1-based index */
        int nx = ctx->nx, ny = ctx->ny;
        int local_err = 0;

        /* CALL THE PLUGIN VIA POINTER */
        ptr_plugin_get_data(&fn, &nx, &ny, buffer, ctx->info, &local_err);

        if (local_err < 0) {
            ctx->err = local_err;
            break;
        }

        /* Verify Data: Calculate sum for THIS frame */
        uint64_t frame_sum = 0;
        for (size_t j = 0; j < frame_px; ++j) {
            frame_sum += buffer[j];
        }

        /* Check for empty frame (all zeros) */
        if (frame_sum == 0) {
            ctx->empty_frames++;
            // Optional: Print debug info (careful with threading/spam)
            // fprintf(stderr, "[Thread] Warning: Frame %d is empty (sum=0)\n", fn);
        }

        ctx->checksum ^= hash_frame(fn, buffer, frame_px);
    }

    free(buffer);
    return NULL;
}

/* --- 5. Main --- */
int main(int argc, char** argv) {
    char* h5_filename = NULL;
    char* lib_filename = "./build/durin-plugin.so"; /* Default */
    int limit_frames = -1;
    int threads = 16;
    int disable_masking = 0;
    int debug_mode = -1; /* -1 = inherit env, 0 = force off, 1 = force on */

    /* Parse Arguments */
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--frames") == 0) && (i + 1) < argc) {
            limit_frames = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) &&
                   (i + 1) < argc) {
            threads = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--lib") == 0) &&
                   (i + 1) < argc) {
            lib_filename = argv[++i];
        } else if (strcmp(argv[i], "--no-mask") == 0) {
            disable_masking = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--no-debug") == 0) {
            debug_mode = 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s <master_file.h5> [options]\n", argv[0]);
            printf("  -l, --lib <path>   Path to .so plugin (default: ./durin-plugin.so)\n");
            printf("  -n, --frames <N>   Limit number of frames to read\n");
            printf("  -t, --threads <N>  Threads for parallel test\n");
            printf("      --no-mask      Disable masking/casting (fast path)\n");
            printf("      --debug        Enable DURIN_DEBUG for verbose plugin logs\n");
            printf("      --no-debug     Disable DURIN_DEBUG\n");
            return 0;
        } else if (!h5_filename) {
            h5_filename = argv[i];
        } else {
            printf("Unrecognised argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (!h5_filename) {
        fprintf(stderr, "Error: No HDF5 file provided.\n");
        return 1;
    }

    if (disable_masking) {
        setenv("DURIN_DISABLE_MASKING", "1", 1);
    }
    if (debug_mode == 1) {
        setenv("DURIN_DEBUG", "1", 1);
    } else if (debug_mode == 0) {
        setenv("DURIN_DEBUG", "0", 1);
    }

    /* --- Load Library --- */
    printf("Loading plugin: %s\n", lib_filename);
    void* handle = dlopen(lib_filename, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    /* --- Resolve Symbols --- */
    ptr_plugin_open = (plugin_open_fn)load_symbol(handle, "plugin_open");
    ptr_plugin_get_header = (plugin_get_header_fn)load_symbol(handle, "plugin_get_header");
    ptr_plugin_get_data = (plugin_get_data_fn)load_symbol(handle, "plugin_get_data");
    ptr_plugin_close = (plugin_close_fn)load_symbol(handle, "plugin_close");

    int info[DURIN_PLUGIN_INFO_SIZE] = {0};
    int err = 0;

    /* --- Open File --- */
    printf("Opening HDF5: %s\n", h5_filename);
    double t_open_start = get_time();
    ptr_plugin_open(h5_filename, info, &err);
    double t_open_end = get_time();

    if (err < 0) {
        fprintf(stderr, "Plugin failed to open file.\n");
        return 1;
    }

    int nx = 0, ny = 0, nbytes = 0, nframes = 0;
    float qx = 0, qy = 0;
    ptr_plugin_get_header(&nx, &ny, &nbytes, &qx, &qy, &nframes, info, &err);
    if (err < 0) {
        fprintf(stderr, "Plugin failed to get header.\n");
        return 1;
    }

    int mapped_frames = info[5] > 0 ? info[5] : 0;
    printf("Metadata Read: %.4fs | Frames: %d", t_open_end - t_open_start, nframes);
    if (mapped_frames > 0 && mapped_frames != nframes) {
        printf(" (mapped: %d)", mapped_frames);
    }
    printf(" | Size: %d x %d | Bytes: %d\n", nx, ny, nbytes);

    int default_frames = mapped_frames > 0 ? mapped_frames : nframes;
    int total_frames = (limit_frames > 0 && limit_frames < default_frames) ? limit_frames
                                                                           : default_frames;
    size_t frame_size_bytes = (size_t)nx * (size_t)ny * (size_t)nbytes;
    size_t frame_px = (size_t)nx * (size_t)ny;

    /* ------------------------------------------------------------------
     * TEST 1: SINGLE THREAD (Sequential)
     * ------------------------------------------------------------------ */
    {
        printf("\n[Test 1] Single Thread (Main Thread Only)...\n");
        int* buffer = malloc(frame_px * sizeof(int));
        uint64_t checksum = 0;
        int empty_count = 0;
        double t_start = get_time();

        for (int i = 0; i < total_frames; i++) {
            int fn = i + 1;
            int l_nx = nx, l_ny = ny;
            ptr_plugin_get_data(&fn, &l_nx, &l_ny, buffer, info, &err);
            if (err < 0) {
                fprintf(stderr, "Error reading frame %d\n", fn);
                break;
            }

            uint64_t frame_sum = 0;
            for (size_t j = 0; j < frame_px; ++j) frame_sum += buffer[j];

            if (frame_sum == 0) {
                empty_count++;
                fprintf(stderr, "  WARNING: Frame %d is empty (sum=0)\n", fn);
            }
            checksum ^= hash_frame(fn, buffer, frame_px);
        }

        double t_end = get_time();
        double dt = t_end - t_start;
        double fps = total_frames / dt;
        double mbps = (total_frames * frame_size_bytes) / (1024.0 * 1024.0 * dt);

        printf("  Time:         %.4f s\n", dt);
        printf("  Speed:        %.2f FPS\n", fps);
        printf("  Throughput:   %.2f MB/s\n", mbps);
        printf("  Hash:         0x%016lx\n", checksum);
        printf("  Empty Frames: %d\n", empty_count);
        free(buffer);
    }

    /* ------------------------------------------------------------------
     * TEST 2: MULTI THREAD (Parallel Workers)
     * ------------------------------------------------------------------ */
    if (threads > 1) {
        printf("\n[Test 2] Multi Thread (%d threads)...\n", threads);

        pthread_t* tids = calloc(threads, sizeof(pthread_t));
        struct worker_ctx* wctx = calloc(threads, sizeof(struct worker_ctx));
        pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
        int shared_next_frame = 0;
        uint64_t checksum_total = 0;
        int empty_frames_total = 0;

        double t_start = get_time();

        for (int i = 0; i < threads; ++i) {
            wctx[i].nx = nx;
            wctx[i].ny = ny;
            wctx[i].nbytes = nbytes;
            wctx[i].total_frames = total_frames;
            wctx[i].info = info;
            wctx[i].frame_mutex = &frame_mutex;
            wctx[i].next_frame = &shared_next_frame;
            wctx[i].checksum = 0;
            wctx[i].empty_frames = 0;
            wctx[i].err = 0;
            pthread_create(&tids[i], NULL, worker_run, &wctx[i]);
        }

        for (int i = 0; i < threads; ++i) {
            pthread_join(tids[i], NULL);
            checksum_total ^= wctx[i].checksum;
            empty_frames_total += wctx[i].empty_frames;
            if (wctx[i].err < 0) err = wctx[i].err;
        }

        double t_end = get_time();
        double dt = t_end - t_start;
        double fps = total_frames / dt;
        double mbps = (total_frames * frame_size_bytes) / (1024.0 * 1024.0 * dt);

        printf("  Time:         %.4f s\n", dt);
        printf("  Speed:        %.2f FPS\n", fps);
        printf("  Throughput:   %.2f MB/s\n", mbps);
        printf("  Hash:         0x%016lx\n", checksum_total);
        printf("  Empty Frames: %d\n", empty_frames_total);

        if (empty_frames_total > 0) {
            printf("  !! WARNING: DETECTED %d EMPTY FRAMES !!\n", empty_frames_total);
        }

        free(tids);
        free(wctx);
    }

    /* --- Cleanup --- */
    ptr_plugin_close(&err);
    dlclose(handle);

    return 0;
}
