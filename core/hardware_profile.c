#include "hardware_profile.h"

#include <string.h>

static const dtv_hardware_profile g_profiles[] = {
    {
        DTV_HARDWARE_HIREMCO_IT9303_AVL6261_RDA5815M,
        "hiremco-it9303-avl6261-rda5815m",
        "Hiremco / SuperDTV UDTV (IT9303 + AVL6261 + RDA5815M)",
        0x048d, 0xf036,
        DTV_BRIDGE_IT9303, DTV_DEMOD_AVL62X1, DTV_TUNER_RDA5815M,
        "firmware/hiremco-it9303.fw",
        "firmware/hiremco-avl62x1.fw",
        3, 0x14, 0x0c, 1
    }
};

size_t dtv_hardware_profile_count(void)
{
    return sizeof(g_profiles) / sizeof(g_profiles[0]);
}

const dtv_hardware_profile *dtv_hardware_profile_at(size_t index)
{
    return index < dtv_hardware_profile_count() ? &g_profiles[index] : NULL;
}

const dtv_hardware_profile *dtv_hardware_profile_by_id(int id)
{
    size_t i;
    for (i = 0; i < dtv_hardware_profile_count(); ++i)
        if (g_profiles[i].id == id)
            return &g_profiles[i];
    return NULL;
}

const dtv_hardware_profile *dtv_hardware_profile_by_key(const char *key)
{
    size_t i;
    if (!key || strcmp(key, "auto") == 0)
        return NULL;
    for (i = 0; i < dtv_hardware_profile_count(); ++i)
        if (strcmp(g_profiles[i].key, key) == 0)
            return &g_profiles[i];
    return NULL;
}

const char *dtv_hardware_selection_key(int selection)
{
    const dtv_hardware_profile *profile =
        dtv_hardware_profile_by_id(selection);
    return profile ? profile->key : "auto";
}
