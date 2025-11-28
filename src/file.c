/*
 * Copyright (c) 2018 Diamond Light Source Ltd.
 * Author: Charles Mita
 */

#include <hdf5.h>
#include <hdf5_hl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "err.h"
#include "file.h"
#include "filters.h"

/* --- GPFS/LIMA2 VDS OPTIMIZATION ---
 * Static cache to keep the Virtual Dataset (VDS) handle open.
 * * In Lima2, "data_00001" is a VDS pointing to multiple raw files 
 * (raw_data_1.h5, raw_data_2.h5, etc.).
 * * Opening this VDS is expensive on GPFS because HDF5 must stat() 
 * all the underlying raw files. By caching the handle, we pay this 
 * cost only once per block, instead of once per frame.
 */
static hid_t CACHED_DID = 0; // Stores the open HDF5 Dataset ID
static char CACHED_DS_NAME[256] = {0}; // Stores the name (e.g., "data_000001")


// For sorting mapping entries
static int compare_frame_map(const void *a, const void *b) {
    const struct FrameMapEntry *A = a;
    const struct FrameMapEntry *B = b;
    return (A->vds_start > B->vds_start) - (A->vds_start < B->vds_start);
}

static int get_virtual_frame(const struct ds_desc_t *, const int, void *);

void reset_dataset_cache() {
    if (CACHED_DID > 0) {
        H5Dclose(CACHED_DID);
        CACHED_DID = 0;
    }
    memset(CACHED_DS_NAME, 0, 256);
}
/* --- END OPTIMIZATION --- */

void clear_det_visit_objects(struct det_visit_objects_t *objects) {
  if (objects->nxdata) {
    H5Oclose(objects->nxdata);
    objects->nxdata = 0;
  }
  if (objects->nxdetector) {
    H5Oclose(objects->nxdetector);
    objects->nxdetector = 0;
  }
}

void free_ds_desc(struct ds_desc_t *desc) {
  H5Gclose(desc->det_g_id);
  H5Gclose(desc->data_g_id);
  free(desc);
}

void free_nxs_desc(struct ds_desc_t *desc) { free_ds_desc(desc); }

void free_eiger_desc(struct ds_desc_t *desc) {
  struct eiger_ds_desc_t *e_desc = (struct eiger_ds_desc_t *)desc;
  free(e_desc->block_sizes);
  free_ds_desc(desc);
}

void free_opt_eiger_desc(struct ds_desc_t *desc)
{
    struct opt_eiger_ds_desc_t *opt_desc =
        (struct opt_eiger_ds_desc_t*)desc;

    reset_dataset_cache();

    /* First: cleanup VDS mapping resources */
    if (opt_desc->frame_map_entries) {
        for (int i = 0; i < opt_desc->n_map_entries; ++i) {
            struct FrameMapEntry *E = &opt_desc->frame_map_entries[i];
            if (E->src_dset_id > 0)
                H5Dclose(E->src_dset_id);
            if (E->src_file_id > 0)
                H5Fclose(E->src_file_id);
            free(E->src_file);
            free(E->src_dset);
        }
        free(opt_desc->frame_map_entries);
    }

    /* If chunk path allocated block_sizes array */
    if (opt_desc->base.block_sizes)
        free(opt_desc->base.block_sizes);

    /* Finally destroy the base descriptor and close HDF5 groups */
    free_ds_desc(desc);
}

double scale_from_units(const char *unit_string) {
  if (strcasecmp("m", unit_string) == 0 ||
      strcasecmp("metres", unit_string) == 0 ||
      strcasecmp("meters", unit_string) == 0) {
    return 1.;
  } else if (strcasecmp("cm", unit_string) == 0 ||
             strcasecmp("centimetres", unit_string) == 0 ||
             strcasecmp("centimeters", unit_string) == 0) {
    return 0.01;
  } else if (strcasecmp("mm", unit_string) == 0 ||
             strcasecmp("millimetres", unit_string) == 0 ||
             strcasecmp("millimeters", unit_string) == 0) {
    return 0.001;
  } else if (strcasecmp("um", unit_string) == 0 ||
             strcasecmp("microns", unit_string) == 0 ||
             strcasecmp("micrometres", unit_string) == 0 ||
             strcasecmp("micrometers", unit_string) == 0) {
    return 0.000001;
  } else {
    fprintf(stderr, "Unrecognised unit string %s", unit_string);
    return 1;
  }
}

int get_nxs_dataset_dims(struct ds_desc_t *desc) {
  hid_t g_id, ds_id, s_id, t_id;
  int retval = 0;
  int ndims = 0;
  int width = 0;
  g_id = desc->data_g_id;
  ;

  ds_id = H5Dopen2(g_id, "data", H5P_DEFAULT);
  if (ds_id <= 0) {
    ERROR_JUMP(-1, done, "Unable to open 'data' dataset");
  }

  t_id = H5Dget_type(ds_id);
  if (t_id <= 0) {
    ERROR_JUMP(-1, close_dataset, "Error getting datatype");
  }

  width = H5Tget_size(t_id);
  if (width <= 0) {
    ERROR_JUMP(-1, close_type, "Error getting type size");
  }

  s_id = H5Dget_space(ds_id);
  if (s_id <= 0) {
    ERROR_JUMP(-1, close_dataset, "Error getting dataspace");
  }

  ndims = H5Sget_simple_extent_ndims(s_id);
  if (ndims != 3) {
    char message[64];
    sprintf(message, "Dataset rank is %d, expected %d", ndims, 3);
    ERROR_JUMP(-1, close_space, message);
  }

  if (H5Sget_simple_extent_dims(s_id, desc->dims, NULL) < 0) {
    ERROR_JUMP(-1, close_space, "Error getting dataset dimensions");
  }

  desc->data_width = width;

close_space:
  H5Sclose(s_id);
close_type:
  H5Tclose(t_id);
close_dataset:
  H5Dclose(ds_id);
done:
  return retval;
}

