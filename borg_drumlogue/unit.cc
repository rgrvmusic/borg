// SDK callbacks for the Borg unit.

#include <cstdint>

#include "borg.h"
#include "unit.h"

namespace {

enum { kParamState = 0 };

}  // namespace

__unit_callback int8_t unit_init(const unit_runtime_desc_t* desc) {
  if (!desc)
    return k_unit_err_undef;

  if (desc->target != unit_header.target)
    return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api))
    return k_unit_err_api_version;

  /* Render always writes stereo output. */
  if (desc->output_channels != 2)
    return k_unit_err_geometry;

  borg::Init(desc->samplerate);
  return k_unit_err_none;
}

__unit_callback void unit_teardown() {}

__unit_callback void unit_reset() {}

__unit_callback void unit_suspend() {}

__unit_callback void unit_resume() {}

__unit_callback void unit_render(const float* in, float* out, uint32_t frames) {
  (void)in;
  borg::Render(out, frames);
}

__unit_callback void unit_set_param_value(uint8_t id, int32_t value) {
  (void)id;
  (void)value;
}

__unit_callback int32_t unit_get_param_value(uint8_t id) {
  (void)id;
  return 0;
}

__unit_callback const char* unit_get_param_str_value(uint8_t id,
                                                     int32_t value) {
  (void)value;
  return id == kParamState ? borg::StateTag() : nullptr;
}

__unit_callback const uint8_t* unit_get_param_bmp_value(uint8_t id,
                                                        int32_t value) {
  (void)id;
  (void)value;
  return nullptr;
}

__unit_callback void unit_set_tempo(uint32_t tempo) { (void)tempo; }

__unit_callback void unit_note_on(uint8_t note, uint8_t velocity) {
  (void)note;
  (void)velocity;
  borg::Request();
}

__unit_callback void unit_note_off(uint8_t note) { (void)note; }

__unit_callback void unit_gate_on(uint8_t velocity) {
  (void)velocity;
  borg::Request();
}

__unit_callback void unit_gate_off() {}

__unit_callback void unit_all_note_off() {}

__unit_callback void unit_pitch_bend(uint16_t bend) { (void)bend; }

__unit_callback void unit_channel_pressure(uint8_t pressure) {
  (void)pressure;
}

__unit_callback void unit_aftertouch(uint8_t note, uint8_t aftertouch) {
  (void)note;
  (void)aftertouch;
}

__unit_callback uint8_t unit_get_preset_index() { return 0; }

__unit_callback const char* unit_get_preset_name(uint8_t idx) {
  (void)idx;
  return nullptr;
}

__unit_callback void unit_load_preset(uint8_t idx) { (void)idx; }
