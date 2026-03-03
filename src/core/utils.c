#define _GNU_SOURCE
#include "core/internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include <strings.h>

char* dup_cstr(const char* s) {
    size_t len = strlen(s) + 1;
    char* out = malloc(len);
    if (!out) return NULL;
    memcpy(out, s, len);
    return out;
}

void get_object_path(hid_t id, char* buf, size_t buflen) {
    if (!buf || buflen == 0) return;
    if (H5Iget_name(id, buf, buflen) < 0) buf[0] = '\0';
}

int link_is_dataset(hid_t loc, const char* name) {
    hid_t d = -1;
    char gpath[MAX_PATH_LEN] = {0};
    get_object_path(loc, gpath, sizeof(gpath));
    H5E_BEGIN_TRY { d = H5Dopen2(loc, name, H5P_DEFAULT); }
    H5E_END_TRY;
    if (durin_debug_enabled) {
        fprintf(stderr, "[DURIN] link_is_dataset: %s/%s => %s\n", gpath[0] ? gpath : "(root)", name,
                (d >= 0 ? "dataset" : "missing"));
    }
    if (d < 0) return 0;
    H5Dclose(d);
    return 1;
}

char* canonical_file_path(hid_t fid, const char* base_dir) {
    char raw[MAX_PATH_LEN] = {0};
    char resolved[PATH_MAX] = {0};
    ssize_t len = H5Fget_name(fid, raw, sizeof(raw));

    /* 1) Absolute path already provided */
    if (len > 0 && raw[0] == '/') {
        if (realpath(raw, resolved)) return dup_cstr(resolved);
        DEBUGF("canonical_file_path: absolute path '%s'\n", resolved[0] ? resolved : raw);
        return dup_cstr(resolved[0] ? resolved : raw);
    }

    /* 2) Try resolving the raw string relative to cwd */
    if (len > 0 && realpath(raw, resolved)) {
        DEBUGF("canonical_file_path: cwd-resolved '%s'\n", resolved);
        return dup_cstr(resolved);
    }

    /* 3) If relative and we know the base directory of the master file, use it */
    if (len > 0 && base_dir && base_dir[0]) {
        char combined[PATH_MAX] = {0};
        size_t len_base = strnlen(base_dir, sizeof(combined) - 1);
        size_t len_raw = strnlen(raw, sizeof(combined) - 1);
        if (len_base + len_raw + 1 < sizeof(combined)) {
            char* dst = combined;
            memcpy(dst, base_dir, len_base);
            dst += len_base;
            if (len_base > 0 && base_dir[len_base - 1] != '/') {
                *dst++ = '/';
            }
            memcpy(dst, raw, len_raw);
            dst[len_raw] = '\0';
            if (realpath(combined, resolved)) {
                DEBUGF("canonical_file_path: base_dir '%s' + '%s' => '%s'\n", base_dir, raw,
                       resolved);
                return dup_cstr(resolved);
            }
        }
    }

#ifdef __linux__
    /* 4) Last resort: ask the kernel for the path of the open file descriptor */
    void* vfd_handle = NULL;
    if (H5Fget_vfd_handle(fid, H5P_DEFAULT, &vfd_handle) >= 0 && vfd_handle) {
        int fd = *(int*)vfd_handle;
        char proc_path[64] = {0};
        snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
        if (realpath(proc_path, resolved)) {
            DEBUGF("canonical_file_path: procfd '%s'\n", resolved);
            return dup_cstr(resolved);
        }
    }
#endif

    /* Fall back to whatever we have */
    DEBUGF("canonical_file_path: fallback to raw path '%s'\n", raw);
    return dup_cstr(raw);
}

double scale_from_units(const char* unit_string) {
    if (!unit_string) return 1.0;

    if (strcasecmp("m", unit_string) == 0 || strcasecmp("metres", unit_string) == 0 ||
        strcasecmp("meters", unit_string) == 0) {
        return 1.0;
    } else if (strcasecmp("cm", unit_string) == 0 || strcasecmp("centimetres", unit_string) == 0 ||
               strcasecmp("centimeters", unit_string) == 0) {
        return 0.01;
    } else if (strcasecmp("mm", unit_string) == 0 || strcasecmp("millimetres", unit_string) == 0 ||
               strcasecmp("millimeters", unit_string) == 0) {
        return 0.001;
    } else if (strcasecmp("um", unit_string) == 0 || strcasecmp("microns", unit_string) == 0 ||
               strcasecmp("micrometres", unit_string) == 0 ||
               strcasecmp("micrometers", unit_string) == 0) {
        return 0.000001;
    } else {
        fprintf(stderr, "Unrecognised unit string %s\n", unit_string);
        return 1.0;
    }
}