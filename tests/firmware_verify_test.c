#include <stdio.h>
#include <stdlib.h>

#include "firmware_verify.h"

static int verify_file(const char *path, dtv_firmware_kind kind)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;
    int result = 1;

    if (!file) return 1;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) { fclose(file); return 1; }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length)
        goto done;
    if (dtv_firmware_verify(kind, data, (size_t)length) != 0)
        goto done;
    data[(size_t)length / 2] ^= 1;
    if (dtv_firmware_verify(kind, data, (size_t)length) == 0)
        goto done;
    result = 0;

done:
    free(data);
    fclose(file);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    if (verify_file(argv[1], DTV_FIRMWARE_IT9303) != 0) return 1;
    if (verify_file(argv[2], DTV_FIRMWARE_AVL62X1) != 0) return 1;
    puts("firmware SHA-256 verification passed");
    return 0;
}
