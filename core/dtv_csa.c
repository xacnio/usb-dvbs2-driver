/* DVB-CSA descrambling on top of libdvbcsa's bitslice interface. A batch
 * shares one key, so packets are sorted into an even and an odd batch as
 * they are seen. Order survives because decryption happens in place: the
 * run is walked once to fill the batches, the batches are flushed, and the
 * caller reads the same memory back. */
#include "dtv_csa.h"

#include <stdlib.h>
#include <string.h>

#include "dvbcsa/dvbcsa.h"

struct dtv_csa_batch {
    struct dvbcsa_bs_key_s *key;
    struct dvbcsa_bs_batch_s *slots;   /* batch_size + 1, NULL terminated */
    unsigned count;
    int valid;
};

struct dtv_csa {
    struct dtv_csa_batch even;
    struct dtv_csa_batch odd;
    unsigned batch_size;
};

static int batch_create(struct dtv_csa_batch *batch, unsigned batch_size)
{
    batch->key = dvbcsa_bs_key_alloc();
    if (!batch->key)
        return -1;
    batch->slots = (struct dvbcsa_bs_batch_s *)
        calloc(batch_size + 1u, sizeof(*batch->slots));
    if (!batch->slots) {
        dvbcsa_bs_key_free(batch->key);
        batch->key = NULL;
        return -1;
    }
    batch->count = 0;
    batch->valid = 0;
    return 0;
}

static void batch_destroy(struct dtv_csa_batch *batch)
{
    if (batch->key)
        dvbcsa_bs_key_free(batch->key);
    free(batch->slots);
    batch->key = NULL;
    batch->slots = NULL;
}

static void batch_flush(struct dtv_csa_batch *batch)
{
    if (!batch->count)
        return;
    batch->slots[batch->count].data = NULL;
    batch->slots[batch->count].len = 0;
    /* 184 is the largest payload a packet can carry; libdvbcsa wants the
     * maximum, not the per-packet lengths, which are in the slots. */
    dvbcsa_bs_decrypt(batch->key, batch->slots, 184);
    batch->count = 0;
}

dtv_csa *dtv_csa_create(void)
{
    dtv_csa *csa = (dtv_csa *)calloc(1, sizeof(*csa));
    if (!csa)
        return NULL;
    csa->batch_size = (unsigned)dvbcsa_bs_batch_size();
    if (batch_create(&csa->even, csa->batch_size) != 0 ||
        batch_create(&csa->odd, csa->batch_size) != 0) {
        batch_destroy(&csa->even);
        batch_destroy(&csa->odd);
        free(csa);
        return NULL;
    }
    return csa;
}

void dtv_csa_destroy(dtv_csa *csa)
{
    if (!csa)
        return;
    batch_destroy(&csa->even);
    batch_destroy(&csa->odd);
    free(csa);
}

void dtv_csa_set_keys(dtv_csa *csa, const uint8_t even[8], int even_valid,
                      const uint8_t odd[8], int odd_valid)
{
    if (!csa)
        return;
    /* Anything still queued belongs to the previous words. */
    batch_flush(&csa->even);
    batch_flush(&csa->odd);
    if (even_valid) {
        dvbcsa_bs_key_set(even, csa->even.key);
        csa->even.valid = 1;
    } else {
        csa->even.valid = 0;
    }
    if (odd_valid) {
        dvbcsa_bs_key_set(odd, csa->odd.key);
        csa->odd.valid = 1;
    } else {
        csa->odd.valid = 0;
    }
}

void dtv_csa_clear_keys(dtv_csa *csa)
{
    if (!csa)
        return;
    csa->even.count = 0;
    csa->odd.count = 0;
    csa->even.valid = 0;
    csa->odd.valid = 0;
}

int dtv_csa_has_key(const dtv_csa *csa)
{
    return csa && (csa->even.valid || csa->odd.valid);
}

size_t dtv_csa_decrypt_run(dtv_csa *csa, uint8_t *packets, size_t count)
{
    size_t index, decrypted = 0;

    if (!csa || !packets)
        return 0;

    for (index = 0; index < count; ++index) {
        uint8_t *packet = packets + index * 188u;
        unsigned scrambling = (unsigned)(packet[3] >> 6);
        unsigned adaptation = (unsigned)((packet[3] >> 4) & 3u);
        struct dtv_csa_batch *batch;
        size_t offset;

        if (packet[0] != 0x47u || scrambling < 2u)
            continue;   /* not scrambled, or not a packet at all */

        /* Bit 0 of the scrambling control is the parity: 0b10 even,
         * 0b11 odd. */
        batch = (scrambling & 1u) ? &csa->odd : &csa->even;
        if (!batch->valid)
            continue;   /* no key for this parity yet -- leave it alone */

        if (adaptation == 1u) {
            offset = 4u;
        } else if (adaptation == 3u) {
            offset = 5u + packet[4];
            if (offset >= 188u) {
                /* Adaptation field fills the packet: nothing to decrypt.
                 * Broadcasters do set the scrambling bits on payload-less
                 * packets (428 per five seconds on a live BISS service),
                 * and leaving the flag on tells the player something
                 * untrue. */
                packet[3] &= 0x3fu;
                continue;
            }
        } else {
            packet[3] &= 0x3fu;   /* no payload; see above */
            continue;
        }

        /* Only the payload is enciphered, and the scrambling bits have to
         * be cleared for the player; doing it here rather than after the
         * flush keeps the packet visited once. */
        packet[3] &= 0x3fu;

        batch->slots[batch->count].data = packet + offset;
        batch->slots[batch->count].len = (unsigned)(188u - offset);
        if (++batch->count >= csa->batch_size)
            batch_flush(batch);
        ++decrypted;
    }

    batch_flush(&csa->even);
    batch_flush(&csa->odd);
    return decrypted;
}
