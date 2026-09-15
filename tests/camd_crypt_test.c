/* Test vectors for the CAM crypto primitives. A wrong DES round or
 * crypt(3) byte only shows up as login failed from the CAM server, with
 * nothing to say why, so they are pinned here: DES from the FIPS/NBS
 * validation set, MD5 from RFC 1321, crypt(3) from openssl passwd -1. */
#include <stdio.h>
#include <string.h>

#include "dtv_camd_crypt.h"

static int failures;

static void hex(const uint8_t *data, size_t size, char *out)
{
    size_t i;
    for (i = 0; i < size; ++i)
        sprintf(out + i * 2u, "%02x", data[i]);
}

static void check_hex(const char *what, const uint8_t *data, size_t size,
                      const char *expected)
{
    char text[128];
    hex(data, size, text);
    if (strcmp(text, expected) != 0) {
        printf("FAIL %s\n  expected: %s\n  found   : %s\n",
               what, expected, text);
        ++failures;
    } else {
        printf("ok   %s = %s\n", what, text);
    }
}

static void check_text(const char *what, const char *value,
                       const char *expected)
{
    if (strcmp(value, expected) != 0) {
        printf("FAIL %s\n  expected: %s\n  found   : %s\n",
               what, expected, value);
        ++failures;
    } else {
        printf("ok   %s = %s\n", what, value);
    }
}

