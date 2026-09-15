/* DVB-CSA descrambling without a tuner. libdvbcsa can encrypt too, so a
 * scrambled stream is manufactured here and run back through dtv_csa. That
 * covers what is ours rather than the library's: parity selection, the
 * adaptation field offset, the scrambling bits, and leaving packets alone
 * when their parity has no key. */
#include <stdio.h>
#include <string.h>

#include "dtv_csa.h"
#include "dvbcsa/dvbcsa.h"

static int failures;

static void check(const char *what, int condition)
{
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

/* Builds one packet with a recognisable payload. With adaptation set, a two
 * byte adaptation field moves the encrypted payload along by three. */
static void build_packet(uint8_t *packet, uint16_t pid, unsigned parity,
                         int adaptation, uint8_t seed)
{
    size_t offset, i;
    memset(packet, 0, 188);
    packet[0] = 0x47;
    packet[1] = (uint8_t)((pid >> 8) & 0x1fu);
    packet[2] = (uint8_t)pid;
    /* scrambling control 0b10 even / 0b11 odd, plus the payload flag */
    packet[3] = (uint8_t)((parity ? 0xc0u : 0x80u) |
                          (adaptation ? 0x30u : 0x10u));
    if (adaptation) {
        packet[4] = 2;        /* adaptation field length */
        packet[5] = 0;
        packet[6] = 0;
        offset = 7;
    } else {
        offset = 4;
    }
    for (i = offset; i < 188u; ++i)
        packet[i] = (uint8_t)(seed + i);
}

static size_t payload_offset(const uint8_t *packet)
{
    return ((packet[3] >> 4) & 3u) == 3u ? 5u + packet[4] : 4u;
}

static void encrypt_packet(const uint8_t cw[8], uint8_t *packet)
{
    struct dvbcsa_key_s *key = dvbcsa_key_alloc();
    size_t offset = payload_offset(packet);
    dvbcsa_key_set(cw, key);
    dvbcsa_encrypt(key, packet + offset, (unsigned)(188u - offset));
    dvbcsa_key_free(key);
}

int main(void)
{
    static const uint8_t even_cw[8] =
        { 0x11, 0x22, 0x33, 0x66, 0xaa, 0xbb, 0xcc, 0x31 };
    static const uint8_t odd_cw[8] =
        { 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78 };
    /* Two packets per case (plain and with an adaptation field) for both
     * parities, plus one that must be left untouched. */
    enum { PACKETS = 5 };
    uint8_t stream[PACKETS * 188], expected[PACKETS * 188];
    dtv_csa *csa;
    size_t decrypted, i;

    build_packet(stream + 0 * 188, 0x100, 0, 0, 0x10);
    build_packet(stream + 1 * 188, 0x100, 0, 1, 0x20);
    build_packet(stream + 2 * 188, 0x101, 1, 0, 0x30);
    build_packet(stream + 3 * 188, 0x101, 1, 1, 0x40);
    build_packet(stream + 4 * 188, 0x102, 0, 0, 0x50);
    memcpy(expected, stream, sizeof(stream));

    encrypt_packet(even_cw, stream + 0 * 188);
    encrypt_packet(even_cw, stream + 1 * 188);
    encrypt_packet(odd_cw, stream + 2 * 188);
    encrypt_packet(odd_cw, stream + 3 * 188);
    encrypt_packet(even_cw, stream + 4 * 188);

    check("a scrambled packet differs from the plaintext",
          memcmp(stream, expected, 188) != 0);

    csa = dtv_csa_create();
    check("the descrambler is created", csa != NULL);
    if (!csa)
        return 1;

    /* Without keys nothing may be touched, and above all the scrambling
     * bits must stay on. */
    check("no key before one is set", !dtv_csa_has_key(csa));
    decrypted = dtv_csa_decrypt_run(csa, stream, PACKETS);
    check("without a key no packet is decrypted", decrypted == 0);
    check("without a key the scrambling bits stay",
          (stream[3] & 0xc0u) == 0x80u);

    /* Only the even word: the odd packets must stay scrambled. */
    dtv_csa_set_keys(csa, even_cw, 1, NULL, 0);
    check("one parity's word counts as a key", dtv_csa_has_key(csa));
    decrypted = dtv_csa_decrypt_run(csa, stream, PACKETS);
    check("three even-parity packets decrypted", decrypted == 3);
    check("even parity back to plaintext",
          memcmp(stream + 0 * 188 + 4, expected + 0 * 188 + 4, 184) == 0);
    check("even parity with an adaptation field back to plaintext",
          memcmp(stream + 1 * 188 + 7, expected + 1 * 188 + 7, 181) == 0);
    check("a decrypted packet has its scrambling bits cleared",
          (stream[3] & 0xc0u) == 0);
    check("odd parity left untouched",
          (stream[2 * 188 + 3] & 0xc0u) == 0xc0u);

    /* Now the odd word as well. */
    dtv_csa_set_keys(csa, even_cw, 1, odd_cw, 1);
    decrypted = dtv_csa_decrypt_run(csa, stream, PACKETS);
    check("two odd-parity packets decrypted", decrypted == 2);
    check("odd parity back to plaintext",
          memcmp(stream + 2 * 188 + 4, expected + 2 * 188 + 4, 184) == 0);
    check("odd parity with an adaptation field back to plaintext",
          memcmp(stream + 3 * 188 + 7, expected + 3 * 188 + 7, 181) == 0);

    /* Headers must come through untouched -- pid, continuity counter and
     * the adaptation field itself are not part of the ciphertext. */
    for (i = 0; i < PACKETS; ++i) {
        char label[64];
        int header_ok = stream[i * 188] == 0x47 &&
                        stream[i * 188 + 1] == expected[i * 188 + 1] &&
                        stream[i * 188 + 2] == expected[i * 188 + 2];
        snprintf(label, sizeof(label), "packet %u header kept",
                 (unsigned)i);
        check(label, header_ok);
    }

    /* Scrambling bits set but no payload: nothing to decrypt, and the flag
     * must not survive into a stream handed over in the clear. Without a
     * key it stays, because then the service really is scrambled. */
    {
        uint8_t lonely[188];
        memset(lonely, 0xff, sizeof(lonely));
        lonely[0] = 0x47;
        lonely[1] = 0x01;
        lonely[2] = 0x23;
        lonely[3] = 0xa0;      /* even parity, adaptation only */
        lonely[4] = 183;       /* the field fills the packet */
        dtv_csa_clear_keys(csa);
        dtv_csa_decrypt_run(csa, lonely, 1);
        check("without a key a packet with no payload is left alone",
              (lonely[3] & 0xc0u) == 0x80u);
        dtv_csa_set_keys(csa, even_cw, 1, odd_cw, 1);
        check("a packet with no payload is not counted as decrypted",
              dtv_csa_decrypt_run(csa, lonely, 1) == 0);
        check("a packet with no payload has its scrambling bits cleared",
              (lonely[3] & 0xc0u) == 0);
    }

    /* A run that is already in the clear must be a no-op. */
    decrypted = dtv_csa_decrypt_run(csa, stream, PACKETS);
    check("plaintext is not decrypted a second time", decrypted == 0);

    dtv_csa_clear_keys(csa);
    check("keys cleared", !dtv_csa_has_key(csa));
    dtv_csa_destroy(csa);

    if (failures)
        printf("\n%d tests failed.\n", failures);
    else
        printf("\nAll CSA tests passed.\n");
    return failures ? 1 : 0;
}
