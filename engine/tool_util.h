#ifndef DTV_TOOLS_TOOL_UTIL_H
#define DTV_TOOLS_TOOL_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "it9300.h"
#include "avl62x1.h"

/* Small helpers every command-line tool needs. */

/* Reads a whole file into memory; the caller frees. NULL on error. */
uint8_t *read_file(const char *path, size_t *size);

/* Loads a firmware file into the bridge (demod = 1) or boots the bridge
 * itself (demod = 0). 0 on success. */
int load_file_to_bridge(it9300 *bridge, const char *path, int demod);

#endif
