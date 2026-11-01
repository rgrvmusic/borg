/**
 * @file mk2_ext.c
 * @brief The extension runtime the microKORG2 preload has never carried.
 *
 * `libborg/borg_ext.c` is the drumlogue's extension runtime: the module loader,
 * the MIDI dispatch, AND a panel menu with the drumlogue's key indices, unit
 * pages, and a mixer that adds a stereo pair to the daemon's playback stream.
 * It is linked only by build-drum.sh, so on the microKORG2 nothing ever opened
 * a `.borgmodule`: the preload there is mk2_shim.c alone, which marks the
 * attach and taps the panel and does nothing else. Copying exec.borgmodule into
 * Programs therefore loaded nothing, and `tools/borgexec.py ping` heard silence
 * from a port it had found correctly.
 *
 * Linking borg_ext.c here as-is is not an option, and not because of the
 * effort. Three of its parts are wrong on this instrument in ways that would be
 * live:
 *
 *   * Its panel hook keys off the path `ttymxc2`, which is the microKORG2's
 *     panel as well. Its key table is the drumlogue's: 0x0f is KEY_MASTER
 *     there and FUNCTION 3 here, so the menu chord would open and draw
 *     drumlogue pages on a panel whose protocol this object cannot speak.
 *   * Its PCM hook mixes a module's stereo pair into the playback stream in
 *     the drumlogue codec's shape. Nothing on the microKORG2 was measured for
 *     that, and a wrong conversion there is heard, not logged.
 *   * Its paths are `/var/lib/drumlogued/...`, and its module log is written
 *     into userfs. This instrument's userfs is flash the user has asked not to
 *     spend.
 *
 * What IS portable is the part that matters here: the module contract in
 * borg_module.h, the loader, and the rawmidi interposition. They move bytes and
 * call back; nothing in them knows what instrument they are inside. That is
 * what this file carries, and nothing else:
 *
 *   * `dlopen` of every `*.borgmodule` directly in the microKORG2's Programs
 *     directory, published as an immutable set, never unloaded.
 *   * `snd_rawmidi_open`/`_close`/`_read`/`_write` interposed so a module is
 *     offered every complete SysEx message and can answer on the output paired
 *     with the input it arrived on. microkorgd imports all four.
 *   * A `borg_host` with this platform's paths, a log under /tmp (tmpfs), and
 *     `text`/`dark`/`row` as no-ops: a module page needs the panel protocol,
 *     which is not known here, and drawing with the drumlogue's would be the
 *     same mistake as the menu above.
 *
 * The first scan runs on the first MIDI read, never in a constructor -- that
 * window kills this daemon, which is why mk2_shim.c resolves nothing there
 * either. `dlopen` takes the loader lock, so it is never on the audio path.
 * Every failure is logged and skipped: a module that will not load must not
 * take the instrument with it.
 *
 * This object does not read or write the panel. MK2 keys stay with mk2_shim.c,
 * and the panel paint stays with the unit.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "borg_module.h"

/* The daemon's own tree. The emulator capture records microkorgd mapping
 * /var/lib/microkorgd, and the config dump lists it; the drumlogue's
 * /var/lib/drumlogued is a different instrument's path and is deliberately not
 * a fallback -- guessing it would load a copy of a module that is not the one
 * the user put on this instrument. */
#ifndef LOGUE_BORG_MODULE_DIR
#define LOGUE_BORG_MODULE_DIR "/var/lib/microkorgd/userfs/Programs"
#endif
/* /tmp is tmpfs. The drumlogue writes its module log into userfs; here that is
 * flash the user has explicitly asked us not to spend. */
#ifndef LOGUE_BORG_MODULE_LOG
#define LOGUE_BORG_MODULE_LOG "/tmp/logue-borg-modules.log"
#endif

#define MODULE_MAX 8
#define MODULE_FILE_MAX 96
#define MODULE_PATH_MAX 512
#define MIDI_PORT_MAX 8
#define MIDI_SYSEX_MAX 512
/* How often a running daemon re-reads the directory, so a module copied while
 * it runs shows up without another switch. */
#define RESCAN_MS 10000

/* ---- logging ------------------------------------------------------------- */

/* Raw syscalls rather than stdio: this runs on the daemon's MIDI thread before
 * anything about it is known, and _IO_* writes take locks the daemon may hold.
 * A failed log is silent -- a diagnostic that refuses to let the daemon run is
 * worse than no diagnostic. */
