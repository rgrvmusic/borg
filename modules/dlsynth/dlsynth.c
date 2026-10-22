/* dlsynth — a second drumlogue synth, inside borg, on one MIDI channel.
 *
 * borg already runs in the daemon's process and already interposes the daemon's
 * ALSA playback and raw-MIDI reads. This module uses the ABI 4 additions those
 * hooks provide: complete channel messages are offered on the MIDI thread, and
 * a zeroed float stereo block is offered on the audio thread, mixed into what
 * the daemon writes to the codec.
 *
 * The unit is an ordinary `.drmlgunit`, loaded from the daemon's own
 * Units/Synths directory with the same 26-byte runtime descriptor and the same
 * 22 callbacks the daemon uses. Only synth units are considered: an effect
 * would never receive a note, because its ABI has no note callbacks, and the
 * filter here is the header's own `target` field, read from the file rather
 * than guessed from its name.
 *
 * The unit is copied to /tmp before dlopen. The daemon may already have the
 * same file mapped for one of its parts, and `dlopen` returns that same mapping
 * for the same file; calling `unit_init` on it again would reset the part the
 * user is playing. A copy is a distinct mapping and therefore a distinct
 * instance, with no state shared with the daemon's own unit. It is unlinked
 * immediately after `dlopen`; the mapping stays.
 *
 * Everything that touches the unit is either on the panel thread (load and
 * teardown) or on the audio thread (render). The panel gates the audio thread
 * with a flag and a short busy handshake before it tears anything down. MIDI
 * arrives on the MIDI thread, so it only queues small events; the audio thread
 * applies them to the unit. That keeps every unit callback except init and
 * teardown on one thread, and keeps the MIDI thread from calling into a unit
 * that the panel is about to close.
 *
 * No process and no thread is created. The only memory allocated after init is
 * the unit's own, inside its `.drmlgunit`. */

/* readdir() on a 32-bit host uses the old getdents and fails with EOVERFLOW on
 * a filesystem whose inode numbers do not fit in 32 bits -- which is what the
 * development machine's overlay filesystem has. The unit directory then looks
 * empty even though every file in it opens. Asking for 64-bit offsets selects
 * readdir64 and the 64-bit getdents, and fixes pread's offset at the same
 * time. It must come before the first system header. */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "borg_module.h"
/* The unit ABI, not a host service: this module dlsyms the units themselves,
 * so the declarations travel with it. build-module.sh adds libborg/ to the
 * include path and this file sits beside the source. */
#include "drumlogue_abi.h"

#define DLSYNTH_DIR_MAX 512
#define DLSYNTH_FILE_MAX 96
#define DLSYNTH_NAME_MAX 12      /* the page's value column */
#define DLSYNTH_LIST_MAX 32
#define DLSYNTH_CHUNK 64         /* what real units cap their render at */
#define DLSYNTH_RING 256         /* MIDI events between the two threads */
#define DLSYNTH_DEFAULT_RATE 48000u
#define DLSYNTH_COPY_TRIES 4

/* The unit's symbol names, in one place. */
#define SYM_INIT "unit_init"
#define SYM_TEARDOWN "unit_teardown"
#define SYM_RESUME "unit_resume"
#define SYM_SUSPEND "unit_suspend"
#define SYM_RENDER "unit_render"
#define SYM_SET_PARAM "unit_set_param_value"
#define SYM_NOTE_ON "unit_note_on"
#define SYM_NOTE_OFF "unit_note_off"
#define SYM_ALL_NOTES_OFF "unit_all_note_off"
#define SYM_PITCH_BEND "unit_pitch_bend"
#define SYM_CHANNEL_PRESSURE "unit_channel_pressure"
#define SYM_AFTERTOUCH "unit_aftertouch"
#define SYM_HEADER "unit_header"

typedef struct dlsynth_entry {
  char file[DLSYNTH_FILE_MAX];
  char name[DLSYNTH_NAME_MAX + 1];
} dlsynth_entry;

typedef struct dlsynth_unit {
  void* handle;
  const unit_header_t* header;
  unit_init_ptr init;
  unit_teardown_ptr teardown;
  unit_resume_ptr resume;
  unit_suspend_ptr suspend;
  unit_render_ptr render;
  unit_set_param_value_ptr set_param;
  unit_note_on_ptr note_on;
  unit_note_off_ptr note_off;
  unit_all_note_off_ptr all_notes_off;
  unit_pitch_bend_ptr pitch_bend;
  unit_channel_pressure_ptr channel_pressure;
  unit_aftertouch_ptr aftertouch;
  int ready;
} dlsynth_unit;

/* Events are three bytes so the ring is a plain array with no padding and the
 * write is one relaxed store per field with a release on the index. */
enum {
  EV_NOTE_ON = 1,
  EV_NOTE_OFF,
  EV_PITCH,
  EV_PRESSURE,
  EV_AFTERTOUCH,
  EV_ALL_OFF
};

typedef struct dlsynth_event {
  uint8_t type;
  uint8_t a;
  uint8_t b;
} dlsynth_event;

