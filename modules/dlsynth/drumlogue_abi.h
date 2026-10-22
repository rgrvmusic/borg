/**
 * drumlogue_abi.h — the drumlogue unit ABI, carried by this module.
 *
 * A drumlogue unit is an ordinary armhf Linux shared object that exports
 * `unit_header` plus the `unit_*` callbacks, and is handed a packed 26-byte
 * `unit_runtime_desc_t` at init. This module loads those files itself, so it
 * needs those declarations and nothing else of the host's.
 *
 * Why a local copy rather than an include path: a `.borgmodule` is built by
 * `../../../build-module.sh`, which adds exactly one include directory,
 * `libborg/`. The ABI declarations are not host services -- the module speaks
 * to the unit directly -- so they belong here, where that one include finds
 * them, and the module builds from the borg checkout alone. The definitions are
 * a hand-written copy of
 * logue-sdk/platform/drumlogue/common/{runtime.h,unit.h,sample_wrapper.h}, the
 * same copy (and the same checks) as the host's, kept to what a loader needs.
 *
 * The asserts matter more here than they look. A drumlogue unit is 32-bit
 * armhf; a 64-bit build moves `sample_ptr` to 48 and every field after it, and
 * the failure is a unit reading its own name as a pointer. The asserts turn
 * that into a compile error.
 */

#ifndef DLSYNTH_DRUMLOGUE_ABI_H
#define DLSYNTH_DRUMLOGUE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNIT_MAX_PARAM_COUNT (24)
#define UNIT_PARAM_NAME_LEN (12)
#define UNIT_NAME_LEN (13)
#define UNIT_SAMPLE_WRAPPER_MAX_NAME_LEN (31)

/** k_unit_api_2_0_0 — what every unit built against the current SDK declares. */
#define UNIT_API_VERSION_2_0_0 (0x00020000u)

/* The one target this module loads: a synth, as opposed to a delay, reverb or
 * master effect. An effect has no note callbacks, so playing it over MIDI is
 * not something it can do. */
#define k_unit_target_drumlogue (4U << 8)
#define k_unit_target_drumlogue_synth (k_unit_target_drumlogue | 5U)

#pragma pack(push, 1)

/** One parameter descriptor. 23 bytes, not the 24 the SDK comment claims. */
typedef struct unit_param {
  int16_t min;
  int16_t max;
  int16_t center;
  int16_t init;
  uint8_t type;
  uint8_t frac : 4;
  uint8_t frac_mode : 1;
  uint8_t reserved : 3;
  char name[UNIT_PARAM_NAME_LEN + 1];
} unit_param_t;

/** The unit's exported metadata. `params` begins at offset 44. */
typedef struct unit_header {
  uint32_t header_size;
  uint16_t target;
  uint32_t api;
  uint32_t dev_id;
  uint32_t unit_id;
  uint32_t version;
  char name[UNIT_NAME_LEN + 1];
  uint32_t num_presets;
  uint32_t num_params;
  unit_param_t params[UNIT_MAX_PARAM_COUNT];
} unit_header_t;

#pragma pack(pop)

/**
 * A sample served to the unit by the host.
 *
 * NOT packed -- `sample_wrapper.h` has no pragma pack, so this takes natural
 * EABI alignment and is 44 bytes on 32-bit. `frames` is a size_t: at 64-bit it
 * would be 8 bytes, `sample_ptr` would move to 48, and the unit would read both
 * fields from the wrong place.
 */
typedef struct sample_wrapper {
  uint8_t bank;
  uint8_t index;
  uint8_t channels;
  uint8_t _padding;
  char name[UNIT_SAMPLE_WRAPPER_MAX_NAME_LEN + 1];
  size_t frames;
  const float *sample_ptr;
} sample_wrapper_t;

typedef uint8_t (*unit_runtime_get_num_sample_banks_ptr)(void);
typedef uint8_t (*unit_runtime_get_num_samples_for_bank_ptr)(uint8_t);
typedef const sample_wrapper_t *(*unit_runtime_get_sample_ptr)(uint8_t, uint8_t);

/** Packed, 26 bytes on arm32. The only thing the host passes to unit_init. */
#pragma pack(push, 1)
typedef struct unit_runtime_desc {
  uint16_t target;                 /*  0 */
  uint32_t api;                    /*  2 */
  uint32_t samplerate;             /*  6 */
  uint16_t frames_per_buffer;      /* 10 */
  uint8_t input_channels;          /* 12 */
  uint8_t output_channels;         /* 13 */
  unit_runtime_get_num_sample_banks_ptr get_num_sample_banks;        /* 14 */
  unit_runtime_get_num_samples_for_bank_ptr get_num_samples_for_bank; /* 18 */
  unit_runtime_get_sample_ptr get_sample;                            /* 22 */
} unit_runtime_desc_t;
#pragma pack(pop)

typedef int8_t (*unit_init_ptr)(const unit_runtime_desc_t *);
typedef void (*unit_teardown_ptr)(void);
typedef void (*unit_resume_ptr)(void);
typedef void (*unit_suspend_ptr)(void);
typedef void (*unit_render_ptr)(const float *, float *, uint32_t);
typedef void (*unit_set_param_value_ptr)(uint8_t, int32_t);
typedef void (*unit_note_on_ptr)(uint8_t, uint8_t);
typedef void (*unit_note_off_ptr)(uint8_t);
typedef void (*unit_all_note_off_ptr)(void);
typedef void (*unit_pitch_bend_ptr)(uint16_t);
typedef void (*unit_channel_pressure_ptr)(uint8_t);
typedef void (*unit_aftertouch_ptr)(uint8_t, uint8_t);

#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ != 4)
#error "dlsynth loads 32-bit armhf units and reads their headers: build it with the logue SDK's arm-linux-gnueabihf compiler."
#endif

#if !defined(__SIZEOF_POINTER__) || (__SIZEOF_POINTER__ == 4)

#define DL_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

DL_STATIC_ASSERT(sizeof(unit_param_t) == 23, "unit_param_t must be 23 bytes");
DL_STATIC_ASSERT(sizeof(unit_header_t) == 596, "unit_header_t must be 596 bytes");
DL_STATIC_ASSERT(sizeof(unit_runtime_desc_t) == 26, "runtime desc must be 26");
DL_STATIC_ASSERT(sizeof(sample_wrapper_t) == 44, "sample_wrapper_t must be 44");

DL_STATIC_ASSERT(offsetof(unit_header_t, name) == 22, "header name offset");
DL_STATIC_ASSERT(offsetof(unit_header_t, num_params) == 40, "num_params offset");
DL_STATIC_ASSERT(offsetof(unit_header_t, params) == 44, "params offset is 44");
DL_STATIC_ASSERT(offsetof(unit_param_t, type) == 8, "param type offset");
DL_STATIC_ASSERT(offsetof(unit_param_t, name) == 10, "param name offset");
DL_STATIC_ASSERT(offsetof(sample_wrapper_t, frames) == 36, "wrapper frames offset");
DL_STATIC_ASSERT(offsetof(sample_wrapper_t, sample_ptr) == 40,
                 "wrapper sample_ptr offset");
DL_STATIC_ASSERT(offsetof(unit_runtime_desc_t, get_sample) == 22,
                 "desc get_sample offset");

#endif

#ifdef __cplusplus
}
#endif

#endif /* DLSYNTH_DRUMLOGUE_ABI_H */