int get_frame_simple(const struct ds_desc_t *desc, const char *name,
                     const hsize_t *frame_idx, const hsize_t *frame_size,
                     void *buffer) {

  int retval = 0;
  herr_t err = 0;
  hid_t g_id, ds_id, s_id, ms_id, t_id;

  g_id = desc->data_g_id;

  ds_id = H5Dopen2(g_id, name, H5P_DEFAULT);
  if (ds_id <= 0) {
    char message[64];
    sprintf(message, "Unable to open dataset %.32s", name);
    ERROR_JUMP(-1, done, message);
  }
  s_id = H5Dget_space(ds_id);
  if (s_id <= 0) {
    ERROR_JUMP(-1, close_dataset, "Error getting dataspace");
  }
  t_id = H5Dget_type(ds_id);
  if (t_id <= 0) {
    ERROR_JUMP(-1, close_type, "Error retrieving datatype");
  }
  err = H5Sselect_hyperslab(s_id, H5S_SELECT_SET, frame_idx, NULL, frame_size,
                            NULL);
  if (err < 0) {
    ERROR_JUMP(-1, close_space, "Error seleting hyperslab");
  }
  ms_id = H5Screate_simple(3, frame_size, frame_size);
  if (ms_id < 0) {
    ERROR_JUMP(-1, close_space, "Could not create dataspace");
  }

  err = H5Dread(ds_id, t_id, ms_id, s_id, H5P_DEFAULT, buffer);
  if (err < 0) {
    ERROR_JUMP(-1, close_mspace, "Error reading dataset");
  }

close_mspace:
  H5Sclose(ms_id);
close_space:
  H5Sclose(s_id);
close_type:
  H5Tclose(t_id);
close_dataset:
  H5Dclose(ds_id);
done:
  return retval;
}

int get_frame_from_chunk(const struct ds_desc_t *desc, const char *ds_name,
                         const hsize_t *frame_idx, const hsize_t *frame_size,
                         void *buffer) {

  hid_t d_id = 0;
  hsize_t c_offset[3] = {frame_idx[0], 0, 0};
  uint32_t c_filter_mask = 0;
  hsize_t c_bytes;
  void *c_buffer = NULL;
  int retval = 0;

  if (frame_idx[1] != 0 || frame_idx[2] != 0) {
    char message[64];
    sprintf(message,
            "Require frame selection starts at [n, 0, 0], not [n, %llu, %llu]",
            frame_idx[1], frame_idx[2]);
    ERROR_JUMP(-1, done, message);
  }

  /* --- CACHING LOGIC START --- */
  /* Check if we already have this dataset open */
  if (CACHED_DID > 0 && strcmp(ds_name, CACHED_DS_NAME) == 0) {
      d_id = CACHED_DID;
  } else {
      if (CACHED_DID > 0) {
          H5Dclose(CACHED_DID);
          CACHED_DID = 0;
      }
      
      d_id = H5Dopen(desc->data_g_id, ds_name, H5P_DEFAULT);
      if (d_id < 0) {
        char message[64];
        sprintf(message, "Error opening dataset %.32s", ds_name);
        ERROR_JUMP(-1, done, message);
      }

      CACHED_DID = d_id;
      strncpy(CACHED_DS_NAME, ds_name, 255);
  }
  /* --- CACHING LOGIC END --- */

  if (H5Dget_chunk_storage_size(d_id, c_offset, &c_bytes) < 0) {
    char message[96];
    sprintf(message, "Error reading chunk size from %.32s for frame %llu",
            ds_name, frame_idx[0]);
    ERROR_JUMP(-1, done, message);
  }
  if (c_bytes == 0) {
    char message[96];
    sprintf(message, "Target chunk %llu has zero size for dataset %.32s",
            frame_idx[0], ds_name);
    ERROR_JUMP(-1, done, message);
  }

  c_buffer = buffer;
  

  if (H5DOread_chunk(d_id, H5P_DEFAULT, c_offset, &c_filter_mask, c_buffer) <
      0) {
    char message[128];
    sprintf(message,
            "Error reading chunk %llu from dataset %.32s - size %llu bytes",
            frame_idx[0], ds_name, c_bytes);
    ERROR_JUMP(-1, done, message);
  }

done:
  if (c_buffer && (c_buffer != buffer))
    free(c_buffer);
  
  /* The cache owns it now. */
  // if (d_id) H5Dclose(d_id); 

  return retval;
}

