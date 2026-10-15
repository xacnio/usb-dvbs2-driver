#ifndef DTV_TOOLS_TOOL_UTIL_H
#define DTV_TOOLS_TOOL_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "usb_backend.h"
#include "hardware_profile.h"
#include "it9300.h"
#include "avl62x1.h"

/* Small helpers every command-line tool needs. */

/* Reads a whole file into memory; the caller frees. NULL on error. */
uint8_t *read_file(const char *path, size_t *size);

/* Finds a data file such as firmware/x.fw beside the executable, in the
 * folder above it (the build tree) or in the working directory. 0 when it
 * exists; out then holds its path. */
int resolve_data_path(const char *relative, char *out, size_t size);

/* Opens the first supported receiver plugged in and says which it is.
 * Returns its hardware profile, or NULL after telling stderr why not. */
const dtv_hardware_profile *open_connected_device(dtv_usb **usb);

/* Loads a firmware file into the bridge (demod = 1) or boots the bridge
 * itself (demod = 0). 0 on success. */
int load_file_to_bridge(it9300 *bridge, const char *path, int demod);

#endif
