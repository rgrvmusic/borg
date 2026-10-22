/* transform.borgmodule's tests, run on a development machine.
 *
 * The module is built here for this machine and driven through the same
 * borg_module table the daemon reads: the entry's ABI gate, init, the
 * encoders, the page, and the writable message that is the whole reason this
 * is a module. The host in this file collects rows and log lines; nothing here
 * is a stub of the module's own logic.
 *
 * What it cannot reproduce is the daemon. That this callback is called before
 * the daemon's unit is the host's side of the contract and is not exercised
 * here; what is exercised is that the bytes a caller hands in are the bytes it
 * gets back, changed the way the page says.
 *
 * Build and run with test/run-native.sh. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "borg_module.h"

static int checks;
static int failures;

static void Check(int cond, const char* what) {
  checks++;
  if (cond) return;
  failures++;
  fprintf(stderr, "FAIL: %s\n", what);
}

static char g_row_label[64][32];
static char g_row_value[64][32];

static void HostLog(const char* line) { (void)line; }

static const char* HostPath(int which) {
  (void)which;
  return "/tmp";
}

static void HostRow(unsigned y, const char* label, const char* value) {
  if (y >= 64) return;
  snprintf(g_row_label[y], sizeof g_row_label[y], "%s",
           label != NULL ? label : "");
  snprintf(g_row_value[y], sizeof g_row_value[y], "%s",
           value != NULL ? value : "");
}

static const char* RowValue(const borg_module* m, unsigned y) {
  m->page_draw();
  return y < 64 ? g_row_value[y] : "";
}

static void Send(const borg_module* m, unsigned char* msg, size_t n) {
  m->midi_transform(NULL, msg, n);
}

int main(void) {
  borg_host host;
  const borg_module* m;
  unsigned char note[3];
  unsigned char off[3];
  unsigned char cc[3];
  unsigned char program[2];

  memset(&host, 0, sizeof host);
  host.size = sizeof host;
  host.abi = BORG_MODULE_ABI;
  host.log = HostLog;
  host.path = HostPath;
  host.row = HostRow;
  host.samplerate = 48000;

  /* The module is only meaningful where the message is writable. */
  Check(borg_module_entry(4) == NULL, "an ABI 4 host is refused");
  m = borg_module_entry(5);
  Check(m != NULL, "an ABI 5 host is accepted");
  if (m == NULL) return 1;
  Check(m->abi == 5, "the module declares ABI 5");
  Check((m->capabilities & BORG_CAP_MIDI_TRANSFORM) != 0,
        "the module asks for the transform");
  Check(m->midi_transform != NULL, "the transform callback is present");
  Check(m->encoder != NULL, "the page has encoder input");
  Check(m->init(&host, "/test/transform.borgmodule") == 0, "init succeeds");

  /* At the defaults the message is untouched: installing the module must not
   * change what the instrument plays. */
  note[0] = 0x90; note[1] = 60; note[2] = 100;
  Send(m, note, 3);
  Check(note[1] == 60 && note[2] == 100, "the defaults pass a note through");
  Check(strcmp(RowValue(m, 44), "0") == 0, "nothing was counted as transformed");

  /* Transpose, and a clamp at the top of the MIDI range. */
  m->encoder(0, 12);
  note[0] = 0x90; note[1] = 60; note[2] = 100;
  Send(m, note, 3);
  Check(note[1] == 72, "encoder 1 transposes up 12 semitones");
  Check(note[2] == 100, "transpose leaves velocity alone");
  Check(strcmp(RowValue(m, 26), "+12") == 0, "the page shows the transpose");

  note[1] = 100;
  m->encoder(0, 20);                   /* clamps at +24, not +32 */
  note[0] = 0x90;
  Send(m, note, 3);
  Check(note[1] == 124, "the transpose is clamped to +24");
  m->encoder(0, -100);
  note[0] = 0x90; note[1] = 10;
  Send(m, note, 3);
  Check(note[1] == 0, "the transpose is clamped to -24 at note 0");

  /* A note-off is the same note, transposed, so the right voice stops. */
  m->encoder(0, 100);                  /* -24 clamps at +24 */
  off[0] = 0x80; off[1] = 60; off[2] = 0;
  Send(m, off, 3);
  Check(off[1] == 84, "a note-off is transposed by the same amount");

  /* Velocity scaling, and the 0x9n velocity 0 that is a note-off. */
  m->encoder(0, -24);                  /* +24 back to 0 */
  m->encoder(1, 4);                    /* 100 -> 120 percent */
  note[0] = 0x90; note[1] = 60; note[2] = 100;
  Send(m, note, 3);
  Check(note[2] == 120, "encoder 2 scales note-on velocity");
  Check(strcmp(RowValue(m, 35), "120%") == 0, "the page shows the velocity");

  note[0] = 0x90; note[1] = 60; note[2] = 0;
  Send(m, note, 3);
  Check(note[2] == 0, "velocity 0 stays a note-off");

  /* Everything that is not a note goes through untouched, including the
   * two-byte messages, which have no velocity byte to scale. */
  cc[0] = 0xb0; cc[1] = 7; cc[2] = 100;
  Send(m, cc, 3);
  Check(cc[1] == 7 && cc[2] == 100, "a controller message is untouched");
  program[0] = 0xc0; program[1] = 5;
  Send(m, program, 2);
  Check(program[1] == 5, "a program change is untouched");

  /* The count is what the page uses to say the module is doing something. */
  Check(strcmp(RowValue(m, 44), "5") == 0,
        "only the messages actually rewritten are counted");

  /* Below the note range the envelope is 0 to 127, never negative. */
  m->encoder(0, -100);
  note[0] = 0x90; note[1] = 0; note[2] = 100;
  Send(m, note, 3);
  Check(note[1] == 0, "a note below 0 is clamped, not wrapped");

  printf("%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