int get_nxs_frame(const struct ds_desc_t *desc, const int n, void *buffer) {
  /* detector data are the two inner most indices */
  /* TODO: handle ndims > 3 and select appropriately */
  int retval = 0;
  hsize_t frame_idx[3] = {n, 0, 0};
  hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};
  if (n < 0 || n >= desc->dims[0]) {
    char message[64];
    sprintf(message, "Selected frame %d is out of range valid range [0, %d]", n,
            (int)desc->dims[0] - 1);
    ERROR_JUMP(-1, done, message);
  }
  retval = get_frame_simple(desc, "data", frame_idx, frame_size, buffer);
  if (retval < 0) {
    ERROR_JUMP(retval, done, "");
  }
done:
  return retval;
}

int get_dectris_eiger_frame(const struct ds_desc_t *desc, int n, void *buffer) {

  int retval = 0;
  int block, frame_count, idx;
  struct eiger_ds_desc_t *eiger_desc = (struct eiger_ds_desc_t *)desc;
  char data_name[16] = {0};
  hsize_t frame_idx[3] = {0, 0, 0};
  hsize_t frame_size[3] = {1, desc->dims[1], desc->dims[2]};

  if (n < 0 || n >= desc->dims[0]) {
    char message[64];
    sprintf(message, "Selected frame %d is out of range valid range [0, %d]", n,
            (int)desc->dims[0] - 1);
    ERROR_JUMP(-1, done, message);
  }

  /* determine the relevant data block */
  frame_count = 0;
  block = 0;
  while ((frame_count += eiger_desc->block_sizes[block]) <= n)
    block++;
  idx = n - (frame_count -
             eiger_desc->block_sizes[block]); /* index in current block */
  frame_idx[0] = idx;
  sprintf(data_name, "data_%06d", block + 1);
  retval =
      eiger_desc->frame_func(desc, data_name, frame_idx, frame_size, buffer);
  if (retval < 0) {
    ERROR_JUMP(retval, done, "");
  }
done:
  return retval;
}

int get_dectris_eiger_dataset_dims(struct ds_desc_t *desc) {
  int retval = 0;
  int n_datas = 0;
  int n = 0;
  int data_width = 0;
  int ndims = 3;
  char ds_name[16] = {0}; /* 12 chars in "data_xxxxxx\0" */
  int *frame_counts = NULL;
  hsize_t dims[3] = {0};
  struct eiger_ds_desc_t *eiger_desc = (struct eiger_ds_desc_t *)desc;

  /* datasets are "data_%06d % n" - need to determine how many of these there
   * are and what the ranges are */

  sprintf(ds_name, "data_%06d", n_datas + 1);
  while (H5Lexists(desc->data_g_id, ds_name, H5P_DEFAULT) > 0) {
    sprintf(ds_name, "data_%06d", ++n_datas + 1);
  }

  frame_counts = malloc(n_datas * sizeof(*frame_counts));

  for (n = 0; n < n_datas; n++) {
    hid_t ds_id, t_id, s_id;
    hsize_t block_dims[3] = {0};
    sprintf(ds_name, "data_%06d", n + 1);
    ds_id = H5Dopen2(desc->data_g_id, ds_name, H5P_DEFAULT);
    if (ds_id < 0) {
      char message[64];
      sprintf(message, "Unable to open dataset %.16s", ds_name);
      ERROR_JUMP(-1, loop_end, message);
    }
    t_id = H5Dget_type(ds_id);
    if (t_id < 0) {
      ERROR_JUMP(-1, close_dataset, "Unable to get datatype")
    }
    s_id = H5Dget_space(ds_id);
    if (s_id < 0) {
      ERROR_JUMP(-1, close_type, "Unable to get dataspace");
    }

    data_width = H5Tget_size(t_id);
    if (data_width <= 0) {
      ERROR_JUMP(-1, close_space, "Unable to get type size");
    }

    ndims = H5Sget_simple_extent_ndims(s_id);
    if (ndims != 3) {
      char message[64];
      sprintf(message, "Dataset %.16s has rank %d, expected %d", ds_name, ndims,
              3);
      ERROR_JUMP(-1, close_space, message);
    }
    if (H5Sget_simple_extent_dims(s_id, block_dims, NULL) < 0) {
      ERROR_JUMP(-1, close_space, "Unable to read dataset dimensions");
    }

    dims[1] = block_dims[1];
    dims[2] = block_dims[2];

    dims[0] += block_dims[0];
    frame_counts[n] = block_dims[0];

  close_space:
    H5Sclose(s_id);
  close_type:
    H5Tclose(t_id);
  close_dataset:
    H5Dclose(ds_id);
  loop_end:
    if (retval < 0)
      break;
  }

  if (retval < 0) {
    free(frame_counts);
  } else {
    memcpy(desc->dims, dims, 3 * sizeof(*dims));
    desc->data_width = data_width;
    eiger_desc->n_data_blocks = n_datas;
    eiger_desc->block_sizes = frame_counts;
  }
  return retval;
}

