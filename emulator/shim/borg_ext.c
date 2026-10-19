/* The extensions the object carries once it is inside drumlogued.
 *
 * status   MUTE and MASTER together open a two-page readout on the panel.
 *          Keys pressed while it is open are consumed, so the daemon sees
 *          neither the chord nor the keys, and cannot paint over the page.
 *          PAGE up/down moves between pages; the same chord closes it, and
 *          the daemon's last screen is replayed.
 * midi     every byte through snd_rawmidi_read/write is counted. Note
 *          messages that arrive whole in one read can be transposed or
 *          velocity-curved (LOGUE_BORG_MIDI_TRANSPOSE,
 *          LOGUE_BORG_MIDI_VELOCITY, both off by default).
 *
 * No symbol is resolved from a constructor: that window kills this daemon.
 * Every interposer resolves its real function on the first call it serves.
 * No process and no thread is created; the hooks run on the daemon's own. */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "borg_module.h"
#define BORG_UNIT_BUILD 1
#include "borg_unit.h"

#ifndef LOGUE_BORG_MIDI_LOG
#define LOGUE_BORG_MIDI_LOG "/tmp/logue-borg-midi.log"
#endif

/* Modules live directly in Programs, next to the unit and log directories.
 * A subdirectory is not scanned: the instrument's userfs is the only place a
 * module is expected. */
#ifndef LOGUE_BORG_MODULE_DIR
#define LOGUE_BORG_MODULE_DIR "/var/lib/drumlogued/userfs/Programs"
#endif

#ifndef LOGUE_BORG_MODULE_LOG
#define LOGUE_BORG_MODULE_LOG "/var/lib/drumlogued/userfs/Programs/borg-modules.log"
#endif

#define PANEL_FDS_MAX 8
#define CAPTURE_MAX 8192
#define MODULE_MAX 8
#define MODULE_FILE_MAX 96
#define MODULE_DIR_MAX 256
#define MODULE_PATH_MAX 512
/* STATUS and MIDI come first; unit pages follow, then module pages. */
#define UNIT_PAGE_BASE 2
#define UNIT_MAX 4

/* The daemon's own key indices, measured from the panel. */
#define KEY_MASTER 0x0f
#define KEY_PAGE_DOWN 0x21
#define KEY_PAGE_UP 0x20
#define KEY_MUTE 0x22

static int panel_fds[PANEL_FDS_MAX];
static int n_panel_fds;
static int panel_out_fd = -1;

static int menu_open;
static int menu_page;
/* The daemon's panel mode, from its id 4 command: 1 for the sequencer views
 * (LIVE, STEP, ACCENT), 0 for an ordinary page. The sequencer views are kept
 * by the panel itself, so the menu never draws over them: it switches to page
 * mode, draws there, and switches back with the id 4 command. Painting over
 * that view and replaying a capture garbles it, because its state is retained
 * and not fully expressed in the screen frames. */
static int view_mode;
static int mute_held;
static int master_held;
static int chord_latch;

/* The first of the two chord keys is held for one read. If the other follows,
 * neither reaches the daemon; if anything else follows, the held key is
 * delivered first, in order, so a single MUTE or MASTER press still works. */
static unsigned char held_key;
static int held_valid;

/* The daemon's screen, as class-0x85 frames, replayed on close. A page-entry
 * fill is the one frame held back until something is drawn into it. */
static unsigned char capture[CAPTURE_MAX];
static size_t capture_len;
static int capture_valid;
static unsigned char pending_fill[12];
static int pending_valid;
static int pending_whole;

static unsigned long midi_in_bytes, midi_out_bytes;
static unsigned long midi_in_msgs, midi_out_msgs, midi_notes;
static unsigned long midi_last_status, midi_last_note, midi_last_vel;
static int midi_transpose;
static int midi_velocity = 100;

static unsigned long audio_calls, audio_errors;
static unsigned long menu_opens;

static time_t start_epoch;

static long raw_write(int fd, const void* p, size_t n) {
  return syscall(SYS_write, fd, p, n);
}