static void raw_log(const char* path, const char* s) {
  const long fd = syscall(SYS_openat, AT_FDCWD, path,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  size_t n = strlen(s);

  if (fd < 0) return;
  while (n > 0) {
    const long wrote = syscall(SYS_write, fd, s, n);
    if (wrote <= 0) {
      if (wrote < 0 && errno == EINTR) continue;
      break;
    }
    s += (size_t)wrote;
    n -= (size_t)wrote;
  }
  syscall(SYS_close, fd);
}

static void module_logf(const char* fmt, ...) {
  char msg[256];
  char line[320];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  snprintf(line, sizeof line, "borg ext: %s\n", msg);
  raw_log(LOGUE_BORG_MODULE_LOG, line);
}

static uint64_t now_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000U + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ---- the modules --------------------------------------------------------- */

typedef struct module_set {
  int n_modules;
  const borg_module* mod[MODULE_MAX];
  int n_tried;
  char tried[MODULE_MAX][MODULE_FILE_MAX];
} module_set;

static module_set* module_set_live;
static int module_load_state;      /* 0 idle, 1 loading, 2 loaded */
static int module_rescan_busy;
static uint64_t module_scanned_at;
/* The host serializes every module callback between modules, as the contract
 * in borg_module.h promises; the daemon reads several MIDI ports on several
 * threads. */
static pthread_mutex_t module_call_lock = PTHREAD_MUTEX_INITIALIZER;

static int host_midi_write(void* port, const void* data, size_t n);
static const char* host_port_name(void* port);

static void host_log(const char* line) {
  if (line != NULL) module_logf("%s", line);
}

/* Nothing here can draw. `text`/`dark`/`row` are the drumlogue panel's, and a
 * module that draws through them would put the drumlogue's box frames on this
 * instrument's panel -- the exact confusion this file exists to avoid. The
 * functions are present because the host table is append-only and a module
 * checks them for NULL before using them. */
static void host_nodraw(unsigned x, unsigned y, unsigned w, unsigned h,
                        const char* s) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)s;
}

static void host_norow(unsigned y, const char* label, const char* value) {
  (void)y;
  (void)label;
  (void)value;
}

static const char* host_path(int which) {
  if (which == BORG_PATH_TMP) return "/tmp";
  if (which == BORG_PATH_UNITS) return "/var/lib/microkorgd/userfs/Units";
  return "/var/lib/microkorgd/userfs/Programs";
}

/* samplerate stays 0: this object does not hook the playback stream, so it has
 * no rate to report and must not invent one. A module that initialises a unit
 * treats 0 as 48000 by contract. */
static borg_host host_services = {
  (uint32_t)sizeof(borg_host), BORG_MODULE_ABI,
  host_log, host_path, host_nodraw, host_nodraw, host_norow,
  host_midi_write, host_port_name, 0
};

static int module_suffix(const char* name) {
  static const char ext[] = ".borgmodule";
  const size_t n = strlen(name);
  const size_t e = sizeof ext - 1;

  return n > e && strcmp(name + n - e, ext) == 0;
}

static int module_name_sort(const void* a, const void* b) {
  return strcmp((const char*)a, (const char*)b);
}

/* A name that was attempted is never attempted again in this run: a file that
 * fails to load must not fill the log on every rescan. */
static int module_tried(const module_set* set, const char* name) {
  int i;

  for (i = 0; i < set->n_tried; i++) {
    if (strcmp(set->tried[i], name) == 0) return 1;
  }
  return 0;
}

static void module_load_file(module_set* set, const char* dir,
                             const char* name) {
  char path[MODULE_PATH_MAX];
  void* handle;
  borg_module_entry_fn entry;
  const borg_module* m;
  int slot;

  if (set->n_modules >= MODULE_MAX || module_tried(set, name)) return;
  if (set->n_tried < MODULE_MAX) {
    snprintf(set->tried[set->n_tried], MODULE_FILE_MAX, "%s", name);
    set->n_tried++;
  }

  snprintf(path, sizeof path, "%s/%s", dir, name);
  handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    module_logf("%s: %s", path, dlerror());
    return;
  }
  dlerror();
  entry = (borg_module_entry_fn)dlsym(handle, BORG_MODULE_ENTRY);
  if (entry == NULL) {
    module_logf("%s: no %s", path, BORG_MODULE_ENTRY);
    return;
  }
  m = entry(BORG_MODULE_ABI);
  /* An older module may refuse the current number before returning its table.
   * It is offered the number it knows, as borg_ext.c does. */
  if (m == NULL) m = entry(1);
  if (m == NULL || m->abi < 1 || m->abi > BORG_MODULE_ABI) {
    module_logf("%s: ABI %u not 1..%u", path,
                (unsigned)(m != NULL ? m->abi : 0u), BORG_MODULE_ABI);
    return;
  }
  if (m->init != NULL && m->init(&host_services, path) != 0) {
    module_logf("%s: init refused", path);
    return;
  }
  slot = set->n_modules;
  set->mod[slot] = m;
  set->n_modules = slot + 1;
  module_logf("loaded %s %s (abi %u, caps 0x%x)%s",
              m->name != NULL ? m->name : "?",
              m->version != NULL ? m->version : "", (unsigned)m->abi,
              (unsigned)m->capabilities,
              (m->capabilities & ~(BORG_CAP_MIDI | BORG_CAP_PAGE)) != 0
                  ? " [caps beyond MIDI/PAGE not offered here]"
                  : "");
}