int read_pixel_info(hid_t g_id, const char *path, double *size) {
  /*
   * NXdetector allows pixel size to be an array (for varied pixel size),
   * however XDS only allows for a single value.
   * TODO: handle array case (return first value maybe?)
   */

  /* read the scalar dataset value and scale according to the unit in the
   * attribute */
  /* returned value is in metres */
  int retval = 0;
  herr_t err = 0;
  hid_t ds_id;
  double value = 0;
  ds_id = H5Dopen2(g_id, path, H5P_DEFAULT);
  if (ds_id < 0) {
    char message[64];
    sprintf(message, "Error opening dataset %.32s", path);
    ERROR_JUMP(-1, done, message);
  }

  err =
      H5Dread(ds_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &value);
  if (err < 0) {
    char message[64];
    sprintf(message, "Error reading dataset %.32s", path);
    ERROR_JUMP(-1, close_dataset, message);
  }

  if (H5Aexists(ds_id, "units") > 0) {
    /* string may be variable length */
    hid_t a_id, t_id, mt_id;
    void *str_buffer = NULL;
    int str_size = -1;
    double scale = 1;
    a_id = H5Aopen(ds_id, "units", H5P_DEFAULT);
    if (a_id < 0) {
      char message[100];
      sprintf(message,
              "Error openeing units attribute for %.32s after existence check",
              path);
      ERROR_JUMP(-1, close_dataset, message);
    }

    t_id = H5Aget_type(a_id);
    if (t_id < 0) {
      ERROR_JUMP(-1, close_attribute, "Error getting datatype");
    }
    /* TODO: handle multiple strings in attribute (just detect and error) */
    if (H5Tis_variable_str(t_id) > 0) {
      str_size = -1;
      str_buffer = malloc(sizeof(char *));
    } else {
      str_size = H5Tget_size(t_id);
      /* do not assume room has been left for null-byte in fixed length string
       */
      str_buffer = malloc(str_size + 1);
    }
    if (str_buffer == NULL) {
      ERROR_JUMP(-1, close_datatype,
                 "Unable to allocate space for variable length string");
    }
    mt_id = H5Tcopy(H5T_C_S1);
    if (mt_id < 0) {
      ERROR_JUMP(-1, free_string, "Error creating HDF5 String datatype");
    }
    err = H5Tset_size(mt_id, str_size == -1 ? H5T_VARIABLE : str_size);
    if (err < 0) {
      char message[64];
      sprintf(message, "Error setting datatype size to %d", str_size);
      ERROR_JUMP(-1, close_mem_datatype, message);
    }

    err = H5Aread(a_id, mt_id, str_buffer);
    if (err < 0) {
      ERROR_JUMP(-1, close_mem_datatype, "Error reading units attribute");
    }
    /* ensure last byte is null */
    if (str_size > 0)
      ((char *)str_buffer)[str_size] = '\0';

    scale = scale_from_units(str_size == -1 ? *(char **)str_buffer
                                            : (char *)str_buffer);
    value *= scale;

    if (str_size == -1) {
      /* we have to create this dataspace just to indicate we want
       * to clear the entire vlen dataset (all 1 element - good job HDF5)
       */
      hsize_t dims[1] = {1};
      hid_t s_id = H5Screate_simple(1, dims, NULL);
      H5Sselect_all(s_id);
      H5Dvlen_reclaim(mt_id, s_id, H5P_DEFAULT, str_buffer);
      H5Sclose(s_id);
    }
  close_mem_datatype:
    H5Tclose(mt_id);
  free_string:
    free(str_buffer);
  close_datatype:
    H5Tclose(t_id);
  close_attribute:
    H5Aclose(a_id);
  } /* if H5Aexists(...) */

close_dataset:
  H5Dclose(ds_id);
done:
  if (retval == 0)
    *size = value;
  return retval;
}

int get_nxs_pixel_info(const struct ds_desc_t *desc, double *x_size,
                       double *y_size) {
  int retval = 0;
  if (read_pixel_info(desc->det_g_id, "x_pixel_size", x_size) < 0) {
    ERROR_JUMP(-1, done, "");
  }
  if (read_pixel_info(desc->det_g_id, "y_pixel_size", y_size) < 0) {
    ERROR_JUMP(-1, done, "");
  }
done:
  return retval;
}

int get_dectris_eiger_pixel_info(const struct ds_desc_t *desc, double *x_size,
                                 double *y_size) {
  int retval = 0;
  if (read_pixel_info(desc->det_g_id, "detectorSpecific/x_pixel_size", x_size) <
      0) {
    ERROR_JUMP(-1, done, "");
  }
  if (read_pixel_info(desc->det_g_id, "detectorSpecific/y_pixel_size", y_size) <
      0) {
    ERROR_JUMP(-1, done, "");
  }
done:
  return retval;
}

int get_nxs_pixel_mask(const struct ds_desc_t *desc, int *buffer) {
  int retval = 0;
  hid_t ds_id;
  herr_t err = 0;

  ds_id = H5Dopen2(desc->det_g_id, "pixel_mask", H5P_DEFAULT);
  if (ds_id < 0) {
    ERROR_JUMP(-1, done, "Error opening pixel_mask dataset");
  }

  err = H5Dread(ds_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer);
  if (err < 0) {
    ERROR_JUMP(-1, close_dataset, "Error reading pixel_mask dataset");
  }

close_dataset:
  H5Dclose(ds_id);
done:
  return retval;
}