static void raw_log(const char* path, const char* line) {
  const int fd = (int)syscall(SYS_openat, AT_FDCWD, path,
                              O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return;
  raw_write(fd, line, strlen(line));
  syscall(SYS_close, fd);
}

__attribute__((constructor)) static void borg_ext_init(void) {
  const char* t = getenv("LOGUE_BORG_MIDI_TRANSPOSE");
  const char* v = getenv("LOGUE_BORG_MIDI_VELOCITY");
  if (t != NULL) midi_transpose = atoi(t);
  if (v != NULL) {
    midi_velocity = atoi(v);
    if (midi_velocity < 1) midi_velocity = 1;
    if (midi_velocity > 200) midi_velocity = 200;
  }
  start_epoch = time(NULL);
}

/* ---------------------------------------------------------------- the panel */

static int panel_is(int fd) {
  int i;
  for (i = 0; i < n_panel_fds; i++) {
    if (panel_fds[i] == fd) return 1;
  }
  return 0;
}

static void panel_reset(void) {
  menu_open = 0;
  menu_page = 0;
  view_mode = 0;
  mute_held = 0;
  master_held = 0;
  chord_latch = 0;
  held_key = 0;
  held_valid = 0;
  capture_len = 0;
  capture_valid = 1;
  pending_valid = 0;
  pending_whole = 0;
}

static void panel_add(int fd) {
  if (fd < 0 || panel_is(fd)) return;
  if (n_panel_fds == 0) panel_reset();
  if (n_panel_fds < PANEL_FDS_MAX) {
    panel_fds[n_panel_fds++] = fd;
  } else {
    panel_fds[0] = fd;
  }
}

static void panel_del(int fd) {
  int i;
  for (i = 0; i < n_panel_fds; i++) {
    if (panel_fds[i] == fd) {
      int j;
      for (j = i + 1; j < n_panel_fds; j++) panel_fds[j - 1] = panel_fds[j];
      n_panel_fds--;
      break;
    }
  }
  if (n_panel_fds == 0) panel_reset();
}

/* One class-0x85 id 0 frame: box header, then the text (at most 51 chars).
 *
 * The two colour bytes are the ink and the field behind it, in that order.
 * The daemon writes `0f 00` for titles, values and part names (light ink on the
 * dark page) and `00 0f` for parameter names (dark ink on a light field). Menu
 * rows use the title pair, so no light field appears behind them; a cell with
 * a light field uses the label pair. The menu draws in page mode, so the kind
 * is 0. */
static size_t screen_frame(unsigned char* out, unsigned char role,
                           unsigned char ink, unsigned char field,
                           unsigned x, unsigned y, unsigned w, unsigned h,
                           const char* text) {
  size_t n = strlen(text);
  if (n > 51) n = 51;
  out[0] = 0x85;
  out[1] = (unsigned char)(13 + n);
  out[2] = 0x00;
  out[3] = role;
  out[4] = ink;
  out[5] = field;
  out[6] = (unsigned char)(x & 0x7f);
  out[7] = (unsigned char)((x >> 7) & 0x7f);
  out[8] = (unsigned char)y;
  out[9] = (unsigned char)(w & 0x7f);
  out[10] = (unsigned char)((w >> 7) & 0x7f);
  out[11] = (unsigned char)h;
  out[12] = 0x00;
  memcpy(out + 13, text, n);
  return 13 + n;
}

/* One class-0x85 id 1 frame: a region fill. Role 0 is the page background;
 * role 0x0f paints a light band for dark-ink text. The daemon paints the band
 * first and draws the text into it; the text frame's field byte alone does not
 * paint it on the instrument. The menu draws in page mode, so the kind is 0 as
 * on the daemon's pages. */
static size_t fill_frame(unsigned char* out, unsigned char role,
                         unsigned x, unsigned y, unsigned w, unsigned h) {
  out[0] = 0x85;
  out[1] = 0x0c;
  out[2] = 0x01;
  out[3] = role;
  out[4] = 0x00;                              /* style */
  out[5] = (unsigned char)(x & 0x7f);
  out[6] = (unsigned char)((x >> 7) & 0x7f);
  out[7] = (unsigned char)y;
  out[8] = (unsigned char)(w & 0x7f);
  out[9] = (unsigned char)((w >> 7) & 0x7f);
  out[10] = (unsigned char)h;
  out[11] = 0x00;
  return 12;
}

/* Role 0 is the daemon's label and title style, the small font. Role 2 is the
 * large value style, which does not fit six rows on a 64-unit screen. */
static void put_styled(unsigned x, unsigned y, unsigned w, unsigned h,
                       unsigned char ink, unsigned char field,
                       const char* text) {
  unsigned char frame[64];
  const size_t n = screen_frame(frame, 0x20, ink, field, x, y, w, h, text);
  if (panel_out_fd >= 0) raw_write(panel_out_fd, frame, n);
}

static void put_text(unsigned x, unsigned y, unsigned w, unsigned h,
                     const char* text) {
  put_styled(x, y, w, h, 0x0f, 0x00, text);
}

/* Dark ink for a cell that carries its own light field. The daemon's inverse
 * label cell is a 12-high role 0x0f band with 8-high text inset by 2: BANK and
 * TARGET on the PROGRAM SELECT page are sent exactly this way. Matching that
 * shape matters; the panel does not invert an 8-high band with the text flush
 * against its edges. */
static void put_dark(unsigned x, unsigned y, unsigned w, unsigned h,
                     const char* text) {
  unsigned char fill[12];

  if (panel_out_fd < 0) return;
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x0f, x, y, w, 12));
  put_styled(x, y + 2, w, h, 0x00, 0x0f, text);
}

/* A field name on the left and its value right-aligned on the right. Both are
 * light on the dark page: a light band behind the name is hard to read at this
 * size, so the column and the gap do the separating. */
static void menu_row(unsigned y, const char* label, const char* value) {
  char cell[16];

  snprintf(cell, sizeof cell, "%-12s", label);
  put_text(0, y, 120, 8, cell);
  snprintf(cell, sizeof cell, "%12s", value);
  put_text(128, y, 128, 8, cell);
}

/* --------------------------------------------------------------- the modules
 *
 * A .borgmodule is an ordinary ARM shared object with its own suffix, kept
 * directly in the userfs Programs directory. The first scan runs on the panel
 * thread at the first panel read or write: never in a constructor, because that
 * window kills this daemon, and never on the audio path, because dlopen takes
 * the loader lock. The directory is scanned again whenever the menu opens, so a
 * module copied while the daemon runs shows up without another switch. The
 * loaded set is published in one atomic store and is never freed or unloaded.
 * A module that fails to load is logged and skipped; it never takes the daemon
 * with it, and it is not retried on every menu open. */