static void module_scan_dir(module_set* set, const char* dir) {
  int i, n_names = 0;
  char names[MODULE_MAX][MODULE_FILE_MAX];
  DIR* d;
  struct dirent* e;

  d = opendir(dir);
  if (d == NULL) {
    module_logf("no module directory %s: %s", dir, strerror(errno));
    return;
  }
  while ((e = readdir(d)) != NULL && n_names < MODULE_MAX) {
    if (!module_suffix(e->d_name)) continue;
    snprintf(names[n_names], MODULE_FILE_MAX, "%s", e->d_name);
    n_names++;
  }
  closedir(d);

  qsort(names, (size_t)n_names, MODULE_FILE_MAX, module_name_sort);
  for (i = 0; i < n_names; i++) module_load_file(set, dir, names[i]);
}

static const char* module_dir(void) {
  const char* override = getenv("LOGUE_BORG_MODULE_DIR");

  if (override != NULL && *override != '\0') return override;
  return LOGUE_BORG_MODULE_DIR;
}

static void modules_load(void) {
  int expected = 0;
  module_set* set;

  if (!__atomic_compare_exchange_n(&module_load_state, &expected, 1, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }

  set = (module_set*)calloc(1, sizeof *set);
  if (set == NULL) {
    raw_log(LOGUE_BORG_MODULE_LOG, "borg ext: out of memory\n");
    __atomic_store_n(&module_load_state, 2, __ATOMIC_RELEASE);
    return;
  }

  module_logf("scanning %s", module_dir());
  module_scan_dir(set, module_dir());
  module_logf("%d module(s)", set->n_modules);
  module_scanned_at = now_ms();
  __atomic_store_n(&module_set_live, set, __ATOMIC_RELEASE);
  __atomic_store_n(&module_load_state, 2, __ATOMIC_RELEASE);
}

/* A module copied into Programs while the daemon runs is picked up here, on
 * the MIDI thread, at most once per RESCAN_MS. The loaded set is never freed
 * and never unloaded: a module may still be reachable from a callback. */
static void modules_rescan(void) {
  const module_set* live;
  module_set* set;
  int before_modules, before_tried, expected = 0;

  if (__atomic_load_n(&module_load_state, __ATOMIC_ACQUIRE) != 2) return;
  if (now_ms() - module_scanned_at < RESCAN_MS) return;
  if (!__atomic_compare_exchange_n(&module_rescan_busy, &expected, 1, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }

  module_scanned_at = now_ms();
  live = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  set = (module_set*)calloc(1, sizeof *set);
  if (set == NULL) {
    __atomic_store_n(&module_rescan_busy, 0, __ATOMIC_RELEASE);
    return;
  }
  if (live != NULL) *set = *live;

  before_modules = set->n_modules;
  before_tried = set->n_tried;
  module_scan_dir(set, module_dir());
  if (set->n_modules != before_modules || set->n_tried != before_tried) {
    __atomic_store_n(&module_set_live, set, __ATOMIC_RELEASE);
  } else {
    free(set);
  }
  __atomic_store_n(&module_rescan_busy, 0, __ATOMIC_RELEASE);
}

static void modules_midi_in(void* port, const unsigned char* data, size_t n) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if ((m->capabilities & BORG_CAP_MIDI) != 0 && m->midi_in != NULL) {
      m->midi_in(port, data, n);
    }
  }
  pthread_mutex_unlock(&module_call_lock);
}

static void modules_tick(void) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  modules_rescan();
  set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  if (set == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->tick != NULL) m->tick();
  }
  pthread_mutex_unlock(&module_call_lock);
}

/* ---- the MIDI ------------------------------------------------------------ */

