#ifndef READERS_H
#define READERS_H

#include "file.h"

int get_virtual_frame(const struct ds_desc_t* d, const int f, void* buf);
int build_parallel_index(struct opt_eiger_ds_desc_t* opt);
int get_virtual_frame_parallel(const struct ds_desc_t* d, const int f, void* buf);

#endif /* READERS_H */