typedef struct module_set {
  int n_modules;
  int n_pages;
  const borg_module* mod[MODULE_MAX];
  char name[MODULE_MAX][MODULE_FILE_MAX];
  int n_tried;
  char tried[MODULE_MAX][MODULE_FILE_MAX];
} module_set;

static module_set* module_set_live;
static int module_load_state;           /* 0 idle, 1 loading, 2 loaded */
static int module_rescan_busy;

static void module_logf(const char* fmt, ...) {
  char msg[256];
  char line[320];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  snprintf(line, sizeof line, "borg module: %s\n", msg);
  raw_log(LOGUE_BORG_MODULE_LOG, line);
}

static void host_log(const char* line) {
  if (line != NULL) module_logf("%s", line);
}

static const char* host_path(int which) {
  if (which == BORG_PATH_TMP) return "/tmp";
  return "/var/lib/drumlogued/userfs/Programs";
}

static void host_text(unsigned x, unsigned y, unsigned w, unsigned h,
                      const char* s) {
  if (panel_out_fd >= 0 && s != NULL) put_text(x, y, w, h, s);
}

static void host_dark(unsigned x, unsigned y, unsigned w, unsigned h,
                      const char* s) {
  if (panel_out_fd >= 0 && s != NULL) put_dark(x, y, w, h, s);
}

static void host_row(unsigned y, const char* label, const char* value) {
  if (panel_out_fd >= 0) menu_row(y, label != NULL ? label : "",
                                  value != NULL ? value : "");
}

static const borg_host host_services = {
  (uint32_t)sizeof(borg_host), BORG_MODULE_ABI,
  host_log, host_path, host_text, host_dark, host_row
};

/* ------------------------------------------------------------ the unit pages
 *
 * A unit is loaded into this same process, so it can see this object's
 * symbols. The declarations in borg_unit.h are weak: a unit built against
 * them runs unchanged when borg is not active, because the symbol resolves to
 * zero. A unit that does find us registers a page and a draw callback.
 *
 * Registration happens from unit_init or unit_resume, off the audio thread.
 * The set is published as an immutable array and the old one is left alone:
 * the panel thread may still be walking it, and a registration is rare enough
 * that a few dozen bytes are cheaper than a lock on the panel path. */

typedef struct unit_set {
  int n;
  const borg_unit_desc* desc[UNIT_MAX];
} unit_set;

static unit_set* unit_set_live;

static unit_set* unit_set_copy(void) {
  const unit_set* cur = __atomic_load_n(&unit_set_live, __ATOMIC_ACQUIRE);
  unit_set* next = (unit_set*)malloc(sizeof *next);

  if (next == NULL) return NULL;
  if (cur != NULL) {
    *next = *cur;
  } else {
    next->n = 0;
  }
  return next;
}

int borg_unit_register(const borg_unit_desc* desc) {
  unit_set* next;
  int i;

  if (desc == NULL || desc->abi != BORG_UNIT_ABI || desc->name == NULL ||
      desc->name[0] == '\0' || desc->draw == NULL) {
    return -1;
  }
  next = unit_set_copy();
  if (next == NULL) return -1;
  for (i = 0; i < next->n; i++) {
    if (next->desc[i] == desc) {
      __atomic_store_n(&unit_set_live, next, __ATOMIC_RELEASE);
      return 0;
    }
  }
  if (next->n >= UNIT_MAX) {
    free(next);
    return -1;
  }
  next->desc[next->n++] = desc;
  __atomic_store_n(&unit_set_live, next, __ATOMIC_RELEASE);
  module_logf("unit page %s", desc->name);
  return 0;
}

void borg_unit_unregister(const borg_unit_desc* desc) {
  unit_set* next;
  int i, j;
  int found = 0;

  if (desc == NULL) return;
  next = unit_set_copy();
  if (next == NULL) return;
  for (i = 0; i < next->n; i++) {
    if (next->desc[i] == desc) {
      for (j = i + 1; j < next->n; j++) next->desc[j - 1] = next->desc[j];
      next->n--;
      found = 1;
      break;
    }
  }
  if (!found) {
    free(next);
    return;
  }
  if (next->n == 0) {
    /* The empty set was never published, so it can be freed. The old set is
     * still reachable by a reader and stays allocated. */
    __atomic_store_n(&unit_set_live, NULL, __ATOMIC_RELEASE);
    free(next);
    return;
  }
  __atomic_store_n(&unit_set_live, next, __ATOMIC_RELEASE);
}

static int unit_count(void) {
  const unit_set* set = __atomic_load_n(&unit_set_live, __ATOMIC_ACQUIRE);

  return set != NULL ? set->n : 0;
}

static const borg_unit_desc* page_unit(int page) {
  const unit_set* set = __atomic_load_n(&unit_set_live, __ATOMIC_ACQUIRE);
  const int i = page - UNIT_PAGE_BASE;

  if (set == NULL || i < 0 || i >= set->n) return NULL;
  return set->desc[i];
}