typedef struct _snd_rawmidi snd_rawmidi_t;

ssize_t snd_rawmidi_write(snd_rawmidi_t* rm, const void* buf, size_t n);

/* The daemon opens each port with one call that hands back an input and an
 * output handle. A module is given the input handle, and a reply is written to
 * the output returned beside it, so a command that arrives on the USB port is
 * answered there. A closed port keeps its row, which the next open reuses, so a
 * handle stays valid for the life of the module even if the daemon reopens the
 * device. */
typedef struct midi_port {
  snd_rawmidi_t* input;
  snd_rawmidi_t* output;
  char name[128];
} midi_port;

static midi_port midi_ports[MIDI_PORT_MAX];
static int n_midi_ports;
static pthread_mutex_t midi_ports_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t midi_account_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char midi_sysex_buf[MIDI_SYSEX_MAX];
static size_t midi_sysex_len;
static void* midi_sysex_port;
static int midi_sysex_active;

static void midi_port_register(snd_rawmidi_t* in, snd_rawmidi_t* out,
                               const char* name) {
  int i;
  int free_row = -1;

  pthread_mutex_lock(&midi_ports_lock);
  for (i = 0; i < n_midi_ports; i++) {
    if (midi_ports[i].input == in) {
      midi_ports[i].output = out;
      snprintf(midi_ports[i].name, sizeof midi_ports[i].name, "%s",
               name != NULL ? name : "");
      pthread_mutex_unlock(&midi_ports_lock);
      return;
    }
    if (free_row < 0 && midi_ports[i].input == NULL &&
        midi_ports[i].output == NULL) {
      free_row = i;
    }
  }
  if (free_row < 0 && n_midi_ports < MIDI_PORT_MAX) free_row = n_midi_ports++;
  if (free_row >= 0) {
    midi_ports[free_row].input = in;
    midi_ports[free_row].output = out;
    snprintf(midi_ports[free_row].name, sizeof midi_ports[free_row].name,
             "%s", name != NULL ? name : "");
  }
  pthread_mutex_unlock(&midi_ports_lock);
}

int snd_rawmidi_open(snd_rawmidi_t** in_rmidi, snd_rawmidi_t** out_rmidi,
                     const char* name, int mode) {
  static int (*real)(snd_rawmidi_t**, snd_rawmidi_t**, const char*, int);

  if (real == NULL) {
    real = (int (*)(snd_rawmidi_t**, snd_rawmidi_t**, const char*, int))
        dlsym(RTLD_NEXT, "snd_rawmidi_open");
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -ENOSYS;
  }
  {
    const int rc = real(in_rmidi, out_rmidi, name, mode);
    if (rc >= 0 && in_rmidi != NULL && *in_rmidi != NULL) {
      midi_port_register(*in_rmidi, (out_rmidi != NULL) ? *out_rmidi : NULL,
                         name);
    }
    return rc;
  }
}

int snd_rawmidi_close(snd_rawmidi_t* rm) {
  static int (*real)(snd_rawmidi_t*);
  int i;

  if (real == NULL) {
    real = (int (*)(snd_rawmidi_t*)) dlsym(RTLD_NEXT, "snd_rawmidi_close");
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -ENOSYS;
  }
  pthread_mutex_lock(&midi_ports_lock);
  for (i = 0; i < n_midi_ports; i++) {
    if (midi_ports[i].input == rm) midi_ports[i].input = NULL;
    if (midi_ports[i].output == rm) midi_ports[i].output = NULL;
  }
  pthread_mutex_unlock(&midi_ports_lock);
  return real(rm);
}

static int host_midi_write(void* port, const void* data, size_t n) {
  snd_rawmidi_t* out = NULL;
  size_t sent = 0;
  int waits = 0;
  int i;

  if (port == NULL || (data == NULL && n > 0)) return -1;

  pthread_mutex_lock(&midi_ports_lock);
  for (i = 0; i < n_midi_ports; i++) {
    if (midi_ports[i].input == (snd_rawmidi_t*)port) {
      out = midi_ports[i].output;
      break;
    }
  }
  pthread_mutex_unlock(&midi_ports_lock);
  if (out == NULL) return -1;

  /* The daemon opens its ports non-blocking, so a full output buffer answers
   * EAGAIN. A reply is small and the buffer drains at the port's own rate; the
   * bound keeps a stuck port from parking the daemon's MIDI thread. */
  while (sent < n) {
    const ssize_t put = snd_rawmidi_write(out, (const char*)data + sent,
                                          n - sent);
    if (put > 0) {
      sent += (size_t)put;
      waits = 0;
      continue;
    }
    if (put < 0 && errno == EAGAIN && waits < 500) {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 1000000L;
      nanosleep(&ts, NULL);
      waits++;
      continue;
    }
    return -1;
  }
  return 0;
}