static const borg_host* g_host;
static char g_dir[DLSYNTH_DIR_MAX];
static dlsynth_entry g_list[DLSYNTH_LIST_MAX];
static int g_count;
static int g_selected = -1;

static unsigned g_channel = 1;
static char g_state[DLSYNTH_NAME_MAX + 1] = "NO UNIT";
static unsigned long g_notes;
static unsigned long g_dropped;
/* Render accounting, for the page: whether the host asked for audio at all, and
 * how loud the unit's own output was. Those are the two facts that separate an
 * audio path that is not mixing from a unit that renders silence -- the host's
 * call counter alone cannot, because it counts the daemon's writes whether or
 * not a module was mixed into them. */
static unsigned long g_renders;
static int g_peak_pct;
/* The unit's own render, in microseconds: the worst block since the unit was
 * loaded. This is the number that decides whether the second synth can live
 * inside the instrument's 1333 us period, and it is published on the panel
 * because the log that would otherwise carry it dies with a wedged daemon. */
static unsigned long g_render_us_max;

/* The loaded unit. `ready` is published only after init and every declared
 * parameter default has been pushed, and cleared before teardown. */
static dlsynth_unit g_unit;

/* Producers are serialized by the host's module_call_lock: the MIDI thread's
 * midi_msg and the panel thread's encoder/key never run together. The audio
 * thread is the only consumer. */
static dlsynth_event g_ring[DLSYNTH_RING];
static unsigned g_head;
static unsigned g_tail;

static int g_audio_on;
static int g_audio_busy;
static unsigned g_copy_seq;

/* A unit is rendered only while it has something to say. The daemon does the
 * same -- a drumlogue part's render is gated on its voice being active -- and
 * on the instrument's single Cortex-A7 the difference is not academic: a unit
 * rendering silence still costs about a fifth of the core, and with the
 * daemon's own multi engine playing, that is enough to miss the deadline. The
 * stream's period is 64 frames (1.33 ms) written in 32-frame halves, so each
 * write has 667 us of audio time to itself.
 * The gate opens on any event (a note starts a voice) and closes only after
 * this many blocks have produced silence. At the daemon's 32-frame write that
 * is ten seconds; a unit that keeps producing sound, including one that runs on
 * render alone, never reaches it because every non-silent block resets the
 * count. */
#define DLSYNTH_IDLE_BLOCKS 15000
static unsigned long g_idle_blocks;

/* What counts as silence for the gate. Not `== 0.0f`: an oscillator unit with
 * no note keeps emitting denormal tails and rounding dust, and on this core a
 * denormal is not free -- but at -120 dBFS it is inaudible either way. A value
 * below the floor is written out as an exact zero, so the host's conversion
 * and the gate both see silence. NaN is zeroed too: a unit that produces one
 * must not poison the daemon's block. */
#define DLSYNTH_SILENCE 1e-6f

/* The zeroed input buffer handed to the unit. A synth declares no input
 * channels, but passing a valid pointer costs 512 bytes and removes a way for
 * a unit that reads its input anyway to fault. */
static float g_input[DLSYNTH_CHUNK * 2];

/* ------------------------------------------------------------- small helpers */

static void set_state(const char* s) {
  snprintf(g_state, sizeof g_state, "%s", s);
}

static void dlog(const char* fmt, ...) {
  char line[256];
  va_list ap;

  if (g_host == NULL || g_host->log == NULL) return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  g_host->log(line);
}

/* Monotonic microseconds, for timing one unit render. clock_gettime costs tens
 * of nanoseconds through the vDSO on this board, which is noise beside the
 * render it is measuring. */
static unsigned long long now_us(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (unsigned long long)ts.tv_sec * 1000000ULL +
         (unsigned long long)ts.tv_nsec / 1000;
}

static int ends_with(const char* s, const char* suffix) {
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);

  return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* The panel stops a text run at a comma and the value column is 12 characters.
 * A unit may call itself anything, so make the bytes safe and short. */
static void clean_name(char* dst, size_t n, const char* src) {
  size_t i = 0;

  while (src != NULL && *src != '\0' && i + 1 < n) {
    const unsigned char c = (unsigned char)*src++;
    dst[i++] = (c >= 0x20 && c < 0x7f && c != ',') ? (char)c : ' ';
  }
  dst[i] = '\0';
}

static void stem_of(char* dst, size_t n, const char* file) {
  const char* dot = strrchr(file, '.');
  size_t len = dot != NULL ? (size_t)(dot - file) : strlen(file);

  if (len >= n) len = n - 1;
  memcpy(dst, file, len);
  dst[len] = '\0';
}

/* --------------------------------------------------------- reading the header
 *
 * The list must contain synth units only, and a unit is not run to find that
 * out. `unit_header` is an exported object, so the file's own ELF tables name
 * where it is; the target and the display name are read from those bytes.
 * This avoids dlopen()ing every candidate, which would run constructors of
 * effect units that are only being listed, and would need a matching
 * dlclose(). */

