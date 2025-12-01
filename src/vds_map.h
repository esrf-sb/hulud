#ifndef VDS_MAP_H
#define VDS_MAP_H

#include "file.h"

int build_vds_mapping(hid_t ds_id, const char* master_dir, struct opt_eiger_ds_desc_t* opt,
                      struct eiger_ds_desc_t* eiger_desc_to_free);

#endif /* VDS_MAP_H */