static const char* host_port_name(void* port) {
  static const char unknown[] = "";
  const char* name = unknown;
  int i;

  pthread_mutex_lock(&midi_ports_lock);
  for (i = 0; i < n_midi_ports; i++) {
    if (midi_ports[i].input == (snd_rawmidi_t*)port) {
      name = midi_ports[i].name;
      break;
    }
  }
  pthread_mutex_unlock(&midi_ports_lock);
  return name;
}

/* One SysEx message is assembled and handed to the modules whole, terminator
 * always on the end: a message that overflowed is refused for its length rather
 * than waited on for an end that is not coming. A byte on another port abandons
 * the half-received message rather than mixing two ports into one. */
static void midi_sysex_end(void) {
  if (midi_sysex_len < MIDI_SYSEX_MAX) {
    midi_sysex_buf[midi_sysex_len++] = 0xf7;
  } else {
    midi_sysex_buf[MIDI_SYSEX_MAX - 1] = 0xf7;
    midi_sysex_len = MIDI_SYSEX_MAX;
  }
  modules_midi_in(midi_sysex_port, midi_sysex_buf, midi_sysex_len);
  midi_sysex_len = 0;
  midi_sysex_port = NULL;
}

static void midi_account(unsigned char* buf, size_t n, void* port) {
  size_t i;

  for (i = 0; i < n; i++) {
    const unsigned char b = buf[i];

    if (b >= 0xf8) continue;               /* realtime, no state */
    if (midi_sysex_active) {
      if (midi_sysex_port != port) {
        midi_sysex_active = 0;
        midi_sysex_len = 0;
        midi_sysex_port = NULL;
      } else if (b == 0xf7) {
        midi_sysex_active = 0;
        midi_sysex_end();
        continue;
      } else if (b < 0x80) {
        if (midi_sysex_len < MIDI_SYSEX_MAX) midi_sysex_buf[midi_sysex_len++] = b;
        continue;
      } else {
        /* Another status byte ends the message without a terminator; it is
         * handled below as the start of what follows. */
        midi_sysex_active = 0;
        midi_sysex_len = 0;
        midi_sysex_port = NULL;
      }
    }
    if (b == 0xf0) {
      midi_sysex_active = 1;
      midi_sysex_len = 1;
      midi_sysex_buf[0] = 0xf0;
      midi_sysex_port = port;
      continue;
    }
    /* Channel messages and the rest of the system range are not offered: this
     * object implements ABI 2's midi_in only, and a module that asked for a
     * transform would need its bytes rewritten in the daemon's own buffer,
     * which is a change to what the instrument plays. The load log says so. */
    if (b >= 0x80) {
      midi_sysex_active = 0;
      midi_sysex_len = 0;
      midi_sysex_port = NULL;
    }
  }
}

ssize_t snd_rawmidi_read(snd_rawmidi_t* rm, void* buf, size_t n) {
  static ssize_t (*real)(snd_rawmidi_t*, void*, size_t);

  if (real == NULL) {
    real = (ssize_t (*)(snd_rawmidi_t*, void*, size_t))
        dlsym(RTLD_NEXT, "snd_rawmidi_read");
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const ssize_t got = real(rm, buf, n);
    /* The first read is the trigger for the scan: the daemon is up, its MIDI
     * thread exists, and dlopen's loader lock is safe to take here. Never a
     * constructor -- that window kills this daemon. */
    if (__atomic_load_n(&module_load_state, __ATOMIC_ACQUIRE) == 0) {
      modules_load();
    }
    if (got > 0) {
      pthread_mutex_lock(&midi_account_lock);
      midi_account((unsigned char*)buf, (size_t)got, rm);
      pthread_mutex_unlock(&midi_account_lock);
    }
    if (got >= 0) modules_tick();
    return got;
  }
}

ssize_t snd_rawmidi_write(snd_rawmidi_t* rm, const void* buf, size_t n) {
  static ssize_t (*real)(snd_rawmidi_t*, const void*, size_t);

  if (real == NULL) {
    real = (ssize_t (*)(snd_rawmidi_t*, const void*, size_t))
        dlsym(RTLD_NEXT, "snd_rawmidi_write");
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real(rm, buf, n);
}