static int read_at(int fd, void* p, size_t n, off_t off) {
  unsigned char* q = (unsigned char*)p;

  while (n > 0) {
    const ssize_t got = pread(fd, q, n, off);
    if (got < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (got == 0) return -1;
    q += got;
    n -= (size_t)got;
    off += got;
  }
  return 0;
}

/* 0 when a `unit_header` was found and copied out, -1 otherwise. The target is
 * not filtered here: the caller does that, so a caller could still report what
 * it found. */
static int read_unit_header(const char* path, unit_header_t* out) {
  int fd = -1, rc = -1;
  Elf32_Ehdr eh;
  Elf32_Shdr* sh = NULL;
  Elf32_Sym* syms = NULL;
  char* str = NULL;
  int symtab = -1;
  unsigned i;

  fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  if (read_at(fd, &eh, sizeof eh, 0) != 0) goto done;
  if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS32 ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB ||
      eh.e_type != ET_DYN || eh.e_machine != EM_ARM ||
      eh.e_shnum == 0 || eh.e_shnum > 128 ||
      eh.e_shentsize != sizeof(Elf32_Shdr)) {
    goto done;
  }

  sh = (Elf32_Shdr*)calloc(eh.e_shnum, sizeof *sh);
  if (sh == NULL) goto done;
  if (read_at(fd, sh, (size_t)eh.e_shnum * sizeof *sh, eh.e_shoff) != 0) {
    goto done;
  }
  for (i = 0; i < eh.e_shnum; i++) {
    if (sh[i].sh_type == SHT_DYNSYM) {
      symtab = (int)i;
      break;
    }
  }
  if (symtab < 0 || sh[symtab].sh_link >= eh.e_shnum ||
      sh[symtab].sh_entsize != sizeof(Elf32_Sym) ||
      sh[symtab].sh_size == 0 || sh[symtab].sh_size > (1u << 20)) {
    goto done;
  }

  {
    const Elf32_Shdr* st = &sh[sh[symtab].sh_link];
    if (st->sh_type != SHT_STRTAB || st->sh_size == 0 ||
        st->sh_size > (1u << 20)) {
      goto done;
    }
    str = (char*)malloc((size_t)st->sh_size + 1);
    if (str == NULL) goto done;
    if (read_at(fd, str, (size_t)st->sh_size, st->sh_offset) != 0) goto done;
    str[st->sh_size] = '\0';
  }

  syms = (Elf32_Sym*)malloc((size_t)sh[symtab].sh_size);
  if (syms == NULL) goto done;
  if (read_at(fd, syms, (size_t)sh[symtab].sh_size, sh[symtab].sh_offset) != 0) {
    goto done;
  }

  for (i = 0; i < sh[symtab].sh_size / sizeof(Elf32_Sym); i++) {
    const Elf32_Sym* s = &syms[i];
    uint32_t v;
    unsigned j;

    if (s->st_name == 0 || s->st_name >= sh[sh[symtab].sh_link].sh_size) {
      continue;
    }
    if (strcmp(str + s->st_name, SYM_HEADER) != 0) continue;
    if (ELF32_ST_TYPE(s->st_info) != STT_OBJECT ||
        s->st_shndx == SHN_UNDEF || s->st_value == 0) {
      continue;
    }

    v = s->st_value;
    for (j = 0; j < eh.e_shnum; j++) {
      const Elf32_Shdr* sec = &sh[j];
      if (sec->sh_type == SHT_NOBITS || sec->sh_addr == 0) continue;
      if (v < sec->sh_addr || v - sec->sh_addr > sec->sh_size) continue;
      if (sizeof(unit_header_t) > sec->sh_size - (v - sec->sh_addr)) continue;
      if (read_at(fd, out, sizeof *out,
                  (off_t)sec->sh_offset + (off_t)(v - sec->sh_addr)) != 0) {
        goto done;
      }
      if (out->header_size < 44 || out->header_size > sizeof *out ||
          out->num_params > UNIT_MAX_PARAM_COUNT) {
        goto done;
      }
      rc = 0;
      goto done;
    }
  }

done:
  free(syms);
  free(str);
  free(sh);
  if (fd >= 0) close(fd);
  return rc;
}

static int entry_sort(const void* a, const void* b) {
  const dlsynth_entry* x = (const dlsynth_entry*)a;
  const dlsynth_entry* y = (const dlsynth_entry*)b;
  int c = strcmp(x->name, y->name);

  return c != 0 ? c : strcmp(x->file, y->file);
}

/* The list is rebuilt when the directory changes, not on every page open. Each
 * candidate costs a header read from userfs flash, and flash I/O on this board
 * can keep the core long enough to miss an audio block: the page is opened
 * while the instrument is playing, so scanning a dozen units each time it is
 * shown is heard as artifacts. Adding or removing a file changes the
 * directory's mtime, which is what re-arms the scan. */
static time_t g_scan_sec;
static long g_scan_nsec;
static int g_scanned;

/* Rebuild the list. Synth units only, by the header's target field; an effect,
 * a delay or a master effect is not listed at all and can therefore not be
 * selected. The selected row is kept by file name when it still exists. */