int get_dectris_eiger_pixel_mask(const struct ds_desc_t *desc, int *buffer) {
  int retval = 0;
  hid_t ds_id;
  herr_t err = 0;

  ds_id = H5Dopen2(desc->det_g_id, "detectorSpecific/pixel_mask", H5P_DEFAULT);
  if (ds_id < 0) {
    ERROR_JUMP(-1, done, "Error opening detectorSpecific/pixel_mask");
  }

  err = H5Dread(ds_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer);
  if (err < 0) {
    ERROR_JUMP(-1, close_dataset, "Error reading detectorSpecific/pixel_mask");
  }

close_dataset:
  H5Dclose(ds_id);
done:
  return retval;
}

int get_null_pixel_mask(const struct ds_desc_t *desc, int *buffer) {
  hsize_t buffer_length = desc->dims[1] * desc->dims[2];
  memset(buffer, 0, sizeof(*buffer) * buffer_length);
  return 0;
}

herr_t det_visit_callback(hid_t root_id, const char *name,
                          const H5O_info_t *info, void *op_data) {
  struct det_visit_objects_t *output_data = op_data;
  hid_t g_id;
  herr_t retval = 0;
  if (info->type != H5O_TYPE_GROUP)
    return 0;
  g_id = H5Oopen(root_id, name, H5P_DEFAULT);
  if (g_id < 0) {
    char message[256];
    sprintf(message, "H5OVisit callback: Unable to open group %.128s", name);
    ERROR_JUMP(-1, done, message);
  }

  /* check for an "NX_class" attribute */
  {
    int str_size = 0;
    void *buffer = NULL;
    hid_t a_id, t_id, mt_id;
    if (H5Aexists(g_id, "NX_class") <= 0) {
      /* not an error - just close group and allow continuation */
      retval = 0;
      goto close_group;
    }
    a_id = H5Aopen(g_id, "NX_class", H5P_DEFAULT);
    if (a_id <= 0) {
      char message[256];
      sprintf(message,
              "H5OVisit callback: Error opening NX_class attribute on %.128s "
              "after existence check",
              name);
      ERROR_JUMP(-1, close_group, message);
    }

    t_id = H5Aget_type(a_id);
    if (t_id < 0) {
      ERROR_JUMP(-1, close_attr, "Error getting datatype");
    }
    if (H5Tis_variable_str(t_id) > 0) {
      str_size = -1;
      buffer = malloc(sizeof(char *));
    } else {
      str_size = H5Tget_size(t_id);
      buffer = malloc(str_size + 1);
    }
    if (!buffer) {
      ERROR_JUMP(-1, close_type, "Error allocating string buffer");
    }

    mt_id = H5Tcopy(H5T_C_S1);
    if (mt_id < 0) {
      ERROR_JUMP(-1, free_buffer, "Error creating HDF5 String datatype");
    }
    // set the target string type to be one longer than the recorded string
    // in keeping with the malloc'd buffer - if this is already a null
    // terminated string then we will just have two nulls, if it is not
    // then we won't clobber the last char in the buffer with a null in the
    // H5Aread call
    if (H5Tset_size(mt_id, str_size == -1 ? H5T_VARIABLE : str_size + 1) < 0) {
      char message[64];
      sprintf(message, "Error setting string datatype to size %d", str_size);
      ERROR_JUMP(-1, close_mtype, message);
    }

    if (H5Aread(a_id, mt_id, buffer) < 0) {
      char message[256];
      sprintf(
          message,
          "H5OVisit callback: Error reading NX_class attribute on group %.128s",
          name);
      ERROR_JUMP(-1, close_mtype, message);
    }

    /* at least one file has been seen where the NX_class attribute was not null
     * terminated and extraneous bytes where being read by strcmp -  set the end
     * byte to null
     */

    if (str_size > 0)
      ((char *)buffer)[str_size] = '\0';
    /* test for NXdata or NXdetector */
    {
      char *nxclass = str_size > 0 ? (char *)buffer : *((char **)buffer);
      if (strcmp("NXdata", nxclass) == 0) {
        hid_t out_id = H5Gopen(root_id, name, H5P_DEFAULT);
        output_data->nxdata = out_id;
      } else if (strcmp("NXdetector", nxclass) == 0) {
        hid_t out_id = H5Gopen(root_id, name, H5P_DEFAULT);
        output_data->nxdetector = out_id;
      }
    }

    if (str_size == -1) {
      hsize_t dims[1] = {1};
      hid_t s_id = H5Screate_simple(1, dims, NULL);
      H5Sselect_all(s_id);
      H5Dvlen_reclaim(mt_id, s_id, H5P_DEFAULT, buffer);
      H5Sclose(s_id);
    }

  close_mtype:
    H5Tclose(mt_id);
  free_buffer:
    free(buffer);
  close_type:
    H5Tclose(t_id);
  close_attr:
    H5Aclose(a_id);
  }

close_group:
  if (H5Gclose(g_id) < 0) {
    /* TODO: error trace */
    retval = -1;
  }
done:
  return retval;
}