static const borg_unit_panel unit_panel_services = {
  (uint32_t)sizeof(borg_unit_panel), BORG_UNIT_ABI,
  host_text, host_dark, host_row
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

static void ensure_open(void);

/* A name that was attempted is never attempted again: the file may have been
 * replaced with one that still fails, and a failed file must not fill the log
 * on every menu open. It is remembered across rescans with the loaded set. */
static int module_tried(const module_set* set, const char* name) {
  int i;

  for (i = 0; i < set->n_tried; i++) {
    if (strcmp(set->tried[i], name) == 0) return 1;
  }
  return 0;
}

static void module_note_tried(module_set* set, const char* name) {
  if (set->n_tried < MODULE_MAX) {
    snprintf(set->tried[set->n_tried], MODULE_FILE_MAX, "%s", name);
    set->n_tried++;
  }
}

static void module_load_file(module_set* set, const char* dir,
                             const char* name) {
  char path[MODULE_PATH_MAX];
  void* handle;
  borg_module_entry_fn entry;
  const borg_module* m;
  int slot;

  if (set->n_modules >= MODULE_MAX || module_tried(set, name)) return;
  module_note_tried(set, name);

  snprintf(path, sizeof path, "%.*s/%.*s", MODULE_DIR_MAX - 1, dir,
           MODULE_FILE_MAX - 1, name);
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
  if (m == NULL || m->abi != BORG_MODULE_ABI) {
    module_logf("%s: ABI %u not %u", path,
                (unsigned)(m != NULL ? m->abi : 0u), BORG_MODULE_ABI);
    return;
  }
  if (m->init != NULL && m->init(&host_services, path) != 0) {
    module_logf("%s: init refused", path);
    return;
  }
  slot = set->n_modules;
  set->mod[slot] = m;
  snprintf(set->name[slot], MODULE_FILE_MAX, "%s", name);
  set->n_modules = slot + 1;
  if ((m->capabilities & BORG_CAP_PAGE) != 0 && m->page_draw != NULL &&
      m->name != NULL) {
    set->n_pages++;
  }
  module_logf("loaded %s %s", m->name != NULL ? m->name : "?",
              m->version != NULL ? m->version : "");
}

static void module_scan_dir(module_set* set, const char* dir) {
  int i, n_names = 0;
  char names[MODULE_MAX][MODULE_FILE_MAX];
  DIR* d;
  struct dirent* e;

  d = opendir(dir);
  if (d == NULL) {
    module_logf("no module directory %s", dir);
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

  /* Resolve the real open before dlopen holds the loader lock, so the opens
   * that dlopen itself makes do not try to resolve through it. */
  ensure_open();

  set = (module_set*)calloc(1, sizeof *set);
  if (set == NULL) {
    raw_log(LOGUE_BORG_MODULE_LOG, "borg module: out of memory\n");
    __atomic_store_n(&module_load_state, 2, __ATOMIC_RELEASE);
    return;
  }

  module_scan_dir(set, module_dir());
  __atomic_store_n(&module_set_live, set, __ATOMIC_RELEASE);
  __atomic_store_n(&module_load_state, 2, __ATOMIC_RELEASE);
}

/* Called when the menu opens, on the panel thread. A module copied into
 * Programs while the daemon runs is picked up here, without another switch.
 * The loaded modules are never unloaded; a new set is published only when the
 * scan found something new, so a reader never sees a half-built one. */
static void modules_rescan(void) {
  const module_set* live;
  module_set* set;
  int before_modules, before_tried, expected = 0;

  if (__atomic_load_n(&module_load_state, __ATOMIC_ACQUIRE) != 2) return;
  if (!__atomic_compare_exchange_n(&module_rescan_busy, &expected, 1, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }

  ensure_open();
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

static int module_page_base(void) {
  return UNIT_PAGE_BASE + unit_count();
}

static int page_count(void) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);

  return module_page_base() + (set != NULL ? set->n_pages : 0);
}

static const borg_module* page_module(int page) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  const int base = module_page_base();
  int i, seen = 0;

  if (set == NULL || page < base) return NULL;
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if ((m->capabilities & BORG_CAP_PAGE) == 0 || m->page_draw == NULL ||
        m->name == NULL) {
      continue;
    }
    if (seen == page - base) return m;
    seen++;
  }
  return NULL;
}

