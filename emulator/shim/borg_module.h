/* The contract a .borgmodule is built against.
 *
 * A module is an ordinary ARM shared object loaded by the resident object in
 * drumlogued. It runs in the daemon's own process: no process and no thread is
 * created. A module is never unloaded while the daemon runs, because the code
 * could be called from the audio path.
 *
 * ABI 1 offers one menu page. Later ABIs append callbacks to borg_module; the
 * host reads the abi field first and never reads past the layout that ABI
 * names. Fields are only appended, never reordered or removed.
 *
 * Build:
 *   ${CROSS_COMPILE}gcc -shared -fPIC -O2 -o hello.borgmodule hello.c
 *
 * Callbacks run on the daemon's own threads. page_draw runs on the panel
 * thread with the menu open; draw rows only, never clear the screen. init runs
 * once, off the audio path. Nothing here may create a process or a thread. */

#ifndef BORG_MODULE_H
#define BORG_MODULE_H

#include <stddef.h>
#include <stdint.h>

#define BORG_MODULE_ABI 1u
#define BORG_MODULE_ENTRY "borg_module_entry"
#define BORG_TAB_MAX 8          /* tab title: 7 characters plus NUL */

#define BORG_CAP_PAGE 0x0001u

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
} borg_host;

typedef struct borg_module {
  uint32_t abi;                  /* BORG_MODULE_ABI */
  const char* name;              /* tab title, at most BORG_TAB_MAX-1 chars */
  const char* version;
  uint32_t capabilities;         /* BORG_CAP_* */
  int (*init)(const borg_host* host, const char* module_path);
  void (*page_draw)(void);       /* called while the module's page is active */
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
