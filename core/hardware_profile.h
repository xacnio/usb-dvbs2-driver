#ifndef DTV_HARDWARE_PROFILE_H
#define DTV_HARDWARE_PROFILE_H

#include <stddef.h>
#include <stdint.h>

/* The UI stores AUTO or a concrete profile id. A profile describes the
 * whole USB -> bridge -> demodulator -> tuner chain, so adding a device
 * does not scatter VID/PIDs and firmware names through the daemon. */
enum dtv_hardware_profile_id {
    DTV_HARDWARE_AUTO = 0,
    DTV_HARDWARE_HIREMCO_IT9303_AVL6261_RDA5815M = 1,
    DTV_HARDWARE_PROFILE_COUNT
};

enum dtv_bridge_kind { DTV_BRIDGE_IT9303 = 1 };
enum dtv_demod_kind  { DTV_DEMOD_AVL62X1 = 1 };
enum dtv_tuner_kind  { DTV_TUNER_RDA5815M = 1 };

typedef struct dtv_hardware_profile {
    int id;
    const char *key;
    const char *name;
    uint16_t vid;
    uint16_t pid;
    enum dtv_bridge_kind bridge;
    enum dtv_demod_kind demod;
    enum dtv_tuner_kind tuner;
    const char *bridge_firmware;
    const char *demod_firmware;
    uint8_t bridge_i2c_bus;
    uint8_t demod_i2c_address;
    uint8_t tuner_i2c_address;
    uint8_t demod_reset_gpio;
} dtv_hardware_profile;

const dtv_hardware_profile *dtv_hardware_profile_by_id(int id);
const dtv_hardware_profile *dtv_hardware_profile_by_key(const char *key);
const dtv_hardware_profile *dtv_hardware_profile_at(size_t index);
size_t dtv_hardware_profile_count(void);
const char *dtv_hardware_selection_key(int selection);

#endif