static void menu_draw(void) {
  unsigned char fill[12];
  char value[24];
  char tab[BORG_UNIT_TAB_MAX];
  const int pages = page_count();

  if (panel_out_fd < 0) return;
  /* A unit can suspend while its page is open, which takes the page with it. */
  if (pages > 0 && menu_page >= pages) menu_page = pages - 1;

  /* The daemon's page preamble: the whole screen, then the title band, both
   * role 0. */
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 0, 256, 64));
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 0, 256, 8));
  /* Clear the row area separately. The whole-screen fill is the daemon's own
   * page-entry preamble, but the panel service holds a full fill until an
   * item commits it, and the title-band fill above replaces what it holds.
   * A page with fewer rows than the one before it would then leave the old
   * rows behind. On the instrument the whole-screen fill has already cleared
   * them; this fills the same area again so both ends agree. */
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 24, 256, 40));

  /* The title band is the daemon's own title row: light text on the dark page,
   * centred, in the same frame the daemon gives -PROGRAM SELECT-. */
  put_text(64, 2, 128, 8, "-BORG-");

  /* The tab bar sits in the label row at y=12, as 64-wide cells, exactly like
   * BANK and TARGET. That is the only inverse cell the instrument is proven to
   * draw; an identical band in the title row stays white on white. Both tab
   * texts sit at y=14, the inset the daemon gives an inverse label. The arrows
   * at the right sit where the two page keys act. */
  put_text(192, 14, 10, 8, "<");
  put_text(246, 14, 10, 8, ">");
  if (menu_page == 0) {
    put_dark(0, 12, 64, 8, "STATUS");
  } else {
    put_text(0, 14, 64, 8, "STATUS");
  }
  if (menu_page == 1) {
    put_dark(64, 12, 64, 8, "MIDI");
  } else {
    put_text(64, 14, 64, 8, "MIDI");
  }
  /* The third cell carries the first unit's tab, else the first module's; on a
   * unit or module page it carries that page's tab. It is the same 64-wide cell
   * as STATUS, MIDI, BANK and TARGET. A name is cut to the cell's 7 characters. */
  {
    const int module_base = module_page_base();
    const borg_unit_desc* tab_unit = NULL;
    const borg_module* tab_module = NULL;
    int tab_active = 0;

    if (menu_page >= UNIT_PAGE_BASE && menu_page < module_base) {
      tab_unit = page_unit(menu_page);
      tab_active = 1;
    } else if (menu_page >= module_base) {
      tab_module = page_module(menu_page);
      tab_active = 1;
    } else {
      tab_unit = page_unit(UNIT_PAGE_BASE);
      if (tab_unit == NULL) tab_module = page_module(module_base);
    }
    if (tab_unit != NULL) {
      snprintf(tab, sizeof tab, "%s", tab_unit->name);
      if (tab_active) {
        put_dark(128, 12, 64, 8, tab);
      } else {
        put_text(128, 14, 64, 8, tab);
      }
    } else if (tab_module != NULL) {
      snprintf(tab, sizeof tab, "%s", tab_module->name);
      if (tab_active) {
        put_dark(128, 12, 64, 8, tab);
      } else {
        put_text(128, 14, 64, 8, tab);
      }
    }
  }

  if (menu_page == 0) {
    const long up = (long)(time(NULL) - start_epoch);
    snprintf(value, sizeof value, "%d", (int)getpid());
    menu_row(26, "DAEMON PID:", value);
    if (up >= 3600) {
      snprintf(value, sizeof value, "%ldH %02ldM", up / 3600, (up / 60) % 60);
    } else if (up >= 60) {
      snprintf(value, sizeof value, "%ldM %02ldS", up / 60, up % 60);
    } else {
      snprintf(value, sizeof value, "%ld S", up);
    }
    menu_row(35, "UPTIME:", value);
    snprintf(value, sizeof value, "%lu", audio_calls);
    menu_row(44, "AUDIO CALLS:", value);
    snprintf(value, sizeof value, "%lu / %lu", audio_errors, menu_opens);
    menu_row(53, "XRUNS/OPENS:", value);
  } else if (menu_page == 1) {
    snprintf(value, sizeof value, "%lu / %lu B", midi_in_bytes, midi_out_bytes);
    menu_row(26, "MIDI I/O:", value);
    snprintf(value, sizeof value, "%lu / %lu", midi_in_msgs, midi_notes);
    menu_row(35, "MSGS/NOTES:", value);
    snprintf(value, sizeof value, "%02lX %02lX %02lX", midi_last_status,
             midi_last_note, midi_last_vel);
    menu_row(44, "LAST NOTE:", value);
    snprintf(value, sizeof value, "%+d / %d%%", midi_transpose, midi_velocity);
    menu_row(53, "TRANS/VEL:", value);
  } else if (menu_page < module_page_base()) {
    const borg_unit_desc* u = page_unit(menu_page);
    if (u != NULL && u->draw != NULL) u->draw(&unit_panel_services, u->user);
  } else {
    const borg_module* m = page_module(menu_page);
    if (m != NULL && m->page_draw != NULL) m->page_draw();
  }
}

/* Redraw only the unit's content area, leaving the title and tab bar alone.
 * Called from the panel thread on the panel's own half-second report, so a
 * unit's page follows playback while the menu is held open. */
static void unit_page_redraw(void) {
  unsigned char fill[12];
  const borg_unit_desc* u;

  if (panel_out_fd < 0 || !menu_open) return;
  if (menu_page < UNIT_PAGE_BASE || menu_page >= module_page_base()) return;
  u = page_unit(menu_page);
  if (u == NULL || u->draw == NULL) return;
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 24, 256, 40));
  u->draw(&unit_panel_services, u->user);
}

static void menu_show(int fd) {
  static const unsigned char page_mode[4] = {0x85, 0x04, 0x04, 0x00};
  static const unsigned char view_reset[4] = {0x85, 0x04, 0x09, 0x02};
  panel_out_fd = fd;
  menu_open = 1;
  menu_page = 0;
  menu_opens++;
  /* The daemon begins every view change with id 4 (page or sequencer mode) and
   * id 9, then the page fills. Both commands are part of the page context the
   * inverse labels are drawn in; copying only the fills left the active tab in
   * the wrong video state on the instrument. */
  if (view_mode == 1) raw_write(fd, page_mode, sizeof page_mode);
  raw_write(fd, view_reset, sizeof view_reset);
  /* A module copied since the load point is picked up here. */
  modules_rescan();
  menu_draw();
}