static void scan_units(void) {
  DIR* d;
  struct dirent* e;
  char keep[DLSYNTH_FILE_MAX] = "";
  struct stat st;

  if (g_scanned && stat(g_dir, &st) == 0 && st.st_mtime == g_scan_sec &&
      st.st_mtim.tv_nsec == g_scan_nsec) {
    return;
  }
  if (stat(g_dir, &st) == 0) {
    g_scan_sec = st.st_mtime;
    g_scan_nsec = st.st_mtim.tv_nsec;
  }

  if (g_selected >= 0 && g_selected < g_count) {
    snprintf(keep, sizeof keep, "%s", g_list[g_selected].file);
  }

  g_count = 0;
  d = opendir(g_dir);
  if (d == NULL) {
    set_state(g_unit.ready ? "READY" : "NO DIR");
    g_selected = -1;
    return;
  }
  g_scanned = 1;
  while ((e = readdir(d)) != NULL && g_count < DLSYNTH_LIST_MAX) {
    char path[DLSYNTH_DIR_MAX + DLSYNTH_FILE_MAX];
    char hname[UNIT_NAME_LEN + 1];
    unit_header_t h;
    dlsynth_entry* slot;

    if (e->d_name[0] == '.' || !ends_with(e->d_name, ".drmlgunit")) continue;
    if (strlen(e->d_name) >= DLSYNTH_FILE_MAX) continue;
    snprintf(path, sizeof path, "%.*s/%.*s", DLSYNTH_DIR_MAX - 1, g_dir,
             DLSYNTH_FILE_MAX - 1, e->d_name);
    memset(&h, 0, sizeof h);
    if (read_unit_header(path, &h) != 0) continue;
    if (h.target != k_unit_target_drumlogue_synth) continue;

    slot = &g_list[g_count];
    snprintf(slot->file, sizeof slot->file, "%.*s",
             DLSYNTH_FILE_MAX - 1, e->d_name);
    /* The 13-character field is not trusted to be terminated; the struct is
     * packed and a unit's own bytes are the only bounds that exist. */
    memcpy(hname, h.name, UNIT_NAME_LEN);
    hname[UNIT_NAME_LEN] = '\0';
    clean_name(slot->name, sizeof slot->name, hname);
    if (slot->name[0] == '\0') {
      stem_of(slot->name, sizeof slot->name, e->d_name);
    }
    g_count++;
  }
  closedir(d);

  qsort(g_list, (size_t)g_count, sizeof g_list[0], entry_sort);

  g_selected = g_count > 0 ? 0 : -1;
  for (int i = 0; i < g_count; i++) {
    if (keep[0] != '\0' && strcmp(g_list[i].file, keep) == 0) {
      g_selected = i;
      break;
    }
  }
  if (g_count == 0) {
    set_state(g_unit.ready ? "READY" : "NO SYNTH");
  } else if (strcmp(g_state, "NO SYNTH") == 0 ||
             strcmp(g_state, "NO DIR") == 0) {
    /* A directory that was empty on the last scan and has units now must not
     * keep saying so. A load failure is left alone: the reason is more useful
     * than a generic state, and the next load attempt replaces it. */
    set_state(g_unit.ready ? "READY" : "NO UNIT");
  }
}

/* -------------------------------------------------------------- the MIDI ring */

static void push_event(uint8_t type, uint8_t a, uint8_t b) {
  const unsigned head = __atomic_load_n(&g_head, __ATOMIC_RELAXED);
  const unsigned next = (head + 1) & (DLSYNTH_RING - 1);

  if (next == __atomic_load_n(&g_tail, __ATOMIC_ACQUIRE)) {
    g_dropped++;
    return;
  }
  g_ring[head].type = type;
  g_ring[head].a = a;
  g_ring[head].b = b;
  __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
}

/* Called only on the audio thread, with the unit ready and the load gate open.
 * The unit callbacks below are the only place the queued events become unit
 * calls, which is what keeps the MIDI thread out of the unit. */
/* Returns 1 when anything was applied, so the caller can wake a render gate
 * even for an event this build does not map. */
static int drain_events(void) {
  unsigned tail = g_tail;
  int applied = 0;

  while (tail != __atomic_load_n(&g_head, __ATOMIC_ACQUIRE)) {
    const dlsynth_event e = g_ring[tail];
    tail = (tail + 1) & (DLSYNTH_RING - 1);
    applied = 1;

    switch (e.type) {
      case EV_NOTE_ON:
        if (g_unit.note_on != NULL) g_unit.note_on(e.a, e.b);
        break;
      case EV_NOTE_OFF:
        if (g_unit.note_off != NULL) g_unit.note_off(e.a);
        break;
      case EV_PITCH:
        if (g_unit.pitch_bend != NULL) {
          g_unit.pitch_bend((uint16_t)(e.a | ((uint16_t)e.b << 7)));
        }
        break;
      case EV_PRESSURE:
        if (g_unit.channel_pressure != NULL) {
          g_unit.channel_pressure(e.a);
        }
        break;
      case EV_AFTERTOUCH:
        if (g_unit.aftertouch != NULL) g_unit.aftertouch(e.a, e.b);
        break;
      case EV_ALL_OFF:
        if (g_unit.all_notes_off != NULL) g_unit.all_notes_off();
        break;
      default:
        break;
    }
  }
  __atomic_store_n(&g_tail, tail, __ATOMIC_RELEASE);
  return applied;
}

