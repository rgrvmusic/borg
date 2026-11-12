/**
 * @file header.c
 * @brief Unit header for borg on the microKORG2.
 *
 * One readout and no controls:
 *
 *   0  STATE   !ready  waiting for a C to switch the daemon
 *              !switch the switch is being made
 *              !active this image is the switched one, running under the shim
 *              !err    a switch was asked for and refused; see the log
 *
 * The trigger is a note, not a parameter. A parameter cannot be the trigger on
 * this platform: the host pushes a program's saved values in after unit_init
 * returns, so a parameter shows what was *set*, never what was *asked* -- the
 * lesson rgdump's header.c records. The note arrives in the oscillator runtime
 * context on the render callback, which is the only place this interface
 * exposes it.
 *
 * `.name` is four characters. UNIT_NAME_LEN here is 8, against 13 on the
 * drumlogue, and the array is LEN+1.
 *
 * `.dev_id` stays RGEX. It is the id this unit has been loaded under on the
 * instrument, and a new id is a load refusal before main on a machine with no
 * console -- see the Developer Version gate. Change it only together with a
 * gate entry, never on its own.
 *
 * The page split is dummy-osc's 3/5/5, the only grouping known to draw
 * correctly on this device.
 */

#include "unit.h"
#include "runtime.h"   /* UNIT_TARGET_PLATFORM, UNIT_API_VERSION */

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target      = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api         = UNIT_API_VERSION,
    .dev_id      = 0x52474558U,   /* 'RGEX' */
    .unit_id     = 0x02U,
    .version     = 0x00010000U,
    .name        = "Borg",
    .num_presets = 0,
    .num_params  = 1,
    .params = {
        /* 0 STATE — what happened, which is decided before this can be read:
         * the switch is the whole of unit_init. */
        {0, 63, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"STATE"}},

        /* Page 1 (3 params), then page 2 (5) and page 3 (5): 13 slots in all.
         * Every one after the first is unused and has to be present -- the host
         * reads params in order, so a hole is a parameter whose name is
         * whatever follows it in .rodata. */
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},

        /* Page 2: unused */
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},

        /* Page 3: unused */
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}}
    }
};
