/* The contract a .borgmodule is built against.
 *
 * A module is an ordinary ARM shared object loaded by the resident object in
 * drumlogued. It runs in the daemon's own process: no process and no thread is
 * created. A module is never unloaded while the daemon runs, because the code
 * could be called from the audio path.
 *
 * ABI 1 offers one menu page. ABI 2 appends MIDI: every input the daemon reads
 * is offered to a module that asks for it, and a reply goes out on the output
 * paired with the input the message arrived on. ABI 3 appends panel input for
 * the module's own page: the enter and leave edges, the buttons, and the four
 * endless encoders. ABI 4 appends audio and channel MIDI: a module that asks
 * for BORG_CAP_AUDIO is given a float stereo block on the daemon's playback
 * path, and one that asks for BORG_CAP_MIDI_EVENTS is offered every complete
 * channel message, with its status byte, before it reaches the daemon's own
 * unit. ABI 5 appends the MIDI transform: a module that asks for
 * BORG_CAP_MIDI_TRANSFORM is offered the same messages first, with writable
 * data bytes, so a note can be changed before anything plays it. Later ABIs
 * append more fields; the host reads the abi field first and never reads past
 * the layout that ABI names. Fields are only appended, never reordered or
 * removed.
 *
 * Build:
 *   ${CROSS_COMPILE}gcc -shared -fPIC -O2 -o hello.borgmodule hello.c
 *   ${CROSS_COMPILE}g++ -shared -fPIC -O2 -std=c++14 -o exec.borgmodule exec.cc
 *
 * Callbacks run on the daemon's own threads. page_draw runs on the panel
 * thread with the menu open; draw rows only, never clear the screen. init runs
 * once, off the audio path. midi_in and midi_msg run on the MIDI input thread
 * that read the message; tick runs on the panel thread, and also from a MIDI
 * read when one is happening. page_enter, page_leave, key and encoder run on
 * the panel thread. The host serializes all of those between modules, so a
 * module does not need a lock of its own. audio_render is the exception: it
 * runs on the audio thread and is not serialized with the others, because it
 * cannot wait. Nothing here may create a process or a thread.
 *
 * A complete SysEx message is handed to midi_in, F0 through F7 inclusive.
 * Realtime bytes inside it are dropped by the host. A message longer than the
 * host's buffer is cut short and still ends in F7, so a reader can refuse it
 * for its length rather than wait for an end that is not coming. */

#ifndef BORG_MODULE_H
#define BORG_MODULE_H

#include <stddef.h>
#include <stdint.h>

#define BORG_MODULE_ABI 5u
#define BORG_MODULE_ENTRY "borg_module_entry"
#define BORG_TAB_MAX 8          /* tab title: 7 characters plus NUL */

#define BORG_CAP_PAGE 0x0001u
#define BORG_CAP_MIDI 0x0002u   /* midi_in, and host->midi_write */
#define BORG_CAP_INPUT 0x0004u  /* key and encoder while this page is shown */
#define BORG_CAP_AUDIO 0x0008u  /* audio_render on the playback path */
#define BORG_CAP_MIDI_EVENTS 0x0010u /* midi_msg, every channel message */
#define BORG_CAP_MIDI_TRANSFORM 0x0020u /* midi_transform, before it is played */

#define BORG_PATH_PROGRAMS 0
#define BORG_PATH_TMP 1
#define BORG_PATH_UNITS 2       /* the daemon's Units/Synths directory */