/* --------------------------------------------------------------- the unit */

static uint8_t sample_banks(void) {
  return 0;
}

static uint8_t samples_for_bank(uint8_t bank) {
  (void)bank;
  return 0;
}

static const sample_wrapper_t* sample_get(uint8_t bank, uint8_t index) {
  (void)bank;
  (void)index;
  return NULL;
}

/* Open a private copy of the unit. The copy exists so that selecting the unit
 * the daemon already has on a part gives a separate instance rather than the
 * same dlopen handle; see the file comment. */
static void* open_unit_copy(const char* src, char* err, size_t errn) {
  char dst[DLSYNTH_DIR_MAX + 64];
  /* 8 KB, not 64: this runs on the daemon's panel thread, whose stack is the
   * daemon's to size, and a sixty-four kilobyte frame under dlopen and snprintf
   * is a stack overflow waiting for a thread that was given less. The copy
   * loop does not care how big each read is. */
  unsigned char buf[8192];
  int in, out = -1;
  void* handle;
  unsigned tries;

  in = open(src, O_RDONLY);
  if (in < 0) {
    snprintf(err, errn, "OPEN");
    return NULL;
  }

  for (tries = 0; tries < DLSYNTH_COPY_TRIES; tries++) {
    snprintf(dst, sizeof dst, "%.*s/dlsynth-%ld-%u.drmlgunit",
             DLSYNTH_DIR_MAX - 1, g_host->path(BORG_PATH_TMP),
             (long)getpid(), g_copy_seq++);
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out >= 0) break;
    if (errno != EEXIST) break;
  }
  if (out < 0) {
    close(in);
    snprintf(err, errn, "TMP");
    return NULL;
  }

  for (;;) {
    const ssize_t got = read(in, buf, sizeof buf);
    ssize_t done = 0;

    if (got < 0) {
      if (errno == EINTR) continue;
      goto copy_fail;
    }
    if (got == 0) break;
    while (done < got) {
      const ssize_t put = write(out, buf + done, (size_t)(got - done));
      if (put < 0) {
        if (errno == EINTR) continue;
        goto copy_fail;
      }
      done += put;
    }
  }
  close(in);
  if (close(out) != 0) {
    unlink(dst);
    snprintf(err, errn, "COPY");
    return NULL;
  }

  handle = dlopen(dst, RTLD_NOW | RTLD_LOCAL);
  unlink(dst);                      /* the mapping does not need the name */
  if (handle == NULL) {
    const char* why = dlerror();
    snprintf(err, errn, "%.*s", (int)(errn - 1),
             why != NULL ? why : "LOAD");
    return NULL;
  }
  return handle;

copy_fail:
  close(in);
  close(out);
  unlink(dst);
  snprintf(err, errn, "COPY");
  return NULL;
}

static void close_unit(void) {
  if (g_unit.handle == NULL) return;
  g_unit.ready = 0;
  if (g_unit.suspend != NULL) g_unit.suspend();
  if (g_unit.teardown != NULL) g_unit.teardown();
  dlclose(g_unit.handle);
  memset(&g_unit, 0, sizeof g_unit);
}

/* Returns 0 and fills `err` with a short reason on failure. The unit is not
 * visible to the audio thread until every callback and every declared
 * parameter default has been applied. */
