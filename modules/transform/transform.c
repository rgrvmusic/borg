/* transform — the MIDI note transform, as a module with a page of its own.
 *
 * This used to live in the resident object, set from
 * LOGUE_BORG_MIDI_TRANSPOSE and LOGUE_BORG_MIDI_VELOCITY. Nothing on the
 * instrument can set an environment variable for the daemon: the daemon is
 * started by the instrument's own init before anyone could type anything, and
 * borg's re-exec carries that same environment. So the feature existed and
 * could not be reached. It is a module now, and the two settings are encoders
 * on its page.
 *
 * What a transform module gets that a MIDI_EVENTS module does not is the
 * daemon's own copy of the message, still writable. The host calls this before
 * the daemon's unit renders the note and before any midi_msg module sees it,
 * so changing a byte here changes what is played -- which is the whole point,
 * and why the capability is separate from observation.
 *
 * The transform is deliberately note-only. A synth unit's parameters are not a
 * fixed CC map, so scaling or shifting a controller number would be a guess
 * that happens to sound like a feature. Note and velocity are the two that the
 * drumlogue's own note path defines. */

#include <stdio.h>

#include "borg_module.h"

static const borg_host* g_host;

/* Written on the panel thread, read on the MIDI thread. The host serializes
 * every module callback except audio_render, so these need no atomics. */
static int g_transpose;             /* semitones, -24 to +24 */
static int g_percent = 100;         /* note-on velocity, 5 to 200 percent */
static unsigned long g_changed;     /* messages this module actually rewrote */

static int transform_init(const borg_host* host, const char* module_path) {
  (void)module_path;
  g_host = host;
  if (host->log != NULL) host->log("transform: ready");
  return 0;
}

static void transform_msg(void* port, unsigned char* data, size_t n) {
  unsigned char status;
  int note, vel, changed = 0;

  (void)port;
  /* A note message is three bytes. Two-byte messages (program change,
   * channel pressure) are not notes, and the host passes everything. */
  if (data == NULL || n < 3) return;
  status = data[0] & 0xf0;
  if (status != 0x90 && status != 0x80) return;

  note = (int)data[1] + g_transpose;
  if (note < 0) note = 0;
  if (note > 127) note = 127;
  if (note != (int)data[1]) {
    data[1] = (unsigned char)note;
    changed = 1;
  }

  /* Velocity 0 on 0x9n is a note-off, and stays one: scaling it to 1 would
   * turn every release into a note-on. */
  if (status == 0x90 && data[2] > 0 && g_percent != 100) {
    vel = ((int)data[2] * g_percent + 50) / 100;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;
    if (vel != (int)data[2]) {
      data[2] = (unsigned char)vel;
      changed = 1;
    }
  }
  if (changed) __atomic_add_fetch(&g_changed, 1, __ATOMIC_RELAXED);
}

static void transform_encoder(int index, int delta) {
  if (index == 0) {
    g_transpose += delta;
    if (g_transpose < -24) g_transpose = -24;
    if (g_transpose > 24) g_transpose = 24;
  } else if (index == 1) {
    g_percent += delta * 5;
    if (g_percent < 5) g_percent = 5;
    if (g_percent > 200) g_percent = 200;
  }
}

static void transform_page(void) {
  char value[16];

  if (g_host == NULL || g_host->row == NULL) return;
  snprintf(value, sizeof value, "%+d", g_transpose);
  g_host->row(26, "TRANSPOSE:", value);
  snprintf(value, sizeof value, "%d%%", g_percent);
  g_host->row(35, "VELOCITY:", value);
  snprintf(value, sizeof value, "%lu",
           __atomic_load_n(&g_changed, __ATOMIC_RELAXED));
  g_host->row(44, "TRANSFORMED:", value);
  g_host->row(53, "ENCODERS:", "TRANS/VEL");
}

/* Designated, not positional: the table grows at the end with each ABI, and a
 * missing field would silently become NULL rather than fail to compile. */
static const borg_module transform_module = {
  .abi = BORG_MODULE_ABI,
  .name = "XFORM",
  .version = "1.0.0",
  .capabilities = BORG_CAP_PAGE | BORG_CAP_INPUT | BORG_CAP_MIDI_TRANSFORM,
  .init = transform_init,
  .page_draw = transform_page,
  .encoder = transform_encoder,
  .midi_transform = transform_msg,
};

const borg_module* borg_module_entry(uint32_t host_abi) {
  /* Without ABI 5 there is no writable message to transform, and the page
   * would be two settings that change nothing. */
  if (host_abi < 5) return 0;
  return &transform_module;
}