static void menu_hide(int fd) {
  static const unsigned char seq_mode[4] = {0x85, 0x04, 0x04, 0x01};
  unsigned char fill[12];
  menu_open = 0;
  if (view_mode == 1) {
    /* Switch back to the view the panel has been keeping. Nothing is erased or
     * replayed on top of it, which is what keeps it intact. */
    raw_write(fd, seq_mode, sizeof seq_mode);
    return;
  }
  /* Erase the menu first: the replay below is the daemon's screen as it was,
   * and it does not have to repaint every box the menu used. */
  raw_write(fd, fill, fill_frame(fill, 0x00, 0, 0, 256, 64));
  if (capture_valid && capture_len > 0) {
    raw_write(fd, capture, capture_len);
  }
}

/* The daemon's screen, kept as the frames that produce it.
 *
 * A full-width fill at y=0 is the page-entry clear, and it is held rather than
 * kept: the home screen's burst ends with a whole-screen fill after its title
 * and part name, and that fill is followed by nothing. The daemon means nothing
 * by it -- the panel is what decides the clear is "pending", committed only
 * when something is drawn into it and forgotten otherwise. Applying it here
 * would make the menu close to a blank screen instead of the home page.
 *
 * Only id 0 (text) and id 1 (fill) are screen operations; the other class-0x85
 * ids are carried to the panel but draw nothing this replay needs. The lamps
 * are class 0x80 and are not touched by the menu at all. */
static void capture_append(const unsigned char* f, size_t n) {
  if (!capture_valid) return;
  if (capture_len + n > CAPTURE_MAX) {
    capture_valid = 0;
    capture_len = 0;
    return;
  }
  memcpy(capture + capture_len, f, n);
  capture_len += n;
}

static void capture_frame(const unsigned char* f, size_t n) {
  if (n < 3) return;

  /* The daemon's id 4 command names the panel mode: 1 for the sequencer view
   * it keeps, 0 for an ordinary page. Track it even while the menu is open, so
   * closing switches to whatever the daemon last asked for. */
  if (f[2] == 0x04 && n == 4) {
    view_mode = f[3] & 1;
  }

  if (f[2] == 0x01 && n == 12) {
    const unsigned w = (unsigned)f[8] | ((unsigned)f[9] << 7);
    const unsigned y = f[7];
    if (w >= 256 && y == 0) {
      memcpy(pending_fill, f, 12);
      pending_valid = 1;
      pending_whole = f[10] >= 64;
      return;                       /* held until something is drawn */
    }
  } else if (f[2] != 0x00) {
    return;                         /* not a screen operation */
  }

  /* Text or a region fill commits a held page-entry fill. A whole-screen one
   * is also a clean start for the replay, which is how the buffer stays small. */
  if (pending_valid) {
    if (pending_whole) {
      capture_valid = 1;
      capture_len = 0;
    }
    capture_append(pending_fill, 12);
    pending_valid = 0;
  }
  capture_append(f, n);
}

static size_t out_frame_len(const unsigned char* p, size_t avail) {
  if (avail < 2) return 0;
  switch (p[0]) {
    case 0x80:
      return avail >= 5 ? 5 : 0;
    case 0x81:
    case 0x85:
    case 0x86: {
      const size_t len = p[1];
      return (len >= 2 && len <= avail) ? len : 0;
    }
    default:
      return 1;
  }
}

static int chord_partner(unsigned char a, unsigned char b) {
  return (a == KEY_MUTE && b == KEY_MASTER) ||
         (a == KEY_MASTER && b == KEY_MUTE);
}

/* Rewrites the key frames read from the panel and returns the number of bytes
 * to hand to the daemon. A first chord key is reported as a release and kept
 * back; if the partner follows, neither down is ever delivered, so opening the
 * menu cannot leave MUTE armed or navigate the daemon's screen. If anything
 * else follows, the held down goes out ahead of it and the daemon sees the
 * press in the order it happened. */
static size_t panel_scan(int fd, unsigned char* buf, size_t got, size_t cap) {
  static unsigned char out[4096];
  size_t in = 0, o = 0, inserts = 0;
  panel_out_fd = fd;
  modules_load();

  /* Every kept frame is emitted once, so the output is the input plus one
   * three-byte down per held key delivered. The caller's buffer is always
   * larger than a key read; refuse to grow past it. */
  if (got > sizeof(out) - 16) return got;

  while (in + 3 <= got) {
    if (buf[in] == 0x80) {
      const unsigned char key = buf[in + 1];
      const unsigned char raw = buf[in + 2];
      const unsigned char down = raw != 0;
      unsigned char frame[3];
      int suppress = 0;
      int chord = 0;
      int hold = 0;

      frame[0] = 0x80;
      frame[1] = key;
      frame[2] = raw;

      if (held_valid) {
        if (down && chord_partner(held_key, key)) {
          /* The partner: the gesture was a chord, so the held down and this
           * one both stay back. */
          held_valid = 0;
          chord = 1;
          suppress = 1;
        } else if (got + inserts + 1 <= cap) {
          /* Not a chord after all: the held press happened first. */
          out[o++] = 0x80;
          out[o++] = held_key;
          out[o++] = 0x01;
          inserts++;
          held_valid = 0;
        }
        /* No room: keep it held for a later read. */
      }

      if (key == KEY_MUTE) mute_held = down;
      if (key == KEY_MASTER) master_held = down;

      /* The chord is both keys held, in either order. Releasing either key
       * arms it again; the latch keeps a repeated frame from toggling twice. */
      if (!mute_held || !master_held) chord_latch = 0;

      if (chord) {
        chord_latch = 1;
        if (menu_open) {
          menu_hide(fd);
        } else {
          menu_show(fd);
        }
      } else if (down && mute_held && master_held && !chord_latch) {
        chord_latch = 1;
        chord = 1;
        suppress = 1;
        if (menu_open) {
          menu_hide(fd);
        } else {
          menu_show(fd);
        }
      }

      if (down && !chord && menu_open) {
        const int pages = page_count();
        if (key == KEY_PAGE_DOWN) {
          menu_page = (menu_page + 1) % pages;
          menu_draw();
        } else if (key == KEY_PAGE_UP) {
          menu_page = (menu_page + pages - 1) % pages;
          menu_draw();
        }
      }

      /* Hold the first chord key, but never a key pressed while the menu is
       * open: those are the closing chord and the page keys. */
      if (!chord && down && !menu_open && !held_valid &&
          (key == KEY_MUTE || key == KEY_MASTER) &&
          (key == KEY_MUTE ? !master_held : !mute_held)) {
        held_key = key;
        held_valid = 1;
        hold = 1;
      }

      if (menu_open || chord) suppress = 1;
      if (suppress || hold) frame[2] = 0x00;

      out[o++] = frame[0];
      out[o++] = frame[1];
      out[o++] = frame[2];
      in += 3;
    } else if (buf[in] == 0xfe) {
      size_t len = (in + 4 <= got) ? 4 : got - in;
      memcpy(out + o, buf + in, len);
      o += len;
      in += len;
    } else {
      out[o++] = buf[in++];
    }
  }
  while (in < got) out[o++] = buf[in++];

  /* The panel reports at 2 Hz even when no key moves. That is the tick a unit
   * page uses to follow playback; it runs on the panel thread. */
  if (menu_open) unit_page_redraw();

  if (o <= cap) {
    memcpy(buf, out, o);
    return o;
  }
  return got;
}