static int open_unit(int index, char* err, size_t errn) {
  char path[DLSYNTH_DIR_MAX + DLSYNTH_FILE_MAX];
  dlsynth_unit u;
  unit_runtime_desc_t desc;
  unsigned i, np;
  int inited = 0;

  memset(&u, 0, sizeof u);
  snprintf(path, sizeof path, "%.*s/%.*s", DLSYNTH_DIR_MAX - 1, g_dir,
           DLSYNTH_FILE_MAX - 1, g_list[index].file);

  u.handle = open_unit_copy(path, err, errn);
  if (u.handle == NULL) return -1;

  u.header = (const unit_header_t*)dlsym(u.handle, SYM_HEADER);
  if (u.header == NULL) {
    snprintf(err, errn, "NO HEADER");
    goto fail;
  }
  if (u.header->target != k_unit_target_drumlogue_synth) {
    snprintf(err, errn, "NOT SYNTH");
    goto fail;
  }
  if (u.header->api != UNIT_API_VERSION_2_0_0) {
    snprintf(err, errn, "API %08x", (unsigned)u.header->api);
    goto fail;
  }

  u.init = (unit_init_ptr)dlsym(u.handle, SYM_INIT);
  u.teardown = (unit_teardown_ptr)dlsym(u.handle, SYM_TEARDOWN);
  u.resume = (unit_resume_ptr)dlsym(u.handle, SYM_RESUME);
  u.suspend = (unit_suspend_ptr)dlsym(u.handle, SYM_SUSPEND);
  u.render = (unit_render_ptr)dlsym(u.handle, SYM_RENDER);
  u.set_param = (unit_set_param_value_ptr)dlsym(u.handle, SYM_SET_PARAM);
  u.note_on = (unit_note_on_ptr)dlsym(u.handle, SYM_NOTE_ON);
  u.note_off = (unit_note_off_ptr)dlsym(u.handle, SYM_NOTE_OFF);
  u.all_notes_off = (unit_all_note_off_ptr)dlsym(u.handle, SYM_ALL_NOTES_OFF);
  u.pitch_bend = (unit_pitch_bend_ptr)dlsym(u.handle, SYM_PITCH_BEND);
  u.channel_pressure = (unit_channel_pressure_ptr)
      dlsym(u.handle, SYM_CHANNEL_PRESSURE);
  u.aftertouch = (unit_aftertouch_ptr)dlsym(u.handle, SYM_AFTERTOUCH);
  if (u.init == NULL || u.render == NULL) {
    snprintf(err, errn, "NO CALLBACK");
    goto fail;
  }

  memset(&desc, 0, sizeof desc);
  desc.target = k_unit_target_drumlogue_synth;
  desc.api = UNIT_API_VERSION_2_0_0;
  desc.samplerate =
      (g_host->size >= offsetof(borg_host, samplerate) + sizeof(uint32_t) &&
       g_host->samplerate != 0)
          ? g_host->samplerate
          : DLSYNTH_DEFAULT_RATE;
  desc.frames_per_buffer = DLSYNTH_CHUNK;
  desc.input_channels = 0;
  desc.output_channels = 2;
  desc.get_num_sample_banks = sample_banks;
  desc.get_num_samples_for_bank = samples_for_bank;
  desc.get_sample = sample_get;

  if (u.init(&desc) != 0) {
    /* The unit refused; it has cleaned up after itself. Teardown is only
     * valid for an init that succeeded. */
    snprintf(err, errn, "INIT");
    goto fail;
  }
  inited = 1;

  /* An unpushed parameter reads as NaN in real units, which peak/rms reporting
   * then hides behind a zero. Every declared default goes in, not just the
   * first. */
  np = u.header->num_params;
  if (np > UNIT_MAX_PARAM_COUNT) np = UNIT_MAX_PARAM_COUNT;
  if (np > 0 && u.set_param == NULL) {
    snprintf(err, errn, "NO PARAMS");
    goto fail;
  }
  for (i = 0; i < np; i++) {
    u.set_param((uint8_t)i, u.header->params[i].init);
  }
  if (u.resume != NULL) u.resume();

  /* Warm the unit here, on the panel thread, before the audio gate opens.
   * The first render is not like the others: its code pages are not resident
   * yet and its PLT entries still resolve on first call. On the audio thread
   * each of those page faults and bindings is a missed 1.33 ms deadline, which
   * is heard as artifacts exactly at the moment the unit is loaded. The output
   * is discarded and 64 frames of silence advances nothing audible.
   *
   * A unit whose first render faults does it here, where the crash report
   * names the unit and the audio thread is not holding a period open. */
  {
    float warm[DLSYNTH_CHUNK * 2];
    memset(warm, 0, sizeof warm);
    u.render(g_input, warm, DLSYNTH_CHUNK);
  }

  u.ready = 1;
  g_unit = u;
  return 0;

fail:
  if (inited && u.teardown != NULL) u.teardown();
  dlclose(u.handle);
  return -1;
}

/* Stop the audio thread and wait for it to leave the unit. 200 ms is far
 * longer than a block; a timeout means it is stuck rather than busy, and the
 * load is refused instead of closing a unit under it. */
static int audio_stop(void) {
  int i;

  __atomic_store_n(&g_audio_on, 0, __ATOMIC_SEQ_CST);
  for (i = 0; i < 200; i++) {
    if (!__atomic_load_n(&g_audio_busy, __ATOMIC_SEQ_CST)) return 0;
    usleep(1000);
  }
  return -1;
}

static void load_index(int index) {
  char err[64];

  if (index < 0 || index >= g_count) {
    set_state("NO UNIT");
    return;
  }
  if (audio_stop() != 0) {
    __atomic_store_n(&g_audio_on, 1, __ATOMIC_SEQ_CST);
    set_state("BUSY");
    dlog("dlsynth: audio busy, not switching");
    return;
  }

  close_unit();
  __atomic_store_n(&g_tail, __atomic_load_n(&g_head, __ATOMIC_ACQUIRE),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_notes, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_renders, 0, __ATOMIC_RELAXED);
  g_idle_blocks = 0;
  __atomic_store_n(&g_peak_pct, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_render_us_max, 0, __ATOMIC_RELAXED);
  g_dropped = 0;
  set_state("LOADING");
  err[0] = '\0';
  if (open_unit(index, err, sizeof err) != 0) {
    set_state(err);
    dlog("dlsynth: %s: %s", g_list[index].file, err);
  } else {
    set_state("READY");
    g_selected = index;
    dlog("dlsynth: loaded %s (%.13s) on channel %u, %u Hz",
         g_list[index].file, g_unit.header->name, g_channel,
         (unsigned)(g_host->samplerate != 0 ? g_host->samplerate
                                            : DLSYNTH_DEFAULT_RATE));
  }
  __atomic_store_n(&g_audio_on, 1, __ATOMIC_SEQ_CST);
}

/* ------------------------------------------------------------- the callbacks */