int check_for_chunk_read(hid_t g_id, const char *ds_name,
                         struct opt_eiger_ds_desc_t *desc) {

  int retval = 0;
  int n_filters;
  hsize_t cdims[3];
  hid_t ds_id, dcpl, s_id;
  unsigned int filter_flags, filter_config;
  char filter_name[16];
  size_t name_len = 16;
  size_t cd_nelems = BS_H5_N_PARAMS;
  hsize_t dims[3];
  H5Z_filter_t filter;

  dcpl = 0;
  s_id = 0;
  ds_id = 0;

  ds_id = H5Dopen2(g_id, ds_name, H5P_DEFAULT);
  if (ds_id < 0) {
    char message[64];
    sprintf(message, "Error opening dataset %.32s", ds_name);
    ERROR_JUMP(-1, done, message)
  }

  s_id = H5Dget_space(ds_id);
  if (s_id < 0) {
    char message[64];
    sprintf(message, "Error opening dataspace for %.32s", ds_name);
    ERROR_JUMP(-1, done, message);
  }

  if (3 != H5Sget_simple_extent_ndims(s_id)) {
    goto done;
  }

  if (H5Sget_simple_extent_dims(s_id, dims, NULL) < 0) {
    char message[80];
    sprintf(message, "Error retriving dataset dimensions for %.32s", ds_name);
    ERROR_JUMP(-1, done, message);
  }

  dcpl = H5Dget_create_plist(ds_id);
  if (dcpl < 0) {
    ERROR_JUMP(-1, done, "Error getting dataset creation property list");
  }

  /* check the chunk layout matches the layout we expect of
   * [1, frame_size_y, frame_size_x] (1 frame == 1 chunk) */
  int cndims = H5Pget_chunk(dcpl, 3, cdims);
  if (cndims != 3) {
    goto done;
  }
  if (cdims[0] != 1 || cdims[1] != dims[1] || cdims[2] != dims[2]) {
    goto done;
  }

  /* check for potential filters - only the bitshuffle filter is supported */
  n_filters = H5Pget_nfilters(dcpl);
  if (n_filters < 0) {
    ERROR_JUMP(-1, done, "Error retrieving number of filters on dataset");
  } else if (n_filters > 1) {
    goto done;
  }
  unsigned int tmp_params[BS_H5_N_PARAMS] = {0};

  filter = H5Pget_filter2(dcpl, 0, &filter_flags, &cd_nelems, tmp_params,
                        name_len, filter_name, &filter_config);

  if (filter < 0) {
    ERROR_JUMP(-1, done, "Error retrieving filter information");
  }
  if (filter != BS_H5_FILTER_ID) {
    goto done;
  }
  if (cd_nelems > BS_H5_N_PARAMS) {
    char message[128];
    sprintf(message,
            "More than expected number of parameters to bitshuffle filter - "
            "expected %d, was %lu",
            BS_H5_N_PARAMS, cd_nelems);
    ERROR_JUMP(-1, done, message);
  }

  retval = 1;

done:
  if (dcpl)
    H5Pclose(dcpl);
  if (s_id)
    H5Sclose(s_id);
  if (ds_id)
    H5Dclose(ds_id);
  return retval;
}

int create_dataset_descriptor(struct ds_desc_t **desc,
                              struct det_visit_objects_t *visit_result)
{
    int retval = 0;
    hid_t g_id, ds_id;
    int (*pxl_func)(const struct ds_desc_t *, double *, double *);
    int (*pxl_mask_func)(const struct ds_desc_t *, int *);
    int (*ds_prop_func)(struct ds_desc_t *);
    void (*free_func)(struct ds_desc_t *);
    struct ds_desc_t *output;

    /* Use NXdetector for pixel properties (x_pixel_size, y_pixel_size, mask) */
    g_id = visit_result->nxdetector;

    /* ---------------- Pixel size ---------------- */
    if (g_id > 0 &&
        H5Lexists(g_id, "x_pixel_size", H5P_DEFAULT) > 0 &&
        H5Lexists(g_id, "y_pixel_size", H5P_DEFAULT) > 0) {

        /* /entry/instrument/detector/x_pixel_size & y_pixel_size */
        pxl_func = &get_nxs_pixel_info;

    } else if (g_id > 0 &&
               H5Lexists(g_id, "detectorSpecific", H5P_DEFAULT) > 0 &&
               H5Lexists(g_id, "detectorSpecific/x_pixel_size", H5P_DEFAULT) > 0 &&
               H5Lexists(g_id, "detectorSpecific/y_pixel_size", H5P_DEFAULT) > 0) {

        /* /entry/instrument/detector/detectorSpecific/x_pixel_size & y_pixel_size */
        pxl_func = &get_dectris_eiger_pixel_info;

    } else {
        ERROR_JUMP(-1, done, "Could not locate x/y pixel_size");
    }

