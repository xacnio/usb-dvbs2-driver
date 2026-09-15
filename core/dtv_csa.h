/* DVB-CSA descrambling of transport stream packets.
 *
 * Control words come from the CAM client (dtv_camd.h); this turns them
 * into cleartext packets. libdvbcsa does the cipher in its bitslice form,
 * decrypting a batch at once -- fast enough to run a whole transponder
 * through the same thread that keeps the USB endpoint drained.
 *
 * Packets whose parity has no key are left scrambled, bits intact: a
 * player shown undecrypted payload marked as clear renders noise, while
 * payload still marked scrambled is simply skipped. */
#ifndef DTV_CSA_H
#define DTV_CSA_H

#include <stddef.h>
#include <stdint.h>

typedef struct dtv_csa dtv_csa;

dtv_csa *dtv_csa_create(void);
void dtv_csa_destroy(dtv_csa *csa);

/* Installs new control words. Setting a key schedules it in libdvbcsa, so
 * call only when the words changed (see the generation counter in
 * dtv_camd.h). An invalid word disables that parity. */
void dtv_csa_set_keys(dtv_csa *csa, const uint8_t even[8], int even_valid,
                      const uint8_t odd[8], int odd_valid);

/* Forgets both keys, so a new service never runs through old words. */
void dtv_csa_clear_keys(dtv_csa *csa);

/* Decrypts in place every scrambled packet of an aligned 188 byte run,
 * clearing the scrambling bits of those it could do. Returns the count. */
size_t dtv_csa_decrypt_run(dtv_csa *csa, uint8_t *packets, size_t count);

/* True when at least one parity has a usable key. */
int dtv_csa_has_key(const dtv_csa *csa);

#endif