static void dlsynth_audio(float* out, uint32_t frames) {
  uint32_t off;

  if (out == NULL || frames == 0) return;
  /* Sequentially consistent on both flags, not acquire/release. This is a
   * Dekker handshake with the panel thread: the audio thread reads the gate
   * then sets busy, the panel clears the gate then reads busy. With weaker
   * ordering both reads can see the old value -- the panel concludes the audio
   * thread is out and dlclose()s the unit while a render is about to start.
   * A unit unmapped under a running render is a SIGSEGV in drumlogued, and
   * nothing on this instrument survives that. */
  if (!__atomic_load_n(&g_audio_on, __ATOMIC_SEQ_CST)) return;
  __atomic_store_n(&g_audio_busy, 1, __ATOMIC_SEQ_CST);
  if (!__atomic_load_n(&g_audio_on, __ATOMIC_SEQ_CST) ||
      !__atomic_load_n(&g_unit.ready, __ATOMIC_SEQ_CST)) {
    __atomic_store_n(&g_audio_busy, 0, __ATOMIC_SEQ_CST);
    return;
  }

  if (drain_events()) g_idle_blocks = 0;
  if (g_idle_blocks > DLSYNTH_IDLE_BLOCKS) {
    __atomic_store_n(&g_audio_busy, 0, __ATOMIC_SEQ_CST);
    return;
  }
  for (off = 0; off < frames; off += DLSYNTH_CHUNK) {
    float temp[DLSYNTH_CHUNK * 2];
    const uint32_t n = (frames - off > DLSYNTH_CHUNK)
                           ? DLSYNTH_CHUNK
                           : frames - off;
    uint32_t i;

    memset(temp, 0, (size_t)n * 2 * sizeof(float));
    /* Real units cap their own render at 64 frames. The descriptor says 64,
     * so the host's block is split here rather than handed over whole. */
    {
      const unsigned long long started = now_us();
      g_unit.render(g_input, temp, n);
      {
        const unsigned long long took = now_us() - started;
        unsigned long worst =
            __atomic_load_n(&g_render_us_max, __ATOMIC_RELAXED);
        /* A maximum from the audio thread, read on the panel: the read-then-
         * store race can only lose a value, and losing one sample of a
         * diagnostic is better than a lock on this path. */
        if (took > worst) {
          __atomic_store_n(&g_render_us_max, (unsigned long)took,
                           __ATOMIC_RELAXED);
        }
      }
    }
    {
      float peak = 0.0f;
      for (i = 0; i < n * 2; i++) {
        const float s = temp[i];
        if (!(s == s) || (s > -DLSYNTH_SILENCE && s < DLSYNTH_SILENCE)) {
          temp[i] = 0.0f;
        } else {
          const float v = s < 0.0f ? -s : s;
          if (v > peak) peak = v;
        }
        out[off * 2 + i] += temp[i];
      }
      if (peak > 0.0f) {
        g_idle_blocks = 0;
      } else {
        g_idle_blocks++;
      }
      if (peak > 9.99f) peak = 9.99f;
      __atomic_store_n(&g_peak_pct, (int)(peak * 100.0f), __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&g_renders, 1, __ATOMIC_RELAXED);
  }
  __atomic_store_n(&g_audio_busy, 0, __ATOMIC_SEQ_CST);
}

static void dlsynth_midi_msg(void* port, const unsigned char* data, size_t n) {
  unsigned channel;

  (void)port;
  if (data == NULL || n < 2 || n > 3) return;
  if ((data[0] & 0x80) == 0 || (data[0] & 0xf0) == 0xf0) return;
  channel = (unsigned)(data[0] & 0x0f) + 1;
  if (channel != __atomic_load_n(&g_channel, __ATOMIC_ACQUIRE)) return;
  if (!__atomic_load_n(&g_audio_on, __ATOMIC_SEQ_CST) ||
      !__atomic_load_n(&g_unit.ready, __ATOMIC_SEQ_CST)) {
    return;
  }

  switch (data[0] & 0xf0) {
    case 0x90:
      if (n < 3) return;
      if (data[2] == 0) {
        push_event(EV_NOTE_OFF, data[1], 0);
      } else {
        push_event(EV_NOTE_ON, data[1], data[2]);
        __atomic_add_fetch(&g_notes, 1, __ATOMIC_RELAXED);
      }
      break;
    case 0x80:
      push_event(EV_NOTE_OFF, data[1], 0);
      break;
    case 0xe0:
      push_event(EV_PITCH, data[1], data[2]);
      break;
    case 0xd0:
      push_event(EV_PRESSURE, data[1], 0);
      break;
    case 0xa0:
      push_event(EV_AFTERTOUCH, data[1], data[2]);
      break;
    case 0xb0:
      /* All Sound Off and All Notes Off. The other controllers are not mapped:
       * a synth unit has parameters, not a fixed CC map, and guessing one
       * would make a knob on a controller change something unasked. */
      if (data[1] == 120 || data[1] == 123) push_event(EV_ALL_OFF, 0, 0);
      break;
    default:
      break;
  }
}

static int dlsynth_init(const borg_host* host, const char* module_path) {
  const char* dir;
  const char* chan;
  const char* wanted;
  int i;
  int wanted_found = 0;

  (void)module_path;
  if (host == NULL || host->log == NULL || host->path == NULL ||
      host->row == NULL) {
    return 1;
  }
  if (host->abi < 4) return 1;
  g_host = host;

  dir = getenv("LOGUE_BORG_DLSYNTH_DIR");
  if (dir == NULL || *dir == '\0') dir = host->path(BORG_PATH_UNITS);
  if (dir == NULL) dir = "/var/lib/drumlogued/userfs/Units/Synths";
  snprintf(g_dir, sizeof g_dir, "%.*s", DLSYNTH_DIR_MAX - 1, dir);

  chan = getenv("LOGUE_BORG_DLSYNTH_CHANNEL");
  if (chan != NULL) {
    int v = atoi(chan);
    if (v >= 1 && v <= 16) g_channel = (unsigned)v;
  }

  g_selected = -1;
  scan_units();

  wanted = getenv("LOGUE_BORG_DLSYNTH_UNIT");
  if (wanted != NULL && *wanted != '\0') {
    char stem[DLSYNTH_FILE_MAX];
    int found = 0;
    for (i = 0; i < g_count; i++) {
      stem_of(stem, sizeof stem, g_list[i].file);
      if (strcmp(g_list[i].file, wanted) == 0 ||
          strcmp(stem, wanted) == 0 ||
          strcmp(g_list[i].name, wanted) == 0) {
        g_selected = i;
        found = 1;
        break;
      }
    }
    if (found) {
      wanted_found = 1;
    } else {
      dlog("dlsynth: %s is not a synth unit in %s", wanted, g_dir);
    }
  }

  dlog("dlsynth: %d synth unit(s) in %s, channel %u", g_count, g_dir,
       g_channel);
  /* Do not start a unit just because the module was installed. The directory
   * may hold a sample-based unit, and this module supplies no sample banks;
   * such a unit can fault on its first render. A name in the environment is an
   * explicit choice, and MASTER is the same choice made on the panel. */
  if (g_count == 0) {
    set_state("NO SYNTH");
  } else if (wanted_found) {
    load_index(g_selected);
  } else {
    set_state("NO UNIT");
  }
  return 0;
}

static void dlsynth_page(void) {
  char value[16];
  const char* name = "NO UNIT";

  if (g_host == NULL || g_host->row == NULL) return;
  if (g_selected >= 0 && g_selected < g_count) name = g_list[g_selected].name;
  g_host->row(26, "UNIT:", name);
  snprintf(value, sizeof value, "%u", __atomic_load_n(&g_channel,
                                                      __ATOMIC_ACQUIRE));
  g_host->row(35, "CHANNEL:", value);
  snprintf(value, sizeof value, "%s %d%%", g_state,
           __atomic_load_n(&g_peak_pct, __ATOMIC_RELAXED));
  g_host->row(44, "STATE:", value);
  snprintf(value, sizeof value, "%lu/%lu",
           __atomic_load_n(&g_renders, __ATOMIC_RELAXED),
           __atomic_load_n(&g_render_us_max, __ATOMIC_RELAXED));
  g_host->row(53, "REND/US:", value);
}

static void dlsynth_page_enter(void) {
  scan_units();
}

static void dlsynth_page_leave(void) {
}

static void dlsynth_key(int index, int down) {
  if (index == 0x0f && down) load_index(g_selected);
}

static void dlsynth_encoder(int index, int delta) {
  if (index == 0) {
    if (g_count == 0 || delta == 0) return;
    g_selected = (g_selected + delta) % g_count;
    if (g_selected < 0) g_selected += g_count;
    return;
  }
  if (index == 1 && delta != 0) {
    int ch = (int)g_channel + delta;
    while (ch < 1) ch += 16;
    while (ch > 16) ch -= 16;
    __atomic_store_n(&g_channel, (unsigned)ch, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_unit.ready, __ATOMIC_ACQUIRE)) {
      push_event(EV_ALL_OFF, 0, 0);
    }
    /* Deliberately no log line. `log` is a file open/write/close on userfs
     * flash, and an encoder sends one step per detent: the write holds the
     * core for milliseconds on this board, the next 1.33 ms audio block misses
     * its deadline, and the click that produces was reported as the channel
     * change itself being broken. The page already shows CHANNEL: and there is
     * nothing here a log would add. */
  }
}

static const borg_module dlsynth_module = {
  BORG_MODULE_ABI,
  "DLSYNTH",
  "1.0.0",
  BORG_CAP_PAGE | BORG_CAP_INPUT | BORG_CAP_AUDIO | BORG_CAP_MIDI_EVENTS,
  dlsynth_init,
  dlsynth_page,
  NULL,                     /* midi_in: the page is the control surface */
  NULL,                     /* tick */
  dlsynth_page_enter,
  dlsynth_page_leave,
  dlsynth_key,
  dlsynth_encoder,
  dlsynth_audio,
  dlsynth_midi_msg,
  NULL                      /* midi_transform: dlsynth plays notes as received */
};

const borg_module* borg_module_entry(uint32_t host_abi) {
  if (host_abi < 4) return NULL;
  return &dlsynth_module;
}
