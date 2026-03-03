#ifndef VDS_MAP_H
#define VDS_MAP_H

#include "io/file.h"

int build_vds_mapping(hid_t ds_id, const char* master_dir, struct opt_pilatus_ds_desc_t* opt,
                      struct pilatus_ds_desc_t* pilatus_desc_to_free);

#endif /* VDS_MAP_H */