typedef struct borg_host {
  uint32_t size;                 /* sizeof(borg_host): append-only */
  uint32_t abi;
  void (*log)(const char* line);
  const char* (*path)(int which);
  void (*text)(unsigned x, unsigned y, unsigned w, unsigned h, const char* s);
  void (*dark)(unsigned x, unsigned y, unsigned w, unsigned h, const char* s);
  void (*row)(unsigned y, const char* label, const char* value);
  /* ABI 2. `port` is the opaque input handle handed to midi_in; the reply goes
   * out on the output the daemon opened beside it. The call may wait briefly
   * for room in the output buffer, and returns 0 when every byte went out. */
  int (*midi_write)(void* port, const void* data, size_t n);
  /* ABI 2. The name the daemon opened the port with, such as `hw:2,0,0`, or
   * an empty string. The pointer stays valid while the daemon runs. */
  const char* (*port_name)(void* port);
  /* ABI 4. The rate of the playback stream the daemon opened, or 0 until one
   * has been seen. A module that initialises a unit should treat 0 as 48000,
   * which is the drumlogue's fixed rate. This field is written before the
   * first audio block and does not change afterwards. */
  uint32_t samplerate;
} borg_host;

typedef struct borg_module {
  uint32_t abi;                  /* BORG_MODULE_ABI */
  const char* name;              /* tab title, at most BORG_TAB_MAX-1 chars */
  const char* version;
  uint32_t capabilities;         /* BORG_CAP_* */
  int (*init)(const borg_host* host, const char* module_path);
  void (*page_draw)(void);       /* called while the module's page is active */
  /* ABI 2. One complete SysEx message, F0 through F7 inclusive. */
  void (*midi_in)(void* port, const unsigned char* data, size_t n);
  /* ABI 2. A cheap pulse, on the panel thread and on an active MIDI read. */
  void (*tick)(void);
  /* ABI 3, only when BORG_CAP_INPUT is set. The page became the active menu
   * page, or stopped being it. The module can reset a cursor here. */
  void (*page_enter)(void);
  void (*page_leave)(void);
  /* ABI 3. A panel button: the daemon's own key index, and 1 for a press, 0
   * for a release. PAGE UP, PAGE DOWN and the MUTE/MASTER chord stay with the
   * menu and are not offered. */
  void (*key)(int index, int down);
  /* ABI 3. An endless encoder: 0 to 3 are the knobs under the display, and
   * delta is the signed number of steps. Turning it is not offered while the
   * menu is on a page that does not ask for input. */
  void (*encoder)(int index, int delta);
  /* ABI 4, only when BORG_CAP_AUDIO is set. Add this module's contribution to
   * `out`, which is a zeroed stereo interleaved float buffer of `frames`
   * frames at host->samplerate. Runs on the audio thread: do not block,
   * allocate, lock, call host services, or touch anything the panel or MIDI
   * thread may be changing without an atomic. A module that has nothing to say
   * leaves the buffer alone. */
  void (*audio_render)(float* out, uint32_t frames);
  /* ABI 4, only when BORG_CAP_MIDI_EVENTS is set. One complete channel
   * message: status byte plus its one or two data bytes, after any
   * MIDI_TRANSFORM module has rewritten it. Running status and realtime bytes
   * are assembled or dropped by the host, so `n` is 2 or 3. `port` is the same
   * opaque handle midi_in gets. Runs on the MIDI thread, serialized with the
   * other callbacks except audio_render. */
  void (*midi_msg)(void* port, const unsigned char* data, size_t n);
  /* ABI 5, only when BORG_CAP_MIDI_TRANSFORM is set. One complete channel
   * message on its way to the daemon's own unit. `data` is writable: a module
   * may change the data bytes and the host hands the changed message on, both
   * to the daemon's unit and to every MIDI_EVENTS module. The status byte is
   * not to be changed -- it decides how many data bytes follow. Transform
   * modules run in load order, each seeing what the one before left, and
   * before every midi_msg. Runs on the MIDI thread, serialized with the other
   * callbacks except audio_render. A note message whose bytes were split
   * across two MIDI reads cannot be rewritten in the earlier read's buffer;
   * the host still transforms the copy the modules see. */
  void (*midi_transform)(void* port, unsigned char* data, size_t n);
} borg_module;

typedef const borg_module* (*borg_module_entry_fn)(uint32_t host_abi);

#ifdef __cplusplus
extern "C" {
#endif

const borg_module* borg_module_entry(uint32_t host_abi);

#ifdef __cplusplus
}
#endif

#endif