    /* ---------------- Pixel mask ---------------- */
    if (g_id > 0 && H5Lexists(g_id, "pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_nxs_pixel_mask;
    } else if (g_id > 0 &&
               H5Lexists(g_id, "detectorSpecific", H5P_DEFAULT) > 0 &&
               H5Lexists(g_id, "detectorSpecific/pixel_mask", H5P_DEFAULT) > 0) {
        pxl_mask_func = &get_dectris_eiger_pixel_mask;
    } else {
        pxl_mask_func = &get_null_pixel_mask;
        fprintf(stderr, "WARNING: No pixel mask found\n");
    }

    /* ---------------- Locate DATA ----------------
     * We support both:
     *  - Dectris style:   data_000001 under NXdetector
     *  - ESRF/Lima style: data_000001 under NXdata (/entry/data)
     */

    ds_id = -1;
    ds_prop_func = NULL;

    /* Case 1: data_000001 / data under NXdetector group */
    if (g_id > 0 && H5Lexists(g_id, "data_000001", H5P_DEFAULT) > 0) {
        ds_id = g_id;
        ds_prop_func = &get_dectris_eiger_dataset_dims;

    } else if (g_id > 0 && H5Lexists(g_id, "data", H5P_DEFAULT) > 0) {
        ds_id = g_id;
        ds_prop_func = &get_nxs_dataset_dims;

    /* Case 2: data_000001 / data under NXdata group (/entry/data) */
    } else if (visit_result->nxdata &&
               H5Lexists(visit_result->nxdata, "data_000001", H5P_DEFAULT) > 0) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_dectris_eiger_dataset_dims;

    } else if (visit_result->nxdata &&
               H5Lexists(visit_result->nxdata, "data", H5P_DEFAULT) > 0) {
        ds_id = visit_result->nxdata;
        ds_prop_func = &get_nxs_dataset_dims;

    } else {
        ERROR_JUMP(-1, done, "Could not locate DATA dataset");
    }

    /* ===================================================
     *     EIGER / CHUNKED / VDS HANDLING
     * =================================================== */
    if (ds_prop_func == &get_dectris_eiger_dataset_dims) {

        struct eiger_ds_desc_t *eiger_desc =
            calloc(1, sizeof(struct eiger_ds_desc_t));
        struct opt_eiger_ds_desc_t *opt =
            calloc(1, sizeof(struct opt_eiger_ds_desc_t));

        if (!eiger_desc || !opt) {
            free(eiger_desc);
            free(opt);
            ERROR_JUMP(-1, done, "Out of memory creating Eiger descriptors");
        }

        /* Default: original chunk-based Eiger reading */
        eiger_desc->frame_func = &get_frame_simple;
        opt->base.frame_func   = &get_frame_from_chunk;

        /* Inspect data_000001 under whatever group we found (NXdetector or NXdata) */
        hid_t d = H5Dopen2(ds_id, "data_000001", H5P_DEFAULT);
        if (d < 0) {
            free(eiger_desc);
            free(opt);
            ERROR_JUMP(-1, done, "Could not open data_000001 in data group");
        }

        hid_t p = H5Dget_create_plist(d);
        H5D_layout_t L = H5Pget_layout(p);
        size_t mc = 0;
        if (L == H5D_VIRTUAL)
            H5Pget_virtual_count(p, &mc);

        if (L == H5D_VIRTUAL && mc > 0) {
            /* ---------------- VDS PATH: build explicit mapping, bypass VDS ---------------- */
            free(eiger_desc);

            *(struct opt_eiger_ds_desc_t **)desc = opt;
            free_func = &free_opt_eiger_desc;

            opt->is_virtual     = 1;
            opt->n_map_entries  = (int)mc;
            opt->frame_map_entries =
                calloc(mc, sizeof(struct FrameMapEntry));
            if (!opt->frame_map_entries) {
                H5Pclose(p);
                H5Dclose(d);
                free(opt);
                ERROR_JUMP(-1, done, "Out of memory building VDS mapping");
            }

            /* Global dims from the VDS itself */
            hid_t s = H5Dget_space(d);
            H5Sget_simple_extent_dims(s, opt->base.base.dims, NULL);
            H5Sclose(s);

            /* Build one FrameMapEntry per VDS mapping */
            for (size_t i = 0; i < mc; ++i) {
                struct FrameMapEntry *E = &opt->frame_map_entries[i];

                /* Source file & dataset */
                ssize_t fl = H5Pget_virtual_filename(p, i, NULL, 0);
                E->src_file = calloc(fl + 1, 1);
                H5Pget_virtual_filename(p, i, E->src_file, fl + 1);

                ssize_t dl = H5Pget_virtual_dsetname(p, i, NULL, 0);
                E->src_dset = calloc(dl + 1, 1);
                H5Pget_virtual_dsetname(p, i, E->src_dset, dl + 1);

                /* Virtual and source hyperslab bounds */
                hid_t vsp = H5Pget_virtual_vspace(p, i);
                hid_t ssp = H5Pget_virtual_srcspace(p, i);

                hsize_t v0[3], v1[3], s0[3], s1[3];
                H5Sget_select_bounds(vsp, v0, v1);
                H5Sget_select_bounds(ssp, s0, s1);
                H5Sclose(vsp);
                H5Sclose(ssp);

                E->vds_start = v0[0];
                E->vds_end   = v1[0];
                E->src_start = s0[0];
                E->src_end   = s1[0];

                /* Open source file + dataset once, reuse later */
                E->src_file_id = H5Fopen(E->src_file,
                                         H5F_ACC_RDONLY, H5P_DEFAULT);
                E->src_dset_id = H5Dopen2(E->src_file_id,
                                          E->src_dset, H5P_DEFAULT);

                /* Detect bitshuffle-LZ4 on the source dataset (if any) */
                hid_t sp = H5Dget_create_plist(E->src_dset_id);
                int nf = H5Pget_nfilters(sp);
                if (nf == 0) {
                    E->direct_read = 1;
                } else {
                    unsigned fcfg, flg;
                    size_t n = BS_H5_N_PARAMS;
                    H5Z_filter_t f =
                        H5Pget_filter2(sp, 0, &flg, &n,
                                       E->bs_params,
                                       0, NULL, &fcfg);
                    if (f == BS_H5_FILTER_ID) {
                        E->direct_read   = 1;
                        E->has_bs_filter = 1;
                    }
                }
                H5Pclose(sp);
            }

            /* Sort mappings by vds_start for fast lookup */
            qsort(opt->frame_map_entries,
                  opt->n_map_entries,
                  sizeof(struct FrameMapEntry),
                  compare_frame_map);

            /* Per-frame access function for the whole dataset */
            opt->base.base.get_data_frame = get_virtual_frame;
            output = (struct ds_desc_t *)opt;

        } else {
            /* ---------------- No VDS: original Eiger/chunk behaviour ---------------- */
            free(opt);

            *(struct eiger_ds_desc_t **)desc = eiger_desc;
            free_func = &free_eiger_desc;
            output = (struct ds_desc_t *)eiger_desc;

            /* IMPORTANT: set group before computing dims */
            output->det_g_id  = g_id;
            output->data_g_id = ds_id;

            ds_prop_func(output);  /* get_dectris_eiger_dataset_dims() */
        }

        H5Pclose(p);
        H5Dclose(d);

    } else {
        /* ===================================================
         *     NORMAL NXS DETECTOR (single "data" dataset)
         * =================================================== */
        *desc = calloc(1, sizeof(struct nxs_ds_desc_t));
        if (!*desc)
            ERROR_JUMP(-1, done, "Out of memory creating nxs_ds_desc_t");

        free_func = &free_nxs_desc;
        output = *desc;

        /* Set group before computing dims */
        output->det_g_id  = g_id;
        output->data_g_id = ds_id;

        ds_prop_func(output);    /* get_nxs_dataset_dims() */
    }