static void test_des(void)
{
    /* FIPS 46-3 / NBS example: the classic "Now is the time for all "
     * plaintext block under key 0x0123456789abcdef. */
    static const uint8_t key[8] =
        { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const uint8_t plain[8] =
        { 0x4e, 0x6f, 0x77, 0x20, 0x69, 0x73, 0x20, 0x74 };
    dtv_des_key schedule;
    uint8_t cipher[8], decrypted[8];

    dtv_des_set_key(&schedule, key);
    dtv_des_encrypt_block(&schedule, plain, cipher);
    check_hex("DES encrypt", cipher, 8, "3fa40e8a984d4815");

    dtv_des_decrypt_block(&schedule, cipher, decrypted);
    check_hex("DES decrypt", decrypted, 8, "4e6f772069732074");

    /* All-zero key and data, another published pair. */
    {
        static const uint8_t zero[8] = { 0 };
        dtv_des_key zero_schedule;
        uint8_t out[8];
        dtv_des_set_key(&zero_schedule, zero);
        dtv_des_encrypt_block(&zero_schedule, zero, out);
        check_hex("DES zero key", out, 8, "8ca64de9c1b123a7");
    }
}

static void test_des_parity(void)
{
    uint8_t key[8] = { 0x00, 0x01, 0x02, 0x03, 0xfe, 0xff, 0x80, 0x7f };
    dtv_des_set_odd_parity(key, sizeof(key));
    /* Every byte must end up with an odd number of set bits. */
    {
        size_t i;
        int ok = 1;
        for (i = 0; i < sizeof(key); ++i) {
            unsigned bit, ones = 0;
            for (bit = 0; bit < 8u; ++bit)
                ones += (key[i] >> bit) & 1u;
            if (!(ones & 1u))
                ok = 0;
        }
        if (!ok) {
            printf("FAIL DES odd parity\n");
            ++failures;
        } else {
            printf("ok   DES odd parity\n");
        }
    }
    check_hex("DES parity bytes", key, 8, "01010202fefe807f");
}

static void test_ede2_cbc(void)
{
    /* Round trip is what newcamd depends on: the same two keys and IV
     * must undo the encryption exactly, in place. */
    static const uint8_t key1[8] =
        { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const uint8_t key2[8] =
        { 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10 };
    static const uint8_t iv_source[8] =
        { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t data[24], original[24], iv[8];
    unsigned i;

    for (i = 0; i < sizeof(data); ++i)
        data[i] = (uint8_t)(i * 7u + 3u);
    memcpy(original, data, sizeof(data));

    memcpy(iv, iv_source, 8);
    dtv_des_ede2_cbc(key1, key2, iv, data, data, sizeof(data), 1);
    if (memcmp(data, original, sizeof(data)) == 0) {
        printf("FAIL 3DES-EDE2-CBC encryption left the data unchanged\n");
        ++failures;
    }

    memcpy(iv, iv_source, 8);
    dtv_des_ede2_cbc(key1, key2, iv, data, data, sizeof(data), 0);
    if (memcmp(data, original, sizeof(data)) != 0) {
        printf("FAIL 3DES-EDE2-CBC round trip\n");
        ++failures;
    } else {
        printf("ok   3DES-EDE2-CBC round trip\n");
    }

    /* With key1 == key2 two-key EDE collapses to plain single DES, which
     * gives an independent check against the DES vector above. */
    {
        static const uint8_t plain[8] =
            { 0x4e, 0x6f, 0x77, 0x20, 0x69, 0x73, 0x20, 0x74 };
        uint8_t block[8], zero_iv[8] = { 0 };
        memcpy(block, plain, 8);
        dtv_des_ede2_cbc(key1, key1, zero_iv, block, block, 8, 1);
        check_hex("EDE2 with equal keys == DES", block, 8,
                  "3fa40e8a984d4815");
    }
}

static void test_md5(void)
{
    uint8_t digest[16];

    dtv_md5("", 0, digest);
    check_hex("MD5(\"\")", digest, 16, "d41d8cd98f00b204e9800998ecf8427e");

    dtv_md5("abc", 3, digest);
    check_hex("MD5(\"abc\")", digest, 16,
              "900150983cd24fb0d6963f7d28e17f72");

    dtv_md5("12345678901234567890123456789012345678901234567890"
            "123456789012345678901234567890", 80, digest);
    check_hex("MD5(80 digits)", digest, 16,
              "57edf4a22be3c955ac49da2e2107b67a");
}

static void test_md5_crypt(void)
{
    char output[DTV_MD5_CRYPT_SIZE];

    /* The password in the OSCam configuration this was developed against;
     * newcamd always uses the fixed salt "abcdefgh". */
    dtv_md5_crypt("Windows", "$1$abcdefgh$", output, sizeof(output));
    check_text("crypt(Windows,$1$abcdefgh$)", output,
               "$1$abcdefgh$r29sKtHRBt0szjTr6aBDv/");

    dtv_md5_crypt("", "$1$abcdefgh$", output, sizeof(output));
    check_text("crypt(empty password)", output,
               "$1$abcdefgh$M55TzYaaccxVGbptZWaxX/");

    /* A password longer than one digest, to exercise the folding loop. */
    dtv_md5_crypt("0123456789abcdefghij", "$1$12345678$", output,
                  sizeof(output));
    check_text("crypt(20 characters)", output,
               "$1$12345678$T34DSIKpOl8hPPbhStXgI/");

    /* Salt given bare, without the $1$ wrapper, must behave the same. */
    dtv_md5_crypt("Windows", "abcdefgh", output, sizeof(output));
    check_text("crypt(bare salt)", output,
               "$1$abcdefgh$r29sKtHRBt0szjTr6aBDv/");
}

static void test_aes(void)
{
    /* FIPS-197 appendix C.1. */
    static const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
    };
    static const uint8_t plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    dtv_aes_key schedule;
    uint8_t cipher[16], decrypted[16];

    dtv_aes128_set_key(&schedule, key);
    dtv_aes128_encrypt_block(&schedule, plain, cipher);
    check_hex("AES-128 encrypt", cipher, 16,
              "69c4e0d86a7b0430d8cdb78070b4c55a");

    dtv_aes128_decrypt_block(&schedule, cipher, decrypted);
    check_hex("AES-128 decrypt", decrypted, 16,
              "00112233445566778899aabbccddeeff");
}

static void test_crc32(void)
{
    /* The check value every CRC-32 description quotes. */
    uint32_t crc = dtv_crc32(0, "123456789", 9);
    if (crc != 0xcbf43926u) {
        printf("FAIL CRC-32\n  expected: cbf43926\n  found   : %08x\n",
               (unsigned)crc);
        ++failures;
    } else {
        printf("ok   CRC-32 = %08x\n", (unsigned)crc);
    }
}

int main(void)
{
    test_des();
    test_des_parity();
    test_ede2_cbc();
    test_md5();
    test_md5_crypt();
    test_aes();
    test_crc32();
    if (failures)
        printf("\n%d tests failed.\n", failures);
    else
        printf("\nAll crypto tests passed.\n");
    return failures ? 1 : 0;
}
