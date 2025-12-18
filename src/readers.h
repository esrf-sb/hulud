#ifndef READERS_H
#define READERS_H

#include "file.h"

/* HDF5-based reader for mapped frames. */
int get_virtual_frame(const struct ds_desc_t* d, const int f, void* buf);
/* Build fast chunk offsets for parallel reading. */
int build_parallel_index(struct opt_eiger_ds_desc_t* opt);
/* Thread-safe raw reader using precomputed offsets. */
int get_virtual_frame_parallel(const struct ds_desc_t* d, const int f, void* buf);

#endif /* READERS_H */
