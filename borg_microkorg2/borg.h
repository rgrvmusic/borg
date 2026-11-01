/**
 * @file borg.h
 * @brief The two sides of borg on the microKORG2: the switch, and the panel.
 *
 * Init() decides which of three states this image is in -- ready, switched, or
 * refused -- and schedules the ready banner. Nothing is switched in Init: on
 * this platform a specific note is the request, and a note only exists in the
 * oscillator runtime context that Render is given.
 *
 * Render() silences the eight outputs, watches that context for the activation
 * note, and -- once the shim is watching the panel stream -- turns the
 * FUNCTION 3 + FUNCTION 5 chord into the four-button menu, whose labels it
 * writes into the panel's soft-key row. The shim withholds that chord from
 * microkorgd, whose own FUNCTION 5 opens the INIT option screen; without this
 * the chord would open both.
 *
 * Split from unit.cc so the glue stays the callbacks the SDK expects, and so
 * the panel frames have one home: borg.cc.
 */

#ifndef BORG_MICROKORG2_BORG_H
#define BORG_MICROKORG2_BORG_H

#include <cstdint>

namespace borg {

/** Called from unit_init. Checks the shim's pid marker and decides the state;
 *  never switches. */
void Init(uint32_t samplerate);

/** Called from unit_teardown: the shim keeps a pointer into this image and has
 *  to be told before the image can be unmapped. Safe when there is no shim. */
void Teardown();

/** Called from unit_render. `context` is desc->hooks.runtime_context, or null
 *  if the runtime did not provide one. Opaque here because unit_osc.h defines
 *  its helpers at file scope and would duplicate them if both translation
 *  units included it; borg.cc casts it to unit_runtime_osc_context_t. */
void Render(float* out, uint32_t frames, const void* context);

/** STATE readout: "!ready", "!switch", "!active" or "!err" -- or, for a few
 *  seconds after a menu action, that action's result. */
const char* StateText();

}  // namespace borg

#endif
