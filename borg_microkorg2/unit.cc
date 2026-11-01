/**
 * @file unit.cc
 * @brief microKORG2 SDK unit interface for borg.
 *
 * unit_init only decides the state; the switch is asked for by a note and done
 * from unit_render. That is not the drumlogue rule being bent -- it is the same
 * shape borg_drumlogue uses, where unit_note_on calls Request and the exec
 * happens on the render callback -- reached through the only interface this
 * platform gives an oscillator: there is no note callback here, but the
 * oscillator runtime context carries pitch[8] and a trigger bit array, and
 * unit_render is handed it.
 *
 * The panel is painted from unit_render too: drawing is not part of the switch,
 * a paint on the audio callback can be repeated, and it is how the popup is put
 * back after the daemon's own redraws.
 *
 * The unit is silent. An osc unit is handed eight output channels; every one is
 * written to zero on every render, so the unit adds nothing to the mix.
 */

#include <cstdint>

#include "unit.h"
#include "runtime.h"   /* UNIT_TARGET_PLATFORM, UNIT_API_IS_COMPAT */
#include "borg.h"      /* the switch, the paint and the chord */

namespace {

/** Param index, matching header.c. */
enum { kParamState = 0 };

/* The runtime calls unit_render while the unit is selected, not only while it
 * sounds: waves.h's Suspend comment says another synth being selected is what
 * stops the callback, not silence. So the ready screen paints at the first
 * callback, and the activation note is seen on a later one. */
uint32_t g_rate = 48000;

/* desc->hooks.runtime_context, which is the oscillator context on this
 * platform. It points into the runtime's own storage and stays valid while the
 * unit is loaded. borg.cc casts it. */
const void* g_context = nullptr;

}  // namespace

__unit_callback int8_t unit_init(const unit_runtime_desc_t* desc) {
  if (!desc)
    return k_unit_err_undef;

  if (desc->target != unit_header.target)
    return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api))
    return k_unit_err_api_version;

  /* 0 in, 8 out. An osc unit is handed one frame of eight voices; the check is
   * what makes the silence in unit_render safe. */
  if (desc->input_channels != 0 || desc->output_channels != 8)
    return k_unit_err_geometry;

  g_rate = desc->samplerate != 0 ? desc->samplerate : 48000;
  g_context = desc->hooks.runtime_context;

  /* Nothing is switched here: this only checks whether this image already is
   * the one running under the shim. The switch is the activation note, seen in
   * unit_render. */
  borg::Init(g_rate);
  return k_unit_err_none;
}

/* The shim holds a pointer into this image; this is where it is given up. */
__unit_callback void unit_teardown() { borg::Teardown(); }

__unit_callback void unit_reset() {}

__unit_callback void unit_suspend() {}

__unit_callback void unit_resume() {}

__unit_callback void unit_render(const float* in, float* out, uint32_t frames) {
  (void)in;
  borg::Render(out, frames, g_context);
}

/** Nothing here, deliberately: STATE is a readout, so a value arriving for it
 *  says nothing the unit wants to hear. */
__unit_callback void unit_set_param_value(uint8_t id, int32_t value) {
  (void)id;
  (void)value;
}

/** No value is meaningful for a readout, and the host is not asked for one. */
__unit_callback int32_t unit_get_param_value(uint8_t id) {
  (void)id;
  return 0;
}

/** The report, at every position. */
__unit_callback const char* unit_get_param_str_value(uint8_t id,
                                                     int32_t value) {
  (void)value;
  return id == kParamState ? borg::StateText() : nullptr;
}

__unit_callback const uint8_t* unit_get_param_bmp_value(uint8_t id,
                                                        int32_t value) {
  (void)id;
  (void)value;
  return nullptr;
}

/* The callbacks below are not part of the osc interface on this platform --
 * platform/common/unit.h declares them, and _unit_base.c carries a weak no-op
 * for each. They are defined to match every other unit here and to make the
 * intent explicit: this unit makes no sound, and none of these does anything. */
__unit_callback void unit_set_tempo(uint32_t tempo) { (void)tempo; }

__unit_callback void unit_note_on(uint8_t note, uint8_t velocity) {
  (void)note;
  (void)velocity;
}

__unit_callback void unit_note_off(uint8_t note) { (void)note; }

__unit_callback void unit_gate_on(uint8_t velocity) { (void)velocity; }

__unit_callback void unit_gate_off() {}

__unit_callback void unit_all_note_off() {}

__unit_callback uint8_t unit_get_preset_index() { return 0; }

__unit_callback const char* unit_get_preset_name(uint8_t idx) {
  (void)idx;
  return nullptr;
}

__unit_callback void unit_load_preset(uint8_t idx) { (void)idx; }