    /* Select correct per-frame reader */
    if (free_func == &free_opt_eiger_desc) {
        /* Lima2 master with VDS bypass */
        output->get_data_frame = &get_virtual_frame;
    } else if (ds_prop_func == &get_dectris_eiger_dataset_dims) {
        /* Classic Eiger: data_000001, data_000002, ... */
        output->get_data_frame = &get_dectris_eiger_frame;
    } else {
        /* Plain NXdetector data */
        output->get_data_frame = &get_nxs_frame;
    }

    /* Common descriptor fields (for both paths where we didn't already set) */
    output->det_g_id  = g_id;
    output->data_g_id = ds_id;
    output->get_pixel_properties = pxl_func;
    output->get_pixel_mask       = pxl_mask_func;
    output->free_desc            = free_func;

done:
    return retval;
}



int get_detector_info(const hid_t fid, struct ds_desc_t **desc) {

  int retval = 0;
  herr_t err = 0;
  struct det_visit_objects_t objects = {0};
  err =
      H5Ovisit(fid, H5_INDEX_NAME, H5_ITER_INC, &det_visit_callback, &objects);
  if (err < 0) {
    clear_det_visit_objects(&objects);
    ERROR_JUMP(-1, done, "Error during H5Ovisit callback");
  }
  if (objects.nxdata == 0) {
    fprintf(stderr, "WARNING: Could not locate an NXdata entry\n");
  }
  if (objects.nxdetector == 0) {
    fprintf(stderr, "WARNING: Could not locate an NXdetector entry\n");
  }

  if ((retval = create_dataset_descriptor(desc, &objects)) < 0) {
    ERROR_JUMP(retval, done, "");
  };

done:
  return retval;
}


static int get_virtual_frame(const struct ds_desc_t *d, const int f, void *buf)
{
    const struct opt_eiger_ds_desc_t *opt = (const void*)d;
    fprintf(stderr, "[DEBUG] get_virtual_frame: f=%d, n_map_entries=%d\n",
          f, opt->n_map_entries);

    for(int i=0; i<opt->n_map_entries; ++i){
        struct FrameMapEntry *E = &opt->frame_map_entries[i];
        if(f < E->vds_start || f > E->vds_end) continue;

        hsize_t li = E->src_start + (f - E->vds_start);
        hsize_t off[3]={li,0,0}, size[3]={1,d->dims[1],d->dims[2]};

        if(E->direct_read){
            uint32_t mask=0;
            hsize_t sz=0;
            H5Dget_chunk_storage_size(E->src_dset_id, off, &sz);
            void *tmp = E->has_bs_filter ? malloc(sz) : buf;
            H5DOread_chunk(E->src_dset_id, H5P_DEFAULT, off, &mask, tmp);
            if(E->has_bs_filter){
                bslz4_decompress(E->bs_params, sz,
                                 tmp, d->data_width*d->dims[1]*d->dims[2],
                                 buf);
                free(tmp);
            }
            return 0;
        }

        hid_t fs = H5Dget_space(E->src_dset_id);
        H5Sselect_hyperslab(fs,H5S_SELECT_SET,off,NULL,size,NULL);
        hid_t ms = H5Screate_simple(3,size,NULL);
        hid_t t = H5Dget_type(E->src_dset_id);
        H5Dread(E->src_dset_id,t,ms,fs,H5P_DEFAULT,buf);
        H5Tclose(t); H5Sclose(ms); H5Sclose(fs);
        return 0;
    }
    return -1;
}
