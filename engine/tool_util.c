#include "tool_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtv_platform.h"
#include "firmware_verify.h"

uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;
    long length;
    if (!file) return NULL;
    fseek(file, 0, SEEK_END); length = ftell(file); fseek(file, 0, SEEK_SET);
    if (length > 0) {
        data = malloc((size_t)length);
        if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
            free(data); data = NULL;
        }
    }
    fclose(file);
    if (data) *size = (size_t)length;
    return data;
}

int resolve_data_path(const char *relative, char *out, size_t size)
{
    char exe[DTV_PATH_MAX];
    snprintf(out, size, "%s", relative);
    if (dtv_executable_dir(exe, sizeof(exe)) != 0)
        return dtv_file_exists(out) ? 0 : -1;
    /* Installed tree: <exe dir>/firmware/... Both Windows and POSIX accept
     * '/' in the relative part. */
    snprintf(out, size, "%s%c%s", exe, DTV_PATH_SEPARATOR, relative);
    if (dtv_file_exists(out))
        return 0;
    /* Build tree: the exe sits in build/ and the data one level up. */
    snprintf(out, size, "%s%c..%c%s", exe, DTV_PATH_SEPARATOR,
             DTV_PATH_SEPARATOR, relative);
    if (dtv_file_exists(out))
        return 0;
    /* Last resort: relative to the working directory, as before. */
    snprintf(out, size, "%s", relative);
    return dtv_file_exists(out) ? 0 : -1;
}

int load_file_to_bridge(it9300 *bridge, const char *path, int demod)
{
    char resolved[DTV_PATH_MAX];
    size_t size = 0;
    uint8_t *data;

    resolve_data_path(path, resolved, sizeof(resolved));
    data = read_file(resolved, &size);
    if (!data) return -1;
    if (dtv_firmware_verify(demod ? DTV_FIRMWARE_AVL62X1
                                  : DTV_FIRMWARE_IT9303,
                            data, size) != 0) {
        free(data);
        return -2;
    }
    int rc = demod ? avl62x1_load_patch(bridge, AVL62X1_I2C_ADDR, data, size)
                   : it9300_download_firmware(bridge, data, size);
    free(data);
    return rc;
}