/* ---------------------------------------------------------------- open path */

static int (*real_open)(const char*, int, ...);
static int (*real_open64)(const char*, int, ...);
static int resolving_open;

static void ensure_open(void) {
  if (resolving_open) return;
  resolving_open = 1;
  if (real_open == NULL) real_open = (int (*)(const char*, int, ...))
      dlsym(RTLD_NEXT, "open");
  if (real_open64 == NULL) real_open64 = (int (*)(const char*, int, ...))
      dlsym(RTLD_NEXT, "open64");
  resolving_open = 0;
}

static int path_is_panel(const char* path) {
  return path != NULL && strstr(path, "ttymxc2") != NULL;
}

int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  va_list ap;
  int fd;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  ensure_open();
  if (resolving_open || real_open == NULL) {
    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, mode);
  } else {
    fd = real_open(path, flags, mode);
  }
  if (fd >= 0 && path_is_panel(path)) panel_add(fd);
  return fd;
}

int open64(const char* path, int flags, ...) {
  mode_t mode = 0;
  va_list ap;
  int fd;

  if (flags & (O_CREAT | O_TMPFILE)) {
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }

  ensure_open();
  if (resolving_open || real_open64 == NULL) {
    fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode);
  } else {
    fd = real_open64(path, flags, mode);
  }
  if (fd >= 0 && path_is_panel(path)) panel_add(fd);
  return fd;
}

int dup(int oldfd) {
  const int fd = (int)syscall(SYS_dup, oldfd);
  if (fd >= 0 && panel_is(oldfd)) panel_add(fd);
  return fd;
}

int close(int fd) {
  if (panel_is(fd)) panel_del(fd);
  return (int)syscall(SYS_close, fd);
}

/* read and write go through the syscall rather than the loader. Neither
 * needs the next object's help, and a dlsym on the first write of the
 * daemon's life is the one thing this must not do. */
ssize_t read(int fd, void* buf, size_t n) {
  const ssize_t got = (ssize_t)syscall(SYS_read, fd, buf, n);
  if (got > 0 && panel_is(fd)) {
    return (ssize_t)panel_scan(fd, (unsigned char*)buf, (size_t)got, n);
  }
  return got;
}

ssize_t write(int fd, const void* buf, size_t n) {
  const unsigned char* p = (const unsigned char*)buf;
  size_t i = 0;

  if (!panel_is(fd)) return (ssize_t)syscall(SYS_write, fd, buf, n);

  /* The daemon paints as soon as it is up, so this usually loads the modules
   * before the first key is pressed. The loader is off the audio path. */
  modules_load();

  /* With the menu closed the write goes through untouched; it is parsed only
   * to keep the replay current. */
  if (!menu_open) {
    const ssize_t got = (ssize_t)syscall(SYS_write, fd, buf, n);
    if (got <= 0) return got;
    while (i < n) {
      const size_t len = out_frame_len(p + i, n - i);
      if (len == 0) break;
      if (p[i] == 0x85) capture_frame(p + i, len);
      i += len;
    }
    return got;
  }

  /* With the menu open every screen frame is dropped, however large the write
   * is and wherever the frame sits in it. The main page can refresh in one
   * buffer much bigger than a single frame; passing that through would paint
   * over the menu. The lamps and the other classes are kept, one frame at a
   * time. */
  while (i < n) {
    size_t len = out_frame_len(p + i, n - i);
    if (len == 0) {
      if (p[i] == 0x85) break;      /* screen data that cannot be measured */
      len = n - i;
    }
    if (p[i] == 0x85) {
      capture_frame(p + i, len);
    } else if (len > 0 && syscall(SYS_write, fd, p + i, len) < 0) {
      return -1;
    }
    i += len;
  }
  return (ssize_t)n;
}

