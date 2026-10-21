/* The contract a .borgmodule is built against.
 *
 * A module is an ordinary ARM shared object loaded by the resident object in
 * drumlogued. It runs in the daemon's own process: no process and no thread is
 * created. A module is never unloaded while the daemon runs, because the code
 * could be called from the audio path.
 *
 * ABI 1 offers one menu page. ABI 2 appends MIDI: every input the daemon reads
 * is offered to a module that asks for it, and a reply goes out on the output
 * paired with the input the message arrived on. Later ABIs append more fields;
 * the host reads the abi field first and never reads past the layout that ABI
 * names. Fields are only appended, never reordered or removed.
 *
 * Build:
 *   ${CROSS_COMPILE}gcc -shared -fPIC -O2 -o hello.borgmodule hello.c
 *   ${CROSS_COMPILE}g++ -shared -fPIC -O2 -std=c++14 -o exec.borgmodule exec.cc
 *
 * Callbacks run on the daemon's own threads. page_draw runs on the panel
 * thread with the menu open; draw rows only, never clear the screen. init runs
 * once, off the audio path. midi_in runs on the MIDI input thread that read the
 * message; tick runs on the panel thread, and also from a MIDI read when one is
 * happening. The host serializes all three between modules, so a module does
 * not need a lock of its own. Nothing here may create a process or a thread.
 *
 * A complete SysEx message is handed to midi_in, F0 through F7 inclusive.
 * Realtime bytes inside it are dropped by the host. A message longer than the
 * host's buffer is cut short and still ends in F7, so a reader can refuse it
 * for its length rather than wait for an end that is not coming. */

#ifndef BORG_MODULE_H
#define BORG_MODULE_H

#include <stddef.h>
#include <stdint.h>

#define BORG_MODULE_ABI 2u
#define BORG_MODULE_ENTRY "borg_module_entry"
#define BORG_TAB_MAX 8          /* tab title: 7 characters plus NUL */

#define BORG_CAP_PAGE 0x0001u
#define BORG_CAP_MIDI 0x0002u   /* midi_in, and host->midi_write */

#define BORG_PATH_PROGRAMS 0
#define BORG_PATH_TMP 1

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