/* ----------------------------------------------------------------- the MIDI */

typedef struct _snd_rawmidi snd_rawmidi_t;

static unsigned char midi_status;
static unsigned midi_need;
static unsigned midi_have;
static int midi_sysex;
static int midi_first_here;
static size_t midi_first;

static void midi_note(unsigned char status, size_t note_at, size_t vel_at,
                      unsigned char* buf) {
  int note = buf[note_at];
  int vel = buf[vel_at];

  if (midi_transpose != 0) {
    note += midi_transpose;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    buf[note_at] = (unsigned char)note;
  }
  if ((status & 0xf0) == 0x90 && vel > 0 && midi_velocity != 100) {
    vel = (vel * midi_velocity + 50) / 100;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;
    buf[vel_at] = (unsigned char)vel;
  }

  midi_notes++;
  midi_last_status = status;
  midi_last_note = (unsigned long)buf[note_at];
  midi_last_vel = (unsigned long)buf[vel_at];

  {
    char line[64];
    snprintf(line, sizeof line, "note %02x %02x %02x\n", status,
             buf[note_at], buf[vel_at]);
    raw_log(LOGUE_BORG_MIDI_LOG, line);
  }
}

static void midi_account(unsigned char* buf, size_t n) {
  size_t i;

  if (midi_have > 0) midi_first_here = 0;

  for (i = 0; i < n; i++) {
    const unsigned char b = buf[i];

    if (b >= 0xf8) continue;             /* realtime, no state */
    if (midi_sysex) {
      if (b == 0xf7) midi_sysex = 0;
      continue;
    }
    if (b == 0xf0) {
      midi_sysex = 1;
      midi_status = 0;
      midi_need = midi_have = 0;
      continue;
    }
    if (b == 0xf7) {
      midi_status = 0;
      midi_need = midi_have = 0;
      continue;
    }
    if (b >= 0xf0) {                     /* system common: not transformed */
      midi_status = 0;
      midi_need = midi_have = 0;
      continue;
    }
    if (b >= 0x80) {
      midi_status = b;
      midi_need = ((b & 0xf0) == 0xc0 || (b & 0xf0) == 0xd0) ? 1 : 2;
      midi_have = 0;
      midi_first_here = 1;
      midi_first = i + 1;
      midi_in_msgs++;
      continue;
    }
    if (midi_status == 0) continue;      /* data with no running status */

    if (midi_have == 0) {
      midi_first = i;
      midi_first_here = 1;
    }
    midi_have++;

    if (midi_have == midi_need) {
      const unsigned char status = midi_status;
      if (midi_first_here &&
          ((status & 0xf0) == 0x90 || (status & 0xf0) == 0x80) &&
          midi_first + 1 < n) {
        midi_note(status, midi_first, midi_first + 1, buf);
      }
      midi_have = 0;
      midi_first_here = 0;
    }
  }
}

ssize_t snd_rawmidi_read(snd_rawmidi_t* rm, void* buf, size_t n) {
  static ssize_t (*real)(snd_rawmidi_t*, void*, size_t);
  if (real == NULL) real = (ssize_t (*)(snd_rawmidi_t*, void*, size_t))
      dlsym(RTLD_NEXT, "snd_rawmidi_read");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const ssize_t got = real(rm, buf, n);
    if (got > 0) {
      midi_in_bytes += (unsigned long)got;
      midi_account((unsigned char*)buf, (size_t)got);
    }
    return got;
  }
}

ssize_t snd_rawmidi_write(snd_rawmidi_t* rm, const void* buf, size_t n) {
  static ssize_t (*real)(snd_rawmidi_t*, const void*, size_t);
  if (real == NULL) real = (ssize_t (*)(snd_rawmidi_t*, const void*, size_t))
      dlsym(RTLD_NEXT, "snd_rawmidi_write");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const ssize_t put = real(rm, buf, n);
    if (put > 0) {
      const unsigned char* p = (const unsigned char*)buf;
      size_t i;
      midi_out_bytes += (unsigned long)put;
      for (i = 0; i < (size_t)put; i++) {
        if (p[i] >= 0x80 && p[i] < 0xf0) midi_out_msgs++;
      }
    }
    return put;
  }
}

/* ----------------------------------------------------------------- the audio */

typedef struct _snd_pcm snd_pcm_t;

long snd_pcm_writei(snd_pcm_t* pcm, const void* buf, unsigned long frames) {
  static long (*real)(snd_pcm_t*, const void*, unsigned long);
  if (real == NULL) real = (long (*)(snd_pcm_t*, const void*, unsigned long))
      dlsym(RTLD_NEXT, "snd_pcm_writei");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const long got = real(pcm, buf, frames);
    audio_calls++;
    if (got < 0) audio_errors++;
    return got;
  }
}

long snd_pcm_mmap_writei(snd_pcm_t* pcm, const void* buf, unsigned long frames) {
  static long (*real)(snd_pcm_t*, const void*, unsigned long);
  if (real == NULL)
    real = (long (*)(snd_pcm_t*, const void*, unsigned long))
        dlsym(RTLD_NEXT, "snd_pcm_mmap_writei");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const long got = real(pcm, buf, frames);
    audio_calls++;
    if (got < 0) audio_errors++;
    return got;
  }
}
