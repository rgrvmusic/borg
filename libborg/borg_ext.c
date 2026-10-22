/* The extensions the object carries once it is inside drumlogued.
 *
 * status   MUTE and MASTER together open a two-page readout on the panel.
 *          Keys pressed while it is open are consumed, so the daemon sees
 *          neither the chord nor the keys, and cannot paint over the page.
 *          PAGE up/down moves between pages; the same chord closes it, and
 *          the daemon's last screen is replayed.
 * midi     every byte through snd_rawmidi_read/write is counted. A module that
 *          asks for BORG_CAP_MIDI_TRANSFORM is offered each complete channel
 *          message before the daemon's unit, with writable data bytes; the
 *          note transpose and velocity curve are `transform.borgmodule`, not a
 *          setting of this object, because an environment variable is not
 *          something the instrument can reach once its daemon has started.
 *
 * No symbol is resolved from a constructor: that window kills this daemon.
 * Every interposer resolves its real function on the first call it serves.
 * No process and no thread is created; the hooks run on the daemon's own. */

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "borg_module.h"
#define BORG_UNIT_BUILD 1
#include "borg_unit.h"

extern char** environ;

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
#define MIDI_PORT_MAX 8
/* A command is at most 200 bytes plus a four-byte frame. The buffer is larger
 * so that an over-long message is cut where a reader can see it is over-long,
 * and the terminator is still put back on the end. */
#define MIDI_SYSEX_MAX 512
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

static unsigned long audio_calls, audio_errors;
static unsigned long menu_opens;

/* What the playback stream resolved to, and how many blocks the modules were
 * actually mixed into. On the drumlogue the stream is the codec's own shape --
 * S24_LE, eight channels -- and an entry point that is entered but mixes
 * nothing looks exactly like one that mixes silence: `audio_calls` counts the
 * daemon's writes either way. These two are what tell them apart on the STATUS
 * page, where there is no console to read. */
static int audio_pcm_format = -1;
static unsigned audio_pcm_channels;
static unsigned long audio_mixed;
/* Why the last block was passed through instead of mixed. The counters are
 * written on the audio thread and read on the panel thread, so they are
 * accessed atomically; the enum is the value in `pcm_skip`. */
enum {
  PCM_SKIP_NONE = 0,
  PCM_SKIP_NO_MODULE,   /* no module asked for audio */
  PCM_SKIP_UNRESOLVED,  /* the stream's format is not known yet */
  PCM_SKIP_BUSY,        /* another block holds the scratch buffer */
  PCM_SKIP_BIG          /* the block does not fit the scratch buffer */
};
static int pcm_skip;
static unsigned long pcm_skips, pcm_scratch_steals;
static int pcm_skip_logged;             /* panel thread only */
/* Where the audio thread's time went, published for the panel thread. A block
 * that is mixed and still misses the period is a load question, and the two
 * numbers that answer it are the whole block's cost and the module's part of
 * it: 64 frames is 1333 us on the drumlogue, so a block near that is the
 * cause and a block in tens of us is not. `pcm_get_slow` counts blocks that
 * took the lock because the stream was not resolved on the fast path -- that
 * path does allocation and ALSA calls, and one per block would be a far more
 * expensive bug than the synth. */
static unsigned long long pcm_mix_ns_last, pcm_mix_ns_max;
static unsigned long long pcm_mod_ns_last, pcm_mod_ns_max;
static unsigned long pcm_get_slow;
static int pcm_write_err;               /* last negative writei return */
static unsigned pcm_frames_last;        /* frames in the last mixed write */

/* The hook's own cost is not the question that decides an xrun. On this board
 * the daemon renders its own unit between two writes, and that block has to be
 * produced before the buffer runs dry: the deadline is the buffer's worth of
 * audio minus what was just written, and it is spent by the daemon's engine,
 * this hook and the library's write together. `pcm_gap_ns_*` is the time from
 * the previous write's return to the next hook entry, which is the daemon's own
 * per-block work; `pcm_write_ns_*` is the time inside the library call. Only
 * with all three is a mix time of 60 us either innocent or damning.
 *
 * `pcm_buffer_frames`/`pcm_period_frames` are the stream's real shape, read
 * back once from the handle the daemon writes: the log's old "667 us a 32-frame
 * write owns" was an assumption, and the observed writes are 64 frames, so the
 * slack has to come from the handle and not from a comment. */
static unsigned long long pcm_prev_ret_ns;      /* audio thread only */
static unsigned long long pcm_gap_ns_last, pcm_gap_ns_max;
static unsigned long long pcm_write_ns_last, pcm_write_ns_max;
/* The block that missed, captured on the audio thread at the failure. */
static unsigned long long pcm_err_gap_ns, pcm_err_mix_ns, pcm_err_mod_ns;
static unsigned pcm_buffer_frames, pcm_period_frames;

/* Wall time cannot tell a block that was expensive from one that was merely
 * scheduled late, and the call is lateness that matters: with a four-period
 * buffer an xrun means the audio thread did not run for 5.3 ms, while this
 * hook's whole cost is tens of microseconds. So the same block is also timed
 * with the calling thread's CPU clock. The difference between the two -- CPU
 * consumed per audio cycle -- separates an instrument whose core is genuinely
 * full from one whose audio thread is being descheduled, and the scheduling
 * decision below is taken from it rather than from a guess. */
static unsigned long long pcm_prev_in_wall, pcm_prev_in_cpu;
static unsigned long long pcm_prev_hook_cpu, pcm_prev_write_cpu;
static unsigned long long pcm_engine_cpu_ns, pcm_engine_cpu_max;
static unsigned long long pcm_hook_cpu_ns, pcm_hook_cpu_max;
static unsigned long long pcm_cycle_ns;
enum {
  PCM_RT_UNDECIDED = 0,
  PCM_RT_FIFO,      /* the audio thread was promoted and is the one that runs */
  PCM_RT_CFS,       /* the core is already full; leaving it alone is right */
  PCM_RT_REFUSED    /* no permission to change it */
};
static int pcm_rt_state;
static unsigned long long pcm_rt_cpu_us, pcm_rt_cycle_us;

/* What scheduling each of the two threads runs under, recorded once. A hook
 * whose own work is tens of microseconds cannot miss a 1333 us period by
 * itself; it misses when it is not the highest-priority thing on the core, and
 * whether that is true is a fact about the daemon's threads that no amount of
 * timing the hook will show. The panel thread is the one this object adds work
 * to, so its priority is the other half of the question. */
static int pcm_audio_policy = -1, pcm_audio_prio;
static int pcm_panel_policy = -1, pcm_panel_prio;

/* Where the audio thread is when the counters stop. A frozen MIXED/CALLS has
 * three causes that look identical from the counts alone: the daemon stopped
 * calling the hook (between blocks or gone), a module's render never returned,
 * or the library's own write never returned. The audio thread publishes which
 * one it is in and when it last entered the hook, so the panel can name it --
 * and the panel is the only report that survives a daemon too wedged to answer
 * MIDI, which is exactly the state that made this necessary. `pcm_stage_mod`
 * is the module whose `audio_render` is running. All three are written by the
 * audio thread and read by the panel thread, so atomics, relaxed: they are a
 * diagnostic, not a synchronisation. */
enum {
  PCM_STAGE_IDLE = 0,   /* not in the hook: between blocks */
  PCM_STAGE_HOOK,       /* copying, converting, or adding the mix */
  PCM_STAGE_MODULE,     /* inside a module's audio_render */
  PCM_STAGE_WRITE       /* inside the library's own write */
};
static int pcm_stage;
static int pcm_stage_mod;
static unsigned long long pcm_beat_ms;

static const char* pcm_stage_name(int stage) {
  switch (stage) {
    case PCM_STAGE_HOOK: return "host copy/mix";
    case PCM_STAGE_MODULE: return "module render";
    case PCM_STAGE_WRITE: return "driver write";
    default: return "no write entered";
  }
}

/* A scheduling policy as one word, for a log line. The numbers are the
 * kernel's and are not portable, which is why they are not printed. */
static const char* sched_name(int policy) {
  switch (policy) {
    case -1: return "?";
    case SCHED_FIFO: return "fifo";
    case SCHED_RR: return "rr";
    case SCHED_BATCH: return "batch";
    case SCHED_IDLE: return "idle";
    default: return "other";
  }
}

/* Defined with the audio path below; the STATUS page reports its numbers. */
static unsigned long long pcm_now_ms(void);

static const char* pcm_skip_name(int reason) {
  switch (reason) {
    case PCM_SKIP_NO_MODULE: return "no module asked for audio";
    case PCM_SKIP_UNRESOLVED: return "stream format not resolved";
    case PCM_SKIP_BUSY: return "scratch buffer busy";
    case PCM_SKIP_BIG: return "block larger than the scratch buffer";
    default: return "mixed";
  }
}

static char pcm_skip_letter(int reason) {
  switch (reason) {
    case PCM_SKIP_NO_MODULE: return 'n';
    case PCM_SKIP_UNRESOLVED: return 'u';
    case PCM_SKIP_BUSY: return 'b';
    case PCM_SKIP_BIG: return 's';
    default: return '-';
  }
}

/* Defined with the audio hook below; the STATUS page draws the format name. */
static const char* pcm_format_name(int format);

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

/* ------------------------------------------------------------ fault reporting
 *
 * `drumlogued` installs no SIGSEGV or SIGBUS handler, and bootarbiterd starts
 * it once with nothing to restart it. A bad pointer anywhere inside the daemon
 * -- including inside a unit it hosts -- therefore takes the whole instrument
 * down and leaves no line anywhere. That is indistinguishable from a hang or a
 * power problem, and it is exactly what a report like "it cracked, then the
 * machine died, no log" describes.
 *
 * This installs the handler. It writes the signal, the PC, LR, SP and the
 * faulting address and then re-raises with the default action, so the daemon
 * still dies the way it would have -- nothing is swallowed, nothing continues
 * in an unknown state. The object map is refreshed from the panel thread so the
 * address can be named: the daemon, a borg module, or the private copy dlsynth
 * made of a unit.
 *
 * This is the one diagnostic written to userfs, and the reason is the crash it
 * reports: a dead daemon forces a power cycle, and /tmp is tmpfs, so a line
 * kept there would be wiped along with the fault it describes. One line, only
 * when the process is already dying -- not a stream, and not a wear question. */
#define BORG_CRASH_LOG \
  "/var/lib/drumlogued/userfs/Programs/borg-crash.log"
#define CRASH_OBJECTS_MAX 16

typedef struct {
  unsigned long base;
  unsigned long end;
  char name[32];
} crash_object;

static crash_object crash_objects[CRASH_OBJECTS_MAX];
static int crash_object_count;

static void crash_object_note(unsigned long base, unsigned long end,
                              const char* name) {
  int i;

  if (base == 0 || crash_object_count >= CRASH_OBJECTS_MAX) return;
  for (i = 0; i < crash_object_count; i++) {
    if (crash_objects[i].base == base) return;
  }
  crash_objects[crash_object_count].base = base;
  crash_objects[crash_object_count].end = end;
  snprintf(crash_objects[crash_object_count].name,
           sizeof crash_objects[crash_object_count].name, "%s", name);
  __atomic_store_n(&crash_object_count, crash_object_count + 1,
                   __ATOMIC_RELEASE);
}

static int crash_phdr(struct dl_phdr_info* info, size_t size, void* data) {
  unsigned long end = 0;
  int i;
  const char* name;

  (void)size;
  (void)data;
  for (i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD) continue;
    if (info->dlpi_addr + ph->p_vaddr + ph->p_memsz > end) {
      end = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
    }
  }
  name = info->dlpi_name;
  if (name == NULL || name[0] == '\0') {
    name = "drumlogued";
  } else {
    const char* slash = strrchr(name, '/');
    if (slash != NULL && slash[1] != '\0') name = slash + 1;
  }
  crash_object_note(info->dlpi_addr, end, name);
  return 0;
}

/* Named from the panel thread at 2 Hz: cheap enough there, and never called
 * from the audio thread or from the handler, which must not take the loader
 * lock. */
static void crash_refresh(void) { dl_iterate_phdr(crash_phdr, NULL); }

static int crash_hex(char* out, unsigned long v) {
  static const char digits[] = "0123456789abcdef";
  char tmp[2 * sizeof(unsigned long)];
  int i = 0, n = 0;

  if (v == 0) {
    out[n++] = '0';
    return n;
  }
  while (v != 0) {
    tmp[i++] = digits[v & 0xf];
    v >>= 4;
  }
  while (i > 0) out[n++] = tmp[--i];
  return n;
}

static void crash_handler(int sig, siginfo_t* si, void* uc_) {
  static volatile sig_atomic_t entered;
  ucontext_t* uc = (ucontext_t*)uc_;
  unsigned long pc = 0, lr = 0, sp = 0;
  const char* pc_name = "?";
  const char* lr_name = "?";
  char line[192];
  int n = 0, i, count;
  int fd;

  /* A fault while reporting a fault: give up and let the default happen. */
  if (entered) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  entered = 1;

#if defined(__arm__)
  pc = (unsigned long)uc->uc_mcontext.arm_pc;
  lr = (unsigned long)uc->uc_mcontext.arm_lr;
  sp = (unsigned long)uc->uc_mcontext.arm_sp;
#else
  (void)uc;
#endif
  count = __atomic_load_n(&crash_object_count, __ATOMIC_ACQUIRE);
  for (i = 0; i < count; i++) {
    const crash_object* o = &crash_objects[i];
    if (pc >= o->base && pc < o->end) pc_name = o->name;
    if (lr >= o->base && lr < o->end) lr_name = o->name;
  }

  /* Hand-rolled formatting: snprintf is not async-signal-safe, and this runs
   * on whichever thread faulted -- usually the audio one. */
#define CRASH_LIT(s)                                                       \
  do {                                                                     \
    const char* s_ = (s);                                                  \
    while (*s_ != '\0' && n < (int)sizeof line - 1) line[n++] = *s_++;     \
  } while (0)
#define CRASH_HEX(v)                                                       \
  do {                                                                     \
    if (n < (int)sizeof line - 1) line[n++] = '0';                         \
    if (n < (int)sizeof line - 1) line[n++] = 'x';                         \
    n += crash_hex(line + n, (unsigned long)(v));                          \
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;                \
  } while (0)
#define CRASH_STR(s)                                                       \
  do {                                                                     \
    const char* s_ = (s);                                                  \
    while (*s_ != '\0' && n < (int)sizeof line - 2) line[n++] = *s_++;     \
  } while (0)

  CRASH_LIT("borg: signal ");
  if (sig >= 10) line[n++] = (char)('0' + (sig / 10) % 10);
  line[n++] = (char)('0' + sig % 10);
  CRASH_LIT(" pc=");
  CRASH_HEX(pc);
  CRASH_LIT(" in ");
  CRASH_STR(pc_name);
  CRASH_LIT(" lr=");
  CRASH_HEX(lr);
  CRASH_LIT(" in ");
  CRASH_STR(lr_name);
  CRASH_LIT(" sp=");
  CRASH_HEX(sp);
  CRASH_LIT(" addr=");
  CRASH_HEX(si != NULL ? (unsigned long)si->si_addr : 0);
#undef CRASH_LIT
#undef CRASH_HEX
#undef CRASH_STR
  line[n++] = '\n';

  fd = (int)syscall(SYS_openat, AT_FDCWD, BORG_CRASH_LOG,
                    O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    raw_write(fd, line, (size_t)n);
    syscall(SYS_close, fd);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

/* If the fault is a stack overflow, the handler cannot run on the stack that
 * just overflowed. Each thread that runs daemon or unit code gets an alternate
 * signal stack of its own: the panel thread and the audio thread, which are
 * the only two that can be inside a unit. Shared buffers would be wrong if
 * both faulted at once; two faults at once is not a state worth designing for,
 * but two threads are the real case, so each gets its own. */
static unsigned char crash_stack_audio[16384];
static unsigned char crash_stack_panel[16384];

static void crash_altstack(unsigned char* buf, size_t n) {
  stack_t ss;

  ss.ss_sp = buf;
  ss.ss_size = n;
  ss.ss_flags = 0;
  sigaltstack(&ss, NULL);
}

static void crash_install(void) {
  static const int signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
  struct sigaction sa;
  size_t i;

  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  /* The object map is refreshed from the panel thread, not here: this runs
   * during dynamic linking, and dl_iterate_phdr takes the loader lock. Address
   * names are a convenience; the handler still reports every address. */
  for (i = 0; i < sizeof signals / sizeof signals[0]; i++) {
    sigaction(signals[i], &sa, NULL);
  }
}

__attribute__((constructor)) static void borg_ext_init(void) {
  start_epoch = time(NULL);
  crash_install();
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
  if (which == BORG_PATH_UNITS) return "/var/lib/drumlogued/userfs/Units/Synths";
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

/* Defined with the MIDI hooks below; the host table is built here. */
static int host_midi_write(void* port, const void* data, size_t n);
static const char* host_port_name(void* port);

/* ABI 4 adds samplerate, which the audio hook fills in once it has seen the
 * daemon's playback stream. It is written before the first block is mixed and
 * never changed after, so a module may read it without an atomic. */
static borg_host host_services = {
  (uint32_t)sizeof(borg_host), BORG_MODULE_ABI,
  host_log, host_path, host_text, host_dark, host_row,
  host_midi_write, host_port_name, 0
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
  /* An ABI 1 module was built against the older entry and may refuse the new
   * number before returning its table. It is still a page and nothing else, so
   * it is offered the number it knows. */
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
  snprintf(set->name[slot], MODULE_FILE_MAX, "%s", name);
  set->n_modules = slot + 1;
  if ((m->capabilities & BORG_CAP_PAGE) != 0 && m->page_draw != NULL &&
      m->name != NULL) {
    set->n_pages++;
  }
  module_logf("loaded %s %s%s", m->name != NULL ? m->name : "?",
              m->version != NULL ? m->version : "",
              (m->abi >= 2 && (m->capabilities & BORG_CAP_MIDI) != 0)
                  ? " (MIDI)"
                  : "");
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

/* ------------------------------------------------------------ module calls
 *
 * One module call at a time. A MIDI input thread, the panel tick and the page
 * draw can otherwise be inside the same module together -- the first two on
 * different MIDI ports, the last on the panel thread -- and a module is a
 * small object with no thread story of its own. The MIDI path waits its turn,
 * because a command has to be answered in order. A tick and a page draw do
 * not wait: they are pulses, and a skipped pulse is one the next half second
 * supplies. Nothing here is held across a read or a write to a device. */

static pthread_mutex_t module_call_lock = PTHREAD_MUTEX_INITIALIZER;

static void modules_midi_in(void* port, const unsigned char* data, size_t n) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi < 2 || (m->capabilities & BORG_CAP_MIDI) == 0 ||
        m->midi_in == NULL) {
      continue;
    }
    m->midi_in(port, data, n);
  }
  pthread_mutex_unlock(&module_call_lock);
}

/* ABI 4. One complete channel message, serialized with the other module calls
 * because it is not an audio-path callback: the module is allowed to do here
 * what midi_in may do. Any note transform has already been applied by the time
 * it is offered, so a module hears the notes the daemon's own unit hears. */
static void modules_midi_msg(void* port, const unsigned char* data, size_t n) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi < 4 || (m->capabilities & BORG_CAP_MIDI_EVENTS) == 0 ||
        m->midi_msg == NULL) {
      continue;
    }
    m->midi_msg(port, data, n);
  }
  pthread_mutex_unlock(&module_call_lock);
}

/* ABI 5, and before midi_msg: the message is still the daemon's to play, so a
 * module may rewrite it. The lock is the same one midi_in and midi_msg take --
 * a transform module is offered the same host services they are, and the host
 * serializes all of them. */
static void modules_midi_transform(void* port, unsigned char* data, size_t n) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi < 5 || (m->capabilities & BORG_CAP_MIDI_TRANSFORM) == 0 ||
        m->midi_transform == NULL) {
      continue;
    }
    m->midi_transform(port, data, n);
  }
  pthread_mutex_unlock(&module_call_lock);
}

/* The tab name of the first transform module, or "NONE". The menu's MIDI page
 * has no setting of its own to show any more; what it can honestly report is
 * whether something is loaded that changes the notes. */
static const char* modules_transform_name(void) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return "NONE";
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi >= 5 && (m->capabilities & BORG_CAP_MIDI_TRANSFORM) != 0 &&
        m->midi_transform != NULL && m->name != NULL) {
      return m->name;
    }
  }
  return "NONE";
}

static void modules_tick(void) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  if (pthread_mutex_trylock(&module_call_lock) != 0) return;
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi < 2 || (m->capabilities & BORG_CAP_MIDI) == 0 ||
        m->tick == NULL) {
      continue;
    }
    m->tick();
  }
  pthread_mutex_unlock(&module_call_lock);
}

static void module_page_draw(const borg_module* m) {
  if (m == NULL || m->page_draw == NULL) return;
  if (pthread_mutex_trylock(&module_call_lock) != 0) return;
  m->page_draw();
  pthread_mutex_unlock(&module_call_lock);
}

/* An ABI 3 page that asks for input. The menu sends it the buttons it does not
 * keep for itself, and the encoders while the page is active, so a page can
 * scroll a list without the knob also editing the daemon's page underneath. */
static int module_takes_input(const borg_module* m) {
  return m != NULL && m->abi >= 3 &&
         (m->capabilities & BORG_CAP_INPUT) != 0;
}

static void module_page_enter(int page) {
  const borg_module* m = page_module(page);
  if (!module_takes_input(m) || m->page_enter == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  m->page_enter();
  pthread_mutex_unlock(&module_call_lock);
}

static void module_page_leave(int page) {
  const borg_module* m = page_module(page);
  if (!module_takes_input(m) || m->page_leave == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  m->page_leave();
  pthread_mutex_unlock(&module_call_lock);
}

static void modules_key(int page, int index, int down) {
  const borg_module* m = page_module(page);
  if (!module_takes_input(m) || m->key == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  m->key(index, down);
  pthread_mutex_unlock(&module_call_lock);
}

static void modules_encoder(int page, int index, int delta) {
  const borg_module* m = page_module(page);
  if (!module_takes_input(m) || m->encoder == NULL) return;
  pthread_mutex_lock(&module_call_lock);
  m->encoder(index, delta);
  pthread_mutex_unlock(&module_call_lock);
}

/* ABI 4 audio. Deliberately not serialized by module_call_lock: the audio
 * thread must never wait for a MIDI command or a page draw to finish. The
 * module set is immutable once published, so the walk needs no lock either.
 * `out` arrives zeroed and every module adds to it, which lets several audio
 * modules and the daemon's own mix share one buffer. */
static int modules_have_audio(void) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return 0;
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi >= 4 && (m->capabilities & BORG_CAP_AUDIO) != 0 &&
        m->audio_render != NULL) {
      return 1;
    }
  }
  return 0;
}

static void modules_audio(float* out, uint32_t frames) {
  const module_set* set = __atomic_load_n(&module_set_live, __ATOMIC_ACQUIRE);
  int i;

  if (set == NULL) return;
  for (i = 0; i < set->n_modules; i++) {
    const borg_module* m = set->mod[i];
    if (m->abi < 4 || (m->capabilities & BORG_CAP_AUDIO) == 0 ||
        m->audio_render == NULL) {
      continue;
    }
    /* Published before the call, so a render that never returns is named on
     * the panel instead of only showing as a frozen block counter. */
    __atomic_store_n(&pcm_stage_mod, i, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_stage, PCM_STAGE_MODULE, __ATOMIC_RELAXED);
    m->audio_render(out, frames);
  }
  __atomic_store_n(&pcm_stage, PCM_STAGE_HOOK, __ATOMIC_RELAXED);
}

/* Audio diagnostics go to /tmp, which is tmpfs: RAM. The instrument's userfs
 * is flash, and a line every few seconds is a line every few seconds for as
 * long as the daemon runs -- exactly the shape of write that wears a card out.
 * A diagnostic must not damage the instrument it is diagnosing. The file is
 * capped so a long session cannot eat the RAM either.
 *
 * The other half of the same concern: this is written from the panel thread at
 * most every ten seconds, never from the audio thread, and never per block. */
#define BORG_AUDIO_LOG "/tmp/logue-borg-audio.log"
#define BORG_AUDIO_LOG_MAX 65536UL

static unsigned long audio_log_bytes;

static void audio_logf(const char* fmt, ...) {
  char msg[256];
  char line[320];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  snprintf(line, sizeof line, "borg audio: %s\n", msg);
  if (audio_log_bytes > BORG_AUDIO_LOG_MAX) {
    const int fd =
        (int)syscall(SYS_openat, AT_FDCWD, BORG_AUDIO_LOG,
                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) syscall(SYS_close, fd);
    audio_log_bytes = 0;
  }
  raw_log(BORG_AUDIO_LOG, line);
  audio_log_bytes += strlen(line);
}

/* The audio path's health, written from the panel thread whether or not the
 * menu is open -- a run that crackles while the user plays has the menu
 * closed, and a report from that run used to carry no numbers at all.
 *
 * A ten-second line plus the first occurrence of each fault is the whole
 * budget. `last write` is the negative return of the most recent failed
 * write: -EPIPE is the deadline missed, anything else is a different story. */
static void audio_diag_log(void) {
  static unsigned long long diag_ms;
  static int logged_format = -2;
  static unsigned logged_channels;
  static unsigned long logged_errors;
  static int logged_budget;
  static int logged_slow;
  const int format = __atomic_load_n(&audio_pcm_format, __ATOMIC_RELAXED);
  const unsigned channels =
      __atomic_load_n(&audio_pcm_channels, __ATOMIC_RELAXED);
  const unsigned long mixed = __atomic_load_n(&audio_mixed, __ATOMIC_RELAXED);
  const unsigned long calls = __atomic_load_n(&audio_calls, __ATOMIC_RELAXED);
  const unsigned long errors =
      __atomic_load_n(&audio_errors, __ATOMIC_RELAXED);
  const unsigned long long max_us =
      __atomic_load_n(&pcm_mix_ns_max, __ATOMIC_RELAXED) / 1000;
  const unsigned long slow = __atomic_load_n(&pcm_get_slow, __ATOMIC_RELAXED);

  /* Once, on the panel thread: its scheduling next to the audio thread's, which
   * is recorded in pcm_mix. */
  {
    static int done;
    if (!done) {
      struct sched_param sp;
      int policy = SCHED_OTHER;

      done = 1;
      if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        __atomic_store_n(&pcm_panel_policy, policy, __ATOMIC_RELAXED);
        __atomic_store_n(&pcm_panel_prio, sp.sched_priority, __ATOMIC_RELAXED);
      }
    }
  }

  /* Keep the fault reporter's object map current, so an address in a unit that
   * dlsynth loaded after startup can still be named. Every two seconds, on the
   * panel thread; the loader lock is not touched on the audio thread. */
  {
    static unsigned long long refresh_ms;
    if (pcm_now_ms() - refresh_ms >= 2000) {
      refresh_ms = pcm_now_ms();
      crash_refresh();
    }
  }

  if (format >= 0 && channels > 0 &&
      (format != logged_format || channels != logged_channels)) {
    logged_format = format;
    logged_channels = channels;
    audio_logf("mixing %s %u ch, %lu mixed of %lu writes",
               pcm_format_name(format), channels, mixed, calls);
  }
  /* The first block that went over half the 667 us a 32-frame write owns. */
  if (!logged_budget && max_us >= 500) {
    logged_budget = 1;
    audio_logf("mix peaked at %llu us (module %llu us) per %u frames", max_us,
               __atomic_load_n(&pcm_mod_ns_max, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_frames_last, __ATOMIC_RELAXED));
  }
  /* The first few xruns, each with the block that missed. One line was not
   * enough: the first xrun of a run can be the load's own artefact, and the
   * ones that follow while the user plays are the question. Four lines and
   * then quiet -- a run with hundreds must not turn the log into load. The gap
   * is the daemon's own work for that block, so gap + mix is the time that had
   * to fit the buffer, and a mix of 60 us next to a gap of 1200 us says the
   * module was not the reason. */
  if (errors != logged_errors) {
    static unsigned logged_xruns;

    logged_errors = errors;
    if (logged_xruns < 4) {
      logged_xruns++;
      audio_logf("xrun %lu: write returned %d; gap %llu us + mix %llu us "
                 "(module %llu us) had to fit a %u-frame buffer; previous "
                 "cycle cpu engine %llu us of %llu us; %lu mixed/%lu writes",
                 errors, __atomic_load_n(&pcm_write_err, __ATOMIC_RELAXED),
                 __atomic_load_n(&pcm_err_gap_ns, __ATOMIC_RELAXED) / 1000,
                 __atomic_load_n(&pcm_err_mix_ns, __ATOMIC_RELAXED) / 1000,
                 __atomic_load_n(&pcm_err_mod_ns, __ATOMIC_RELAXED) / 1000,
                 __atomic_load_n(&pcm_buffer_frames, __ATOMIC_RELAXED),
                 __atomic_load_n(&pcm_engine_cpu_ns, __ATOMIC_RELAXED) / 1000,
                 __atomic_load_n(&pcm_cycle_ns, __ATOMIC_RELAXED) / 1000, mixed,
                 calls);
    }
  }
  /* A retried parameter read means the shape came from the fallback, which is
   * slower and is the path that once lost the stream entirely. */
  if (!logged_slow && slow != 0) {
    logged_slow = 1;
    audio_logf("%lu slow parameter gets (fallback path)", slow);
  }
  /* A stalled audio thread is the one failure that has no other report: the
   * daemon can be wedged inside a unit's render or inside the library's own
   * write, and the read that would fetch this file is on one of the same
   * threads. One line per episode, and the same stage on the STATUS page while
   * it lasts, which is the report that survives a wedged daemon. */
  {
    static int stalled_logged;
    const unsigned long long beat =
        __atomic_load_n(&pcm_beat_ms, __ATOMIC_RELAXED);
    const unsigned long long now = pcm_now_ms();
    const int stalled = calls > 0 && beat != 0 && now - beat > 2000;

    if (!stalled) {
      stalled_logged = 0;
    } else if (!stalled_logged) {
      const int stage = __atomic_load_n(&pcm_stage, __ATOMIC_RELAXED);
      const int skip = __atomic_load_n(&pcm_skip, __ATOMIC_RELAXED);
      stalled_logged = 1;
      audio_logf("audio stalled for %llu ms in %s (stage %d, module %d); "
                 "%lu mixed/%lu writes, last write %d, %lu xruns, %lu skips, "
                 "last skip %s",
                 now - beat, pcm_stage_name(stage), stage,
                 __atomic_load_n(&pcm_stage_mod, __ATOMIC_RELAXED), mixed,
                 calls, __atomic_load_n(&pcm_write_err, __ATOMIC_RELAXED),
                 errors, __atomic_load_n(&pcm_skips, __ATOMIC_RELAXED),
                 skip != PCM_SKIP_NONE ? pcm_skip_name(skip) : "none");
    }
  }

  /* The one-time scheduling decision, with the sample it was taken from. A
   * decision without its numbers cannot be argued with, and this one changes
   * how the daemon's own thread is scheduled. */
  {
    static int logged;
    const int rt = __atomic_load_n(&pcm_rt_state, __ATOMIC_RELAXED);

    if (rt != PCM_RT_UNDECIDED && !logged) {
      logged = 1;
      audio_logf("scheduling: audio thread %s; it used %llu us of a %llu us "
                 "cycle over the first 256",
                 rt == PCM_RT_FIFO
                     ? "promoted to fifo 1"
                     : (rt == PCM_RT_REFUSED ? "fifo refused"
                                             : "left in cfs"),
                 __atomic_load_n(&pcm_rt_cpu_us, __ATOMIC_RELAXED),
                 __atomic_load_n(&pcm_rt_cycle_us, __ATOMIC_RELAXED));
    }
  }
  /* The periodic line only while audio is actually moving: a daemon with no
   * playback is not a daemon whose mix time anyone needs, and ten seconds of
   * nothing is still a write. */
  {
    static unsigned long last_calls;
    if (calls == last_calls) return;
    last_calls = calls;
  }
  if (diag_ms != 0 && pcm_now_ms() - diag_ms < 10000) return;
  diag_ms = pcm_now_ms();
  {
    static unsigned long last_errors;
    const unsigned long added = errors - last_errors;

    last_errors = errors;
    audio_logf("mix %llu us (module %llu us), gap %llu us, write %llu us max "
               "%llu/%llu, per %u frames of %u/%u (buffer/period); cpu hook "
               "%llu us, engine %llu us (max %llu) of a %llu us cycle; "
               "%lu mixed/%lu writes, %lu xruns (+%lu), last write %d, "
               "%lu slow gets; audio %s/%d, panel %s/%d",
               __atomic_load_n(&pcm_mix_ns_last, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_mod_ns_last, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_gap_ns_last, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_write_ns_last, __ATOMIC_RELAXED) / 1000,
               max_us,
               __atomic_load_n(&pcm_write_ns_max, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_frames_last, __ATOMIC_RELAXED),
               __atomic_load_n(&pcm_buffer_frames, __ATOMIC_RELAXED),
               __atomic_load_n(&pcm_period_frames, __ATOMIC_RELAXED),
               __atomic_load_n(&pcm_hook_cpu_ns, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_engine_cpu_ns, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_engine_cpu_max, __ATOMIC_RELAXED) / 1000,
               __atomic_load_n(&pcm_cycle_ns, __ATOMIC_RELAXED) / 1000, mixed,
               calls, errors, added,
               __atomic_load_n(&pcm_write_err, __ATOMIC_RELAXED), slow,
               sched_name(__atomic_load_n(&pcm_audio_policy, __ATOMIC_RELAXED)),
               __atomic_load_n(&pcm_audio_prio, __ATOMIC_RELAXED),
               sched_name(__atomic_load_n(&pcm_panel_policy, __ATOMIC_RELAXED)),
               __atomic_load_n(&pcm_panel_prio, __ATOMIC_RELAXED));
  }
}

static void menu_draw(void) {
  unsigned char fill[12];
  char value[24];
  char tab[BORG_UNIT_TAB_MAX];
  const int pages = page_count();

  if (panel_out_fd < 0) return;
  /* A unit can suspend while its page is open, which takes the page with it. */
  if (pages > 0 && menu_page >= pages) {
    module_page_leave(menu_page);
    menu_page = pages - 1;
  }

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
    {
      const unsigned long mixed =
          __atomic_load_n(&audio_mixed, __ATOMIC_RELAXED);
      const unsigned long calls =
          __atomic_load_n(&audio_calls, __ATOMIC_RELAXED);
      const int format =
          __atomic_load_n(&audio_pcm_format, __ATOMIC_RELAXED);
      const unsigned channels =
          __atomic_load_n(&audio_pcm_channels, __ATOMIC_RELAXED);
      const int skip = __atomic_load_n(&pcm_skip, __ATOMIC_RELAXED);
      const unsigned long long beat =
          __atomic_load_n(&pcm_beat_ms, __ATOMIC_RELAXED);
      const unsigned long long now = pcm_now_ms();
      /* Two seconds without a block is not a slow block: the longest one this
       * instrument can be asked for is 1333 us. Anything past that is a thread
       * that is not coming back, and the stage says which one. This is the
       * only report available when the same wedge takes the MIDI thread with
       * it, which is exactly what happened on the board. */
      const int stalled = calls > 0 && beat != 0 && now - beat > 2000;
      const int stage = __atomic_load_n(&pcm_stage, __ATOMIC_RELAXED);

      /* A block that is entered but not mixed is the failure this page exists
       * to make visible: the call counter rises, the modules hear MIDI, and
       * nothing is rendered. The reason is logged once per change, on the
       * panel thread, so a report from the instrument says which one it was
       * instead of only that there was no sound. */
      if (calls > 0 && mixed == 0 && skip != pcm_skip_logged) {
        pcm_skip_logged = skip;
        module_logf("audio: %lu write(s), none mixed: %s (format %d, %u ch, "
                    "%lu skipped, %lu steals)",
                    calls, pcm_skip_name(skip), format, channels,
                    __atomic_load_n(&pcm_skips, __ATOMIC_RELAXED),
                    __atomic_load_n(&pcm_scratch_steals, __ATOMIC_RELAXED));
      }
      /* One letter is enough to name the reason in the row, and the log line
       * above has the words. `x` is the one case pcm_mix cannot name: neither
       * write entry point has been entered at all, so nothing was passed
       * through and no reason was recorded -- the daemon's playback is going
       * somewhere this object does not hook. */
      if (mixed == 0 && calls == 0) {
        snprintf(value, sizeof value, "0/0 x");
      } else if (mixed == 0 && skip != PCM_SKIP_NONE) {
        snprintf(value, sizeof value, "%lu/%lu %c", mixed, calls,
                 pcm_skip_letter(skip));
      } else {
        snprintf(value, sizeof value, "%lu/%lu", mixed, calls);
      }
      menu_row(44, "MIXED/CALLS:", value);

      if (stalled) {
        /* The stage number is the whole point: 0 means no write has been
         * entered since the daemon stopped, 1 is this hook, 2 is a module's
         * render (named by M<n>), 3 is the library's own write. The words are
         * in /tmp/logue-borg-audio.log, which a wedged daemon cannot serve. */
        if (stage == PCM_STAGE_MODULE) {
          snprintf(value, sizeof value, "S%d M%d", stage,
                   __atomic_load_n(&pcm_stage_mod, __ATOMIC_RELAXED));
        } else {
          snprintf(value, sizeof value, "S%d", stage);
        }
        menu_row(53, "STALL:", value);
      } else if (pcm_format_name(format)[0] == '?') {
        /* An unknown format shows its numeric id, so a stream this code
         * refuses can be named from the panel without a console. */
        snprintf(value, sizeof value, "F%d %uC/%lu", format, channels,
                 __atomic_load_n(&audio_errors, __ATOMIC_RELAXED));
        menu_row(53, "PCM/XRUNS:", value);
      } else {
        snprintf(value, sizeof value, "%s %uC/%lu", pcm_format_name(format),
                 channels,
                 __atomic_load_n(&audio_errors, __ATOMIC_RELAXED));
        menu_row(53, "PCM/XRUNS:", value);
      }
    }
  } else if (menu_page == 1) {
    snprintf(value, sizeof value, "%lu / %lu B", midi_in_bytes, midi_out_bytes);
    menu_row(26, "MIDI I/O:", value);
    snprintf(value, sizeof value, "%lu / %lu", midi_in_msgs, midi_notes);
    menu_row(35, "MSGS/NOTES:", value);
    snprintf(value, sizeof value, "%02lX %02lX %02lX", midi_last_status,
             midi_last_note, midi_last_vel);
    menu_row(44, "LAST NOTE:", value);
    /* The transform is a module now, so the page says whether one is loaded
     * rather than showing settings this object no longer owns. */
    menu_row(53, "XFORM:", modules_transform_name());
  } else if (menu_page < module_page_base()) {
    const borg_unit_desc* u = page_unit(menu_page);
    if (u != NULL && u->draw != NULL) u->draw(&unit_panel_services, u->user);
  } else {
    module_page_draw(page_module(menu_page));
  }
}

/* Redraw only the page's content area, leaving the title and tab bar alone.
 * Called from the panel thread on the panel's own half-second report, so a
 * unit's or a module's page follows playback while the menu is held open. A
 * module that is busy with a MIDI callback is left alone for this report: the
 * clear must not happen before the draw is known to be possible. */
static void page_redraw(void) {
  unsigned char fill[12];
  const int module_base = module_page_base();

  if (panel_out_fd < 0 || !menu_open) return;
  if (menu_page < UNIT_PAGE_BASE) return;
  if (menu_page >= module_base) {
    const borg_module* m = page_module(menu_page);
    if (m == NULL || m->page_draw == NULL) return;
    if (pthread_mutex_trylock(&module_call_lock) != 0) return;
    raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 24, 256, 40));
    m->page_draw();
    pthread_mutex_unlock(&module_call_lock);
    return;
  }
  {
    const borg_unit_desc* u = page_unit(menu_page);
    if (u == NULL || u->draw == NULL) return;
    raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 24, 256, 40));
    u->draw(&unit_panel_services, u->user);
  }
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
  /* The first page became the active one without a PAGE key, so its enter
   * edge is owed here rather than only in the paging code. */
  module_page_enter(menu_page);
}

static void menu_hide(int fd) {
  static const unsigned char seq_mode[4] = {0x85, 0x04, 0x04, 0x01};
  unsigned char fill[12];
  module_page_leave(menu_page);
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

  /* First panel read: this thread gets its own fault stack. Units are loaded
   * and torn down here, so this is the other thread that can be inside one. */
  {
    static int alt_installed;
    if (!alt_installed) {
      alt_installed = 1;
      crash_altstack(crash_stack_panel, sizeof crash_stack_panel);
    }
  }
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

      if (!chord && menu_open) {
        const int pages = page_count();
        if (down && key == KEY_PAGE_DOWN) {
          module_page_leave(menu_page);
          menu_page = (menu_page + 1) % pages;
          module_page_enter(menu_page);
          menu_draw();
        } else if (down && key == KEY_PAGE_UP) {
          module_page_leave(menu_page);
          menu_page = (menu_page + pages - 1) % pages;
          module_page_enter(menu_page);
          menu_draw();
        } else if (key != KEY_PAGE_UP && key != KEY_PAGE_DOWN) {
          /* The menu keeps its own keys. Everything else belongs to the page
           * when the page asks for input, including the release, so a module
           * can hold a key if it needs to. */
          modules_key(menu_page, key, down ? 1 : 0);
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
    } else if (buf[in] == 0x81) {
      /* An endless encoder, three bytes. On a page that asks for input the
       * menu takes it; elsewhere it is the daemon's, as before. */
      const int idx = buf[in + 1];
      const int raw = buf[in + 2];
      const int delta = (raw & 0x40) ? raw - 128 : raw;
      if (menu_open && module_takes_input(page_module(menu_page))) {
        modules_encoder(menu_page, idx, delta);
      } else {
        memcpy(out + o, buf + in, 3);
        o += 3;
      }
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

  /* The panel reports at 2 Hz even when no key moves. The tick does a module's
   * waiting work first, so the page drawn below is the state after it, and the
   * page redraw keeps a unit or module page following playback. Both run on the
   * panel thread. */
  modules_tick();
  /* Outside the menu on purpose: the audio path is observed while the user is
   * playing, which is when the menu is closed. */
  audio_diag_log();
  if (menu_open) page_redraw();

  if (o <= cap) {
    memcpy(buf, out, o);
    return o;
  }
  return got;
}

/* ------------------------------------------------------------- re-exec
 *
 * The unit's switch, made available to a module. A session switch has to
 * happen in place: the daemon opens its userfs once, and only a new main will
 * read another. The caller has already mounted the chosen tree where the
 * daemon expects it. This returns only when execve itself failed, because on
 * success it does not return. */

__attribute__((visibility("default"))) int borg_reexec(void) {
  static char cmdline[4096];
  static char* argv[64];
  static char* envp[192];
  static char preload[1024];
  const char* saved;
  char** e;
  size_t off;
  int fd, n, argc = 0, envc = 0;

  saved = getenv("LOGUE_BORG_PRELOAD");
  if (saved == NULL || saved[0] == '\0') return -ENOENT;

  fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline",
                    O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0) return -errno;
  n = (int)syscall(SYS_read, fd, cmdline, sizeof cmdline - 1);
  syscall(SYS_close, fd);
  if (n <= 0) return -EIO;
  cmdline[n] = '\0';

  off = 0;
  while (off < (size_t)n && argc < (int)(sizeof argv / sizeof argv[0]) - 1) {
    argv[argc++] = cmdline + off;
    off += strlen(cmdline + off) + 1;
  }
  argv[argc] = NULL;

  snprintf(preload, sizeof preload, "LD_PRELOAD=%s", saved);
  for (e = environ;
       *e != NULL && envc < (int)(sizeof envp / sizeof envp[0]) - 2; ++e) {
    if (strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_ACTIVE=", 18) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_PID=", 15) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_PRELOAD=", 19) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_SHIM=", 16) == 0) continue;
    envp[envc++] = *e;
  }
  envp[envc++] = preload;
  envp[envc] = NULL;

  /* execve keeps the calling thread's scheduling state. A switch can be asked
   * for from the panel thread or a MIDI thread; neither should carry a
   * realtime class or a lone CPU into the new daemon. */
  {
    struct sched_param param;
    cpu_set_t all;
    sigset_t none;
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int i;

    memset(&param, 0, sizeof param);
    if (sched_getscheduler(0) != SCHED_OTHER) {
      sched_setscheduler(0, SCHED_OTHER, &param);
    }
    setpriority(PRIO_PROCESS, 0, 0);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    CPU_ZERO(&all);
    if (cpus > 0 && cpus <= CPU_SETSIZE) {
      for (i = 0; i < (int)cpus; ++i) CPU_SET(i, &all);
      sched_setaffinity(0, sizeof all, &all);
    }
  }

  fflush(NULL);
  for (fd = 3; fd < 1024; ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }

  execve("/proc/self/exe", argv, envp);
  return -errno;
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

ssize_t snd_rawmidi_write(snd_rawmidi_t* rm, const void* buf, size_t n);

/* The daemon opens each port with one call that hands back an input and an
 * output handle. A module is given the input handle, and a reply is written to
 * the output that was returned beside it, so a command that arrives on the DIN
 * port is answered on the DIN port. The name is the one the daemon opened with,
 * which is what the module shows on its page. A closed port keeps its row and
 * the row is reused by the next open, so a handle stays valid for the lifetime
 * of the module even if the daemon reopens a device. */
typedef struct midi_port {
  snd_rawmidi_t* input;
  snd_rawmidi_t* output;
  char name[128];
} midi_port;

static midi_port midi_ports[MIDI_PORT_MAX];
static int n_midi_ports;
static pthread_mutex_t midi_ports_lock = PTHREAD_MUTEX_INITIALIZER;
/* One SysEx message is assembled at a time and handed to the modules whole.
 * The daemon reads its ports on one thread each, so the accumulator and the
 * note accounting below are taken under this lock. */
static pthread_mutex_t midi_account_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char midi_sysex_buf[MIDI_SYSEX_MAX];
static size_t midi_sysex_len;
/* The port a message in progress arrived on. A byte on another port abandons
 * the half-received message rather than mixing two ports into one. */
static void* midi_sysex_port;

static unsigned char midi_status;
static unsigned midi_need;
static unsigned midi_have;
/* The data bytes of the message in progress, kept across reads so that a
 * message split by the daemon's read size can still be offered whole to an
 * ABI 4 module. */
static unsigned char midi_data[2];
static int midi_sysex;
static int midi_first_here;
static size_t midi_first;

/* Called from snd_rawmidi_open, under no other lock. A handle that is already
 * known is updated rather than added, because the daemon may open the same
 * device twice while an old pair is still being torn down. */
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
        midi_ports[i].output == NULL)
      free_row = i;
  }
  if (free_row < 0 && n_midi_ports < MIDI_PORT_MAX) {
    free_row = n_midi_ports++;
  }
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
  if (real == NULL) real = (int (*)(snd_rawmidi_t**, snd_rawmidi_t**,
                                    const char*, int))
      dlsym(RTLD_NEXT, "snd_rawmidi_open");
  if (real == NULL) {
    errno = ENOSYS;
    return -ENOSYS;
  }
  {
    const int rc = real(in_rmidi, out_rmidi, name, mode);
    if (rc >= 0 && in_rmidi != NULL && *in_rmidi != NULL) {
      snd_rawmidi_t* out = (out_rmidi != NULL) ? *out_rmidi : NULL;
      midi_port_register(*in_rmidi, out, name);
    }
    return rc;
  }
}

int snd_rawmidi_close(snd_rawmidi_t* rm) {
  static int (*real)(snd_rawmidi_t*);
  int i;
  if (real == NULL)
    real = (int (*)(snd_rawmidi_t*)) dlsym(RTLD_NEXT, "snd_rawmidi_close");
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
   * EAGAIN. A reply is small and the buffer drains at the port's own rate, so a
   * wait is enough; 500 ms covers a full 200-byte chunk on a 31250-baud DIN
   * socket, and the bound keeps a stuck port from holding up the daemon's MIDI
   * thread for longer than that. */
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

static void midi_sysex_begin(void* port) {
  midi_sysex_len = 1;
  midi_sysex_buf[0] = 0xf0;
  midi_sysex_port = port;
}

static void midi_sysex_put(unsigned char b) {
  if (midi_sysex_len < MIDI_SYSEX_MAX)
    midi_sysex_buf[midi_sysex_len++] = b;
}

/* The terminator is always on the end, even when the message did not fit, so
 * that a reader refuses the whole message rather than waiting for an end that
 * is not coming. */
static void midi_sysex_end(void* port) {
  if (midi_sysex_len < MIDI_SYSEX_MAX) {
    midi_sysex_buf[midi_sysex_len++] = 0xf7;
  } else {
    midi_sysex_buf[MIDI_SYSEX_MAX - 1] = 0xf7;
    midi_sysex_len = MIDI_SYSEX_MAX;
  }
  modules_midi_in(port, midi_sysex_buf, midi_sysex_len);
  midi_sysex_len = 0;
  midi_sysex_port = NULL;
}

/* Counts and logs a note the daemon will play. The bytes are the ones that
 * reached the daemon, after any transform module had them. */
static void midi_note(unsigned char status, unsigned char note,
                      unsigned char vel) {
  midi_notes++;
  midi_last_status = status;
  midi_last_note = (unsigned long)note;
  midi_last_vel = (unsigned long)vel;

  {
    char line[64];
    snprintf(line, sizeof line, "note %02x %02x %02x\n", status, note, vel);
    raw_log(LOGUE_BORG_MIDI_LOG, line);
  }
}

/* A complete channel message on its way to the daemon's unit. A transform
 * module may rewrite the data bytes; what the daemon reads out of `buf` and
 * what every MIDI_EVENTS module is offered is the result, so the two cannot
 * disagree about a note that was in this read. System messages are not offered
 * here; SysEx goes to midi_in instead, and realtime bytes have no state.
 *
 * A message whose first data byte arrived in an earlier read cannot be
 * rewritten there -- that buffer has been handed back -- so only the copy the
 * modules see carries the transform for it. That is the one case where the
 * daemon plays what the wire sent. */
static void midi_channel_msg(void* port, unsigned char* buf, size_t n,
                             size_t first, int first_here) {
  const unsigned char status = midi_status;
  const size_t len = midi_need + 1;
  unsigned char msg[3];

  if ((status & 0x80) == 0 || (status & 0xf0) == 0xf0) return;
  if (midi_need < 1 || midi_need > 2) return;

  msg[0] = status;
  msg[1] = midi_data[0];
  msg[2] = midi_data[1];
  modules_midi_transform(port, msg, len);
  midi_data[0] = msg[1];
  midi_data[1] = msg[2];

  if (first_here && ((status & 0xf0) == 0x90 || (status & 0xf0) == 0x80) &&
      first + 1 < n) {
    buf[first] = msg[1];
    buf[first + 1] = msg[2];
    midi_note(status, msg[1], msg[2]);
  }
  modules_midi_msg(port, msg, len);
}

static void midi_account(unsigned char* buf, size_t n, void* port) {
  size_t i;

  if (midi_have > 0) midi_first_here = 0;

  for (i = 0; i < n; i++) {
    const unsigned char b = buf[i];

    if (b >= 0xf8) continue;             /* realtime, no state */
    if (midi_sysex) {
      if (midi_sysex_port != port) {
        /* Another port spoke. Drop the partial message and read this byte as
         * ordinary MIDI. */
        midi_sysex = 0;
        midi_sysex_len = 0;
        midi_sysex_port = NULL;
      } else if (b == 0xf7) {
        midi_sysex = 0;
        midi_sysex_end(port);
        continue;
      } else if (b < 0x80) {
        midi_sysex_put(b);
        continue;
      } else {
        /* Any other status byte ends the message without a terminator. It is
         * handled below as the start of what follows. */
        midi_sysex = 0;
        midi_sysex_len = 0;
        midi_sysex_port = NULL;
      }
    }
    if (b == 0xf0) {
      midi_sysex = 1;
      midi_sysex_begin(port);
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
      midi_data[0] = midi_data[1] = 0;
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
    if (midi_have < 2) midi_data[midi_have] = b;
    midi_have++;

    if (midi_have == midi_need) {
      midi_channel_msg(port, buf, n, midi_first, midi_first_here);
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
      pthread_mutex_lock(&midi_account_lock);
      midi_in_bytes += (unsigned long)got;
      midi_account((unsigned char*)buf, (size_t)got, rm);
      pthread_mutex_unlock(&midi_account_lock);
      /* A read is a pulse for a module that is waiting on something slow, such
       * as the kernel's helper, so that its answer is picked up promptly. */
      modules_tick();
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

/* ----------------------------------------------------------------- the audio
 *
 * ABI 4 lets a module add to the daemon's playback. The daemon hands ALSA a
 * buffer and this hook mixes the modules' float stereo contribution into it
 * just before the real call. Both entry points the daemon uses are covered:
 * writei and mmap_writei take the same arguments from here.
 *
 * Nothing on this path may block, allocate in a loop, or take a lock that the
 * panel or MIDI thread can hold. The module set is immutable and lock-free.
 * The stream's shape is recorded where the daemon installs it
 * (snd_pcm_hw_params), so it is known before the first block; the slower
 * snd_pcm_hw_params_current path is a retried fallback for a stream configured
 * before this object was preloaded. A read that fails is never remembered: a
 * stream that could not be resolved once must not stay unresolved for the rest
 * of the run, which is how the mix went silent while the call counter rose.
 * The mix is built in one static scratch buffer, guarded by an atomic so that a
 * second stream (or a re-entrant call) skips the extra synth rather than
 * corrupting the first; an owner that has held it for a second is gone and the
 * block is taken over, so a lost flag cannot silence the module for good.
 *
 * A format this code cannot convert is skipped in silence rather than guessed
 * at: converting an S24 block as if it were S16 would not be a quiet failure,
 * it would be noise. S16_LE, S24_LE, S32_LE and FLOAT_LE are what libasound
 * uses for a daemon's output; S24_LE is the one the drumlogue itself uses.
 *
 * That last point is why this file carries S24 at all. The daemon opens the
 * pcm3168a codec as `hw:0,0` at the codec's own shape: S24_LE, eight channels,
 * 48 kHz, MMAP_INTERLEAVED, 64-frame periods. Until S24 was handled here, every
 * block was skipped -- `snd_pcm_writei` was still entered, so the STATUS page's
 * call counter kept rising, while not one module render was ever asked for.
 * The one measurement behind the eight-channel shape is the mk2 daemon's
 * output tap, where all eight channels carry the same programme material; the
 * drumlogue's daemon writes the same shape, and a module's stereo pair is added
 * to every stereo pair it finds, so the mix is heard whichever pair the codec's
 * routing puts on the jack. */

typedef struct _snd_pcm snd_pcm_t;

#define PCM_INFO_MAX 4
/* The daemon opens the codec with a 256-frame buffer; 2048 is headroom for a
 * larger period without carrying a quarter of a megabyte of BSS in a daemon
 * that has little of it to spare. A block bigger than this is passed through
 * unmixed rather than truncated. */
#define PCM_SCRATCH_FRAMES 2048
#define AUDIO_CHUNK 64

/* The codec's eight outputs are the widest stream this hook has to hold; one
 * scratch buffer sized for that is what makes the copy cost one memcpy. */
#define PCM_MAX_CHANNELS 8

/* snd_pcm_format_t values, from alsa/pcm.h. Only these four are converted. */
#define PCM_FORMAT_S16_LE 2
#define PCM_FORMAT_S24_LE 6         /* 24 significant bits in a 4-byte slot */
#define PCM_FORMAT_S32_LE 10
#define PCM_FORMAT_FLOAT_LE 14

/* S24_LE's full scale is 2^23 - the sample occupies the low three bytes. */
#define PCM_S24_FULL_SCALE 8388608

static const char* pcm_format_name(int format) {
  switch (format) {
    case PCM_FORMAT_S16_LE: return "S16";
    case PCM_FORMAT_S24_LE: return "S24";
    case PCM_FORMAT_S32_LE: return "S32";
    case PCM_FORMAT_FLOAT_LE: return "FLT";
    case -1: return "NONE";
    default: return "?";
  }
}

typedef struct pcm_info {
  snd_pcm_t* pcm;
  int resolved;
  int format;
  unsigned channels;
  unsigned rate;
  unsigned bytes;
  unsigned long seq;      /* allocation order; the oldest slot is evicted */
  unsigned attempts;      /* failed fallback reads, for the retry backoff */
} pcm_info;

static pcm_info pcm_infos[PCM_INFO_MAX];
static pthread_mutex_t pcm_info_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long pcm_info_seq;          /* guarded by pcm_info_lock */
static unsigned char pcm_scratch[PCM_SCRATCH_FRAMES * PCM_MAX_CHANNELS * 4]
    __attribute__((aligned(8)));
static int pcm_scratch_busy;
static unsigned long long pcm_scratch_stamp;    /* ms, when busy was taken */

/* The getters are not declared here: libborg does not include ALSA's headers,
 * because the unit container has no reason to carry them. They are resolved
 * from the library the daemon already linked. */
typedef int (*hw_malloc_fn)(void**);
typedef int (*hw_current_fn)(snd_pcm_t*, void*);
typedef int (*hw_format_fn)(const void*, int*);
typedef int (*hw_channels_fn)(const void*, unsigned*);
typedef int (*hw_rate_fn)(const void*, unsigned*, int*);
typedef int (*hw_free_fn)(void*);

static struct {
  hw_malloc_fn malloc;
  hw_current_fn current;
  hw_format_fn format;
  hw_channels_fn channels;
  hw_rate_fn rate;
  hw_free_fn free;
} pcm_api;

/* Resolve the real libasound function. RTLD_NEXT is the normal answer and is
 * known to work here -- the write path binds through it, which is how a call
 * count rises at all. The handle is asked as a fallback for a link map where
 * libasound does not follow this object (a preload that is not first, or a
 * daemon that loads ALSA itself, where a pthread_once that ran before the
 * library was mapped would otherwise leave this table NULL for the run).
 * Getting this wrong in the other direction is worse: an interposer that
 * cannot find the real function reports ENOSYS and breaks the daemon's audio
 * setup instead of merely failing to record its shape. */
static void* pcm_real(const char* name) {
  void* p = dlsym(RTLD_NEXT, name);

  if (p == NULL) {
    static void* lib;                 /* never closed: the daemon owns it */
    if (lib == NULL) lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_NOLOAD);
    if (lib != NULL) p = dlsym(lib, name);
  }
  return p;
}

/* The stream's real slack, read once from the handle being written. A 64-frame
 * write does not own 1333 us of deadline: it owns whatever the buffer has not
 * already promised the hardware. Asking the handle costs one call at the first
 * block and removes the guess. */
static void pcm_note_budget(snd_pcm_t* pcm) {
  static int queried;
  static int (*get_params)(snd_pcm_t*, unsigned long*, unsigned long*);

  if (queried) return;
  queried = 1;
  if (get_params == NULL)
    get_params = (int (*)(snd_pcm_t*, unsigned long*, unsigned long*))
        pcm_real("snd_pcm_get_params");
  if (get_params != NULL) {
    unsigned long buffer = 0, period = 0;

    if (get_params(pcm, &buffer, &period) == 0) {
      __atomic_store_n(&pcm_buffer_frames, (unsigned)buffer, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_period_frames, (unsigned)period, __ATOMIC_RELAXED);
    }
  }
}

/* Plain dlsym() is the modern ABI here, and that was measured rather than
 * assumed: on the device's own libasound, dlsym()'s default
 * snd_pcm_hw_params_get_format writes its out-parameter and returns 0, while
 * dlvsym(..., "ALSA_0.9") is the legacy value-returning one and returns the
 * format itself (6) with the out-parameter untouched. Reaching for the
 * versioned symbol therefore breaks the read instead of fixing it -- the
 * caller sees a non-zero return and calls the stream unresolved, which is the
 * `u` the panel shows. The default version of a symbol is the current ABI in
 * every libasound, so dlsym() is the correct and portable choice. */
static void pcm_api_init(void) {
  pcm_api.malloc = (hw_malloc_fn)pcm_real("snd_pcm_hw_params_malloc");
  pcm_api.current = (hw_current_fn)pcm_real("snd_pcm_hw_params_current");
  pcm_api.free = (hw_free_fn)pcm_real("snd_pcm_hw_params_free");
  pcm_api.format = (hw_format_fn)pcm_real("snd_pcm_hw_params_get_format");
  pcm_api.channels = (hw_channels_fn)pcm_real("snd_pcm_hw_params_get_channels");
  pcm_api.rate = (hw_rate_fn)pcm_real("snd_pcm_hw_params_get_rate");
}

/* Called only with pcm_info_lock held. Not pthread_once: if the first lookup
 * happened before libasound was mapped, a one-shot would remember that empty
 * table for the life of the process, and every later stream would be passed
 * through as unresolved. Retrying costs a handful of dlsym calls on an
 * unresolved stream and stops as soon as the table is complete. */
static void pcm_api_lookup(void) {
  if (pcm_api.malloc != NULL && pcm_api.current != NULL &&
      pcm_api.free != NULL && pcm_api.format != NULL &&
      pcm_api.channels != NULL && pcm_api.rate != NULL) {
    return;
  }
  pcm_api_init();
}

static unsigned long long pcm_now_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
  return (unsigned long long)ts.tv_sec * 1000 +
         (unsigned long long)ts.tv_nsec / 1000000;
}

static unsigned long long pcm_now_ns(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
  return (unsigned long long)ts.tv_sec * 1000000000ULL +
         (unsigned long long)ts.tv_nsec;
}

/* How much CPU the calling thread has actually used. This is a syscall on
 * armhf rather than a vDSO read, which is why it is taken three times per
 * block and never in a loop. A failure is reported as 0 and the accounting is
 * skipped, so a kernel without the clock costs the numbers and not the audio. */
static unsigned long long pcm_cpu_ns(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
  return (unsigned long long)ts.tv_sec * 1000000000ULL +
         (unsigned long long)ts.tv_nsec;
}

/* A maximum written from the audio thread and read from the panel thread.
 * compare_exchange rather than a load/store pair so two writers cannot lose
 * the larger value. */
static void pcm_note_max(unsigned long long* slot, unsigned long long value) {
  unsigned long long cur = __atomic_load_n(slot, __ATOMIC_RELAXED);

  while (value > cur &&
         !__atomic_compare_exchange_n(slot, &cur, value, 0, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {
  }
}

/* Decide, once, whether the playback thread should be given real-time
 * scheduling -- from measurement rather than from policy.
 *
 * On the drumlogue with a second synth hosted, this thread ran SCHED_OTHER at
 * nice 0 while everything else on the instrument competed with it on equal
 * terms: the panel thread, MIDI, and on a PREEMPT_RT kernel the threaded IRQs.
 * The stream has four 64-frame periods (5.3 ms) of buffer, so an EPIPE means
 * the thread did not run for longer than that, and none of the hook's own tens
 * of microseconds can explain it.
 *
 * Which answer is right depends on one number: the CPU the thread consumes per
 * cycle. A thread that is idle for much of a cycle is being descheduled, and
 * SCHED_FIFO stops every ordinary thread from taking the core away for a
 * timeslice. A thread already burning most of the cycle means a full core,
 * where raising the priority would only take the remaining share from the
 * panel, so it is left alone. A loop that busy-waits out its deadline spends
 * the whole cycle either way, so the ratio distinguishes the two without
 * knowing what runs between the writes.
 *
 * The priority is 1, the lowest real-time one, deliberately. At 50 -- where a
 * PREEMPT_RT kernel's threaded IRQs sit -- this was measured on the drumlogue
 * to starve the I/O the instrument needs: the panel and the sequencer LEDs went
 * irregular whenever the audio thread had work, and exec replies over USB MIDI
 * stopped coming back entirely. Every driver thread the kernel created outranks
 * 1 and keeps the precedence it was given; what changes is only that the
 * playback thread is no longer scheduled as an ordinary thread.
 *
 * The sample is the first 256 cycles, before a module is usually loaded, and
 * that is deliberate: a busy-wait is a property of the loop and is present at
 * any load, so an idle instrument gives the same answer as a playing one.
 */
static void pcm_rt_maybe(unsigned long long cpu, unsigned long long cycle) {
  static unsigned long long cpu_sum, cycle_sum;
  static unsigned samples;

  if (__atomic_load_n(&pcm_rt_state, __ATOMIC_RELAXED) != PCM_RT_UNDECIDED) {
    return;
  }
  cpu_sum += cpu;
  cycle_sum += cycle;
  if (++samples < 256) return;

  __atomic_store_n(&pcm_rt_cpu_us, cpu_sum / samples / 1000, __ATOMIC_RELAXED);
  __atomic_store_n(&pcm_rt_cycle_us, cycle_sum / samples / 1000,
                   __ATOMIC_RELAXED);
  /* Three quarters of the cycle, on the average over the sample. */
  if (4 * cpu_sum < 3 * cycle_sum) {
    struct sched_param sp;
    int policy = SCHED_OTHER;
    int state = PCM_RT_REFUSED;

    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0 &&
        policy != SCHED_OTHER) {
      /* Already real-time: something above this object set it. Leave it. */
      state = PCM_RT_FIFO;
    } else {
      sp.sched_priority = 1;
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
        state = PCM_RT_FIFO;
      }
    }
    __atomic_store_n(&pcm_rt_state, state, __ATOMIC_RELAXED);
  } else {
    __atomic_store_n(&pcm_rt_state, PCM_RT_CFS, __ATOMIC_RELAXED);
  }
}

/* The end of one hook entry, after the library's write returned. `t_in` and
 * `t_cpu` are the two clocks taken at entry; the hook and the write have timed
 * their own CPU in between, and what is left of the cycle is the daemon's own
 * work for the block. */
static void pcm_block_cpu(unsigned long long t_in, unsigned long long t_cpu,
                          unsigned long long cpu_hook,
                          unsigned long long cpu_write) {
  if (t_cpu == 0) return;
  if (pcm_prev_in_cpu != 0 && t_cpu > pcm_prev_in_cpu &&
      t_in > pcm_prev_in_wall) {
    const unsigned long long cycle = t_in - pcm_prev_in_wall;
    const unsigned long long used = t_cpu - pcm_prev_in_cpu;
    const unsigned long long accounted = pcm_prev_hook_cpu + pcm_prev_write_cpu;
    const unsigned long long engine = used > accounted ? used - accounted : 0;

    __atomic_store_n(&pcm_engine_cpu_ns, engine, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_cycle_ns, cycle, __ATOMIC_RELAXED);
    pcm_note_max(&pcm_engine_cpu_max, engine);
    pcm_rt_maybe(used, cycle);
  }
  __atomic_store_n(&pcm_hook_cpu_ns, cpu_hook, __ATOMIC_RELAXED);
  pcm_note_max(&pcm_hook_cpu_max, cpu_hook);
  pcm_prev_in_wall = t_in;
  pcm_prev_in_cpu = t_cpu;
  pcm_prev_hook_cpu = cpu_hook;
  pcm_prev_write_cpu = cpu_write;
}

static void pcm_skip_set(int reason) {
  __atomic_store_n(&pcm_skip, reason, __ATOMIC_RELAXED);
  __atomic_add_fetch(&pcm_skips, 1, __ATOMIC_RELAXED);
}

/* Merge one shape into an entry and say whether this code can mix it. A field
 * that is not being supplied is passed as -1 (format) or 0 (channels, rate)
 * and the entry keeps what it had: the daemon sets the three fields in
 * separate calls, and a stream is usable as soon as the format and the channel
 * count are known. The rate is not required -- it does not affect mixing, and
 * a failed rate read used to be enough to keep a stream unresolved.
 *
 * An unsupported format is kept so the STATUS page can name it, but the entry
 * is not marked resolved, so a later call can correct it. Latching a failure
 * here is what once disarmed the mix for the life of the process: the daemon
 * entered the hook on every block, the call counter rose, and not one module
 * render was ever asked for. */
static int pcm_info_set(pcm_info* pi, int format, unsigned channels,
                        unsigned rate) {
  unsigned bytes = 0;

  if (format >= 0) pi->format = format;
  if (channels > 0) pi->channels = channels;
  if (rate > 0) pi->rate = rate;

  if (pi->format == PCM_FORMAT_S16_LE) {
    bytes = 2;
  } else if (pi->format == PCM_FORMAT_S24_LE ||
             pi->format == PCM_FORMAT_S32_LE ||
             pi->format == PCM_FORMAT_FLOAT_LE) {
    bytes = 4;
  }
  pi->bytes = bytes;
  /* A module that initialises a unit reads this once the stream is known; 0
   * before that is documented as "treat as 48000". */
  if (pi->rate > 0) host_services.samplerate = pi->rate;
  if (bytes == 0 || pi->channels == 0 || pi->channels > PCM_MAX_CHANNELS) {
    __atomic_store_n(&pi->resolved, 0, __ATOMIC_RELEASE);
    return 0;
  }
  /* The values shown on the panel are published from pcm_mix, not here: the
   * daemon opens a capture stream too, and publishing from the hook lets the
   * capture shape overwrite the playback shape on a page that is describing
   * what the modules are mixed into. */
  __atomic_store_n(&pi->resolved, 1, __ATOMIC_RELEASE);
  return 1;
}

static int pcm_info_from_params(pcm_info* pi, const void* params) {
  int fmt = -1;
  unsigned channels = 0, rate = 0;
  int dir = 0;

  pcm_api_lookup();
  if (pcm_api.format == NULL || pcm_api.channels == NULL ||
      pcm_api.rate == NULL) {
    return 0;
  }
  if (pcm_api.format(params, &fmt) != 0) return 0;
  if (pcm_api.channels(params, &channels) != 0) return 0;
  if (pcm_api.rate(params, &rate, &dir) != 0) return 0;
  return pcm_info_set(pi, fmt, channels, rate);
}

/* The fallback, for a stream that was configured before this object was
 * preloaded. pcm_info_get retries it; a failure is not remembered. */
static int pcm_info_from_current(pcm_info* pi) {
  void* params = NULL;
  int ok = 0;

  pcm_api_lookup();
  if (pcm_api.malloc == NULL || pcm_api.current == NULL ||
      pcm_api.format == NULL || pcm_api.channels == NULL ||
      pcm_api.rate == NULL || pcm_api.free == NULL) {
    return 0;
  }
  if (pcm_api.malloc(&params) != 0 || params == NULL) return 0;
  if (pcm_api.current(pi->pcm, params) == 0) {
    ok = pcm_info_from_params(pi, params);
  }
  pcm_api.free(params);
  return ok;
}

/* Find the entry for a stream, allocating or evicting one. The caller holds
 * pcm_info_lock. */
static pcm_info* pcm_info_slot(snd_pcm_t* pcm) {
  int i;

  for (i = 0; i < PCM_INFO_MAX; i++) {
    if (pcm_infos[i].pcm == pcm) return &pcm_infos[i];
  }
  for (i = 0; i < PCM_INFO_MAX; i++) {
    if (pcm_infos[i].pcm == NULL) break;
  }
  if (i == PCM_INFO_MAX) {
    /* A daemon that closes and reopens its stream would otherwise run out of
     * slots, and every later stream would be passed through for the rest of
     * the run -- a silence indistinguishable from a broken module. Evict the
     * entry that was seen first. */
    int oldest = 0;
    for (i = 1; i < PCM_INFO_MAX; i++) {
      if (pcm_infos[i].seq < pcm_infos[oldest].seq) oldest = i;
    }
    i = oldest;
  }
  memset(&pcm_infos[i], 0, sizeof pcm_infos[i]);
  pcm_infos[i].pcm = pcm;
  pcm_infos[i].seq = ++pcm_info_seq;
  /* -1, not 0: zero is a real ALSA format id (S8), and an entry that has not
   * been told anything yet must not read as if the stream were S8. The panel
   * showed `F0 0C` for a stream no call had described, which is the same
   * reading a real S8 stream would give. */
  pcm_infos[i].format = -1;
  return &pcm_infos[i];
}

/* Returns the stream's descriptor. The fast path is a lock-free read of a
 * completed entry; an unresolved stream is looked up under the lock and, if it
 * still cannot be read, retried on every block at first and every 32nd after
 * that. The dense start matters: a stream that is configured while this object
 * is being attached settles within a few blocks, and waiting 32 blocks for the
 * first retry is a second of silence at 64-frame blocks rather than an
 * instant one. */
static pcm_info* pcm_info_get(snd_pcm_t* pcm) {
  pcm_info* pi;
  int i;

  for (i = 0; i < PCM_INFO_MAX; i++) {
    if (pcm_infos[i].pcm == pcm &&
        __atomic_load_n(&pcm_infos[i].resolved, __ATOMIC_ACQUIRE)) {
      return &pcm_infos[i];
    }
  }

  pthread_mutex_lock(&pcm_info_lock);
  __atomic_add_fetch(&pcm_get_slow, 1, __ATOMIC_RELAXED);
  pi = pcm_info_slot(pcm);
  if (!__atomic_load_n(&pi->resolved, __ATOMIC_ACQUIRE)) {
    pi->attempts++;
    if (pi->attempts <= 64 || (pi->attempts & 31) == 0) {
      pcm_info_from_current(pi);
    }
  }
  pthread_mutex_unlock(&pcm_info_lock);
  return pi;
}

/* Merge one field supplied by one of the parameter calls into the stream's
 * entry. The daemon names its format, channels and rate in separate calls
 * before installing them, and those calls carry the values as arguments -- no
 * params object has to be readable for this to work. That matters because an
 * unresolved stream is passed through in silence, and reading the object back
 * (or querying the installed parameters) is the step that has failed on the
 * instrument while the write path itself was hooked. */
static void pcm_info_note(snd_pcm_t* pcm, int format, unsigned channels,
                          unsigned rate) {
  pcm_info* pi;

  if (pcm == NULL) return;
  pthread_mutex_lock(&pcm_info_lock);
  pi = pcm_info_slot(pcm);
  pcm_info_set(pi, format, channels, rate);
  pthread_mutex_unlock(&pcm_info_lock);
}

/* The three setters are recorded only when they succeed, so a format the
 * library refused is not mistaken for the one the stream has. Each passes -1
 * or 0 for the fields it does not carry. */
int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, void* params, int format) {
  static int (*real)(snd_pcm_t*, void*, int);
  int rc;

  if (real == NULL)
    real = (int (*)(snd_pcm_t*, void*, int))
        pcm_real("snd_pcm_hw_params_set_format");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  rc = real(pcm, params, format);
  if (rc == 0) pcm_info_note(pcm, format, 0, 0);
  return rc;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, void* params,
                                   unsigned channels) {
  static int (*real)(snd_pcm_t*, void*, unsigned);
  int rc;

  if (real == NULL)
    real = (int (*)(snd_pcm_t*, void*, unsigned))
        pcm_real("snd_pcm_hw_params_set_channels");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  rc = real(pcm, params, channels);
  if (rc == 0) pcm_info_note(pcm, -1, channels, 0);
  return rc;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, void* params, unsigned rate,
                               int dir) {
  static int (*real)(snd_pcm_t*, void*, unsigned, int);
  int rc;

  if (real == NULL)
    real = (int (*)(snd_pcm_t*, void*, unsigned, int))
        pcm_real("snd_pcm_hw_params_set_rate");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  rc = real(pcm, params, rate, dir);
  if (rc == 0) pcm_info_note(pcm, -1, 0, rate);
  return rc;
}

/* The convenience API carries all three at once. A daemon that uses it does
 * not necessarily reach the snd_pcm_hw_params interposer below as a call of
 * its own, so this is recorded here rather than relying on that. */
int snd_pcm_set_params(snd_pcm_t* pcm, int format, int access,
                       unsigned channels, unsigned rate, int soft_resample,
                       unsigned latency) {
  static int (*real)(snd_pcm_t*, int, int, unsigned, unsigned, int, unsigned);
  int rc;

  if (real == NULL)
    real = (int (*)(snd_pcm_t*, int, int, unsigned, unsigned, int, unsigned))
        pcm_real("snd_pcm_set_params");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  rc = real(pcm, format, access, channels, rate, soft_resample, latency);
  if (rc == 0) pcm_info_note(pcm, format, channels, rate);
  return rc;
}

/* The daemon installs the stream's shape here, and it is recorded here too.
 *
 * snd_pcm_hw_params_current() answers only once the parameters are installed,
 * which is the wrong moment: a block that arrives earlier, or one failed read,
 * used to leave the stream unresolved for the life of the process. The object
 * passed here carries the values the daemon chose, and they are what its
 * interleaved buffer will contain -- a plug converts after this point, not
 * before it. These are the same values the setters above already recorded, and
 * merging them is harmless; this call remains the authoritative confirmation
 * whenever it can be read. The values are read before the real call because a
 * plugin is free to rewrite the object it was handed. */
int snd_pcm_hw_params(snd_pcm_t* pcm, void* params) {
  static int (*real)(snd_pcm_t*, void*);

  if (real == NULL)
    real = (int (*)(snd_pcm_t*, void*))pcm_real("snd_pcm_hw_params");
  if (pcm != NULL && params != NULL) {
    pthread_mutex_lock(&pcm_info_lock);
    pcm_info_from_params(pcm_info_slot(pcm), params);
    pthread_mutex_unlock(&pcm_info_lock);
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real(pcm, params);
}

/* A closed stream's slot is released, so a later snd_pcm_t reusing the same
 * address does not inherit an unrelated format. */
int snd_pcm_close(snd_pcm_t* pcm) {
  static int (*real)(snd_pcm_t*);
  int i;

  if (real == NULL)
    real = (int (*)(snd_pcm_t*))pcm_real("snd_pcm_close");
  if (pcm != NULL) {
    pthread_mutex_lock(&pcm_info_lock);
    for (i = 0; i < PCM_INFO_MAX; i++) {
      if (pcm_infos[i].pcm == pcm) {
        memset(&pcm_infos[i], 0, sizeof pcm_infos[i]);
        break;
      }
    }
    pthread_mutex_unlock(&pcm_info_lock);
  }
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  return real(pcm);
}

static int32_t float_to_s16(float x) {
  if (!(x == x)) return 0;                /* NaN */
  if (x >= 1.0f) return 32767;
  if (x <= -1.0f) return -32768;
  return (int32_t)(x * 32767.0f);
}

static int32_t float_to_s32(float x) {
  if (!(x == x)) return 0;
  if (x >= 1.0f) return 2147483647;
  if (x <= -1.0f) return -2147483647 - 1;
  return (int32_t)(x * 2147483647.0f);
}

static int32_t float_to_s24(float x) {
  if (!(x == x)) return 0;
  if (x >= 1.0f) return PCM_S24_FULL_SCALE - 1;
  if (x <= -1.0f) return -PCM_S24_FULL_SCALE;
  return (int32_t)(x * (float)(PCM_S24_FULL_SCALE - 1));
}

/* S24_LE keeps its sample in the low three bytes of a four-byte slot. The
 * fourth byte is padding, so it is never summed as part of the sample: the
 * field is sign-extended out of the low three bytes first, and the sum is
 * written back sign-extended into all four. */
static void s24_add(unsigned char* p, int32_t add) {
  int32_t cur = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16));
  int64_t sum;

  /* Sign-extend the 24-bit field without a left shift into the sign bit,
   * which is undefined for 0x800000. */
  if ((cur & 0x800000) != 0) cur -= 0x1000000;
  sum = (int64_t)cur + (int64_t)add;
  if (sum > PCM_S24_FULL_SCALE - 1) sum = PCM_S24_FULL_SCALE - 1;
  if (sum < -PCM_S24_FULL_SCALE) sum = -PCM_S24_FULL_SCALE;
  p[0] = (unsigned char)((uint64_t)sum & 0xff);
  p[1] = (unsigned char)(((uint64_t)sum >> 8) & 0xff);
  p[2] = (unsigned char)(((uint64_t)sum >> 16) & 0xff);
  p[3] = (unsigned char)(((uint64_t)sum >> 24) & 0xff);
}

/* Add one chunk of the modules' stereo mix into the PCM block. For the
 * codec's own eight-channel S24 stream the pair is added to every stereo pair
 * (see the header comment); the other formats, which no drumlogue stream uses,
 * keep the narrow two-channel behaviour. A mono stream gets the two averaged. */
static void pcm_add(unsigned char* base, unsigned long frame,
                    const pcm_info* pi, const float* mix, unsigned n) {
  unsigned i, ch;
  unsigned char* p = base + (size_t)frame * pi->channels * pi->bytes;

  if (pi->format == PCM_FORMAT_S16_LE) {
    int16_t* out = (int16_t*)p;
    for (i = 0; i < n; i++) {
      int32_t add = float_to_s16(0.5f * (mix[i * 2] + mix[i * 2 + 1]));
      if (pi->channels > 1) {
        for (ch = 0; ch < 2; ch++) {
          int32_t sum = (int32_t)out[i * pi->channels + ch] +
                        float_to_s16(mix[i * 2 + ch]);
          if (sum > 32767) sum = 32767;
          if (sum < -32768) sum = -32768;
          out[i * pi->channels + ch] = (int16_t)sum;
        }
      } else {
        int32_t sum = (int32_t)out[i] + add;
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        out[i] = (int16_t)sum;
      }
    }
  } else if (pi->format == PCM_FORMAT_S32_LE) {
    int32_t* out = (int32_t*)p;
    for (i = 0; i < n; i++) {
      int32_t add = float_to_s32(0.5f * (mix[i * 2] + mix[i * 2 + 1]));
      if (pi->channels > 1) {
        for (ch = 0; ch < 2; ch++) {
          int64_t sum = (int64_t)out[i * pi->channels + ch] +
                        float_to_s32(mix[i * 2 + ch]);
          if (sum > 2147483647LL) sum = 2147483647LL;
          if (sum < -2147483648LL) sum = -2147483648LL;
          out[i * pi->channels + ch] = (int32_t)sum;
        }
      } else {
        int64_t sum = (int64_t)out[i] + add;
        if (sum > 2147483647LL) sum = 2147483647LL;
        if (sum < -2147483648LL) sum = -2147483648LL;
        out[i] = (int32_t)sum;
      }
    }
  } else if (pi->format == PCM_FORMAT_FLOAT_LE) {
    float* out = (float*)p;
    for (i = 0; i < n; i++) {
      if (pi->channels > 1) {
        out[i * pi->channels] += mix[i * 2];
        out[i * pi->channels + 1] += mix[i * 2 + 1];
      } else {
        out[i] += 0.5f * (mix[i * 2] + mix[i * 2 + 1]);
      }
    }
  } else if (pi->format == PCM_FORMAT_S24_LE) {
    unsigned char* out = p;
    for (i = 0; i < n; i++) {
      unsigned char* f = out + (size_t)i * pi->channels * 4;
      const unsigned pairs = pi->channels / 2;
      const int32_t mono = float_to_s24(0.5f * (mix[i * 2] + mix[i * 2 + 1]));

      if (pi->channels == 1) {
        s24_add(f, mono);
        continue;
      }
      /* Converted once per frame, not once per pair. The eight-channel stream
       * is four pairs, so the obvious loop converted the same two samples
       * eight times to write them four times -- on the instrument that is
       * measurable next to the block's period. */
      {
        const int32_t l = float_to_s24(mix[i * 2]);
        const int32_t r = float_to_s24(mix[i * 2 + 1]);

        for (ch = 0; ch < pairs; ch++) {
          s24_add(f + (size_t)ch * 8, l);
          s24_add(f + (size_t)ch * 8 + 4, r);
        }
      }
      if ((pi->channels & 1) != 0) s24_add(f + (size_t)pairs * 8, mono);
    }
  }
}

/* Mix the modules into a copy of the caller's block and return the copy, or
 * return the block untouched when there is nothing to add. */
static const void* pcm_mix(snd_pcm_t* pcm, const void* buf,
                           unsigned long frames) {
  pcm_info* pi;
  size_t bytes;
  unsigned long off;
  unsigned long long t_start, mod_ns = 0;

  /* First call on the audio thread: give it a stack to fault on. Done here and
   * not in a constructor because an alternate signal stack is per thread, and
   * the daemon's audio thread does not exist when this object is loaded. */
  {
    static int alt_installed;
    if (!alt_installed) {
      struct sched_param sp;
      int policy = SCHED_OTHER;

      alt_installed = 1;
      crash_altstack(crash_stack_audio, sizeof crash_stack_audio);
      if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0) {
        __atomic_store_n(&pcm_audio_policy, policy, __ATOMIC_RELAXED);
        __atomic_store_n(&pcm_audio_prio, sp.sched_priority, __ATOMIC_RELAXED);
      }
    }
  }

  /* The whole hook is timed, not just the mixing loop: the copy in and the
   * scratch hand-over are part of what the audio thread pays, and a block
   * that misses the period because of them would otherwise look innocent. */
  t_start = pcm_now_ns();
  __atomic_store_n(&pcm_beat_ms, pcm_now_ms(), __ATOMIC_RELAXED);
  __atomic_store_n(&pcm_stage, PCM_STAGE_HOOK, __ATOMIC_RELAXED);
  if (buf == NULL || frames == 0 || frames > PCM_SCRATCH_FRAMES) {
    pcm_skip_set(PCM_SKIP_BIG);
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    return buf;
  }
  pi = pcm_info_get(pcm);
  /* Publish the observed shape from the handle being written, so the panel
   * describes the playback stream and not whichever stream was configured
   * last. It is published before the refusals below on purpose: a stream that
   * is refused, or a module set that is empty, then still shows what the
   * instrument's playback actually asked for. `NONE 0C` is the one reading
   * that means nothing has been observed at all. */
  if (pi != NULL) {
    __atomic_store_n(&audio_pcm_format, pi->format, __ATOMIC_RELAXED);
    __atomic_store_n(&audio_pcm_channels, pi->channels, __ATOMIC_RELAXED);
    pcm_note_budget(pcm);
  }
  if (!modules_have_audio()) {
    pcm_skip_set(PCM_SKIP_NO_MODULE);
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    return buf;
  }
  if (pi == NULL || pi->channels == 0 || pi->bytes == 0) {
    pcm_skip_set(PCM_SKIP_UNRESOLVED);
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    return buf;
  }

  bytes = (size_t)frames * pi->channels * pi->bytes;
  if (bytes > sizeof pcm_scratch) {
    pcm_skip_set(PCM_SKIP_BIG);
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    return buf;
  }
  {
    const unsigned long long now = pcm_now_ms();
    if (__atomic_exchange_n(&pcm_scratch_busy, 1, __ATOMIC_ACQ_REL) != 0) {
      /* A second writer, or an owner that never came back. A live owner clears
       * the flag inside the same block; one that has held it for a second is
       * gone (the daemon has one playback thread), and leaving it set would
       * silence the module for the rest of the run. */
      const unsigned long long held =
          __atomic_load_n(&pcm_scratch_stamp, __ATOMIC_RELAXED);
      if (now - held <= 1000) {
        pcm_skip_set(PCM_SKIP_BUSY);
        __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
        return buf;
      }
      __atomic_add_fetch(&pcm_scratch_steals, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&pcm_scratch_stamp, now, __ATOMIC_RELAXED);
  }

  memcpy(pcm_scratch, buf, bytes);
  for (off = 0; off < frames; off += AUDIO_CHUNK) {
    const unsigned n = (frames - off > AUDIO_CHUNK)
                           ? AUDIO_CHUNK
                           : (unsigned)(frames - off);
    float mix[AUDIO_CHUNK * 2];
    unsigned long long a;
    memset(mix, 0, n * 2 * sizeof(float));
    a = pcm_now_ns();
    modules_audio(mix, n);
    mod_ns += pcm_now_ns() - a;
    /* Adding silence is not free on the instrument's core: scanning 2n floats
     * is a fraction of converting and summing n*channels samples. A module
     * with nothing to say -- the common case while another engine plays --
     * must not cost the whole conversion anyway. */
    {
      unsigned i;
      int any = 0;
      for (i = 0; i < n * 2; i++) {
        if (mix[i] != 0.0f) { any = 1; break; }
      }
      if (any) pcm_add(pcm_scratch, off, pi, mix, n);
    }
  }
  {
    const unsigned long long total = pcm_now_ns() - t_start;
    __atomic_store_n(&pcm_frames_last, (unsigned)frames, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_mix_ns_last, total, __ATOMIC_RELAXED);
    pcm_note_max(&pcm_mix_ns_max, total);
    __atomic_store_n(&pcm_mod_ns_last, mod_ns, __ATOMIC_RELAXED);
    pcm_note_max(&pcm_mod_ns_max, mod_ns);
  }
  __atomic_store_n(&pcm_scratch_busy, 0, __ATOMIC_RELEASE);
  pcm_skip_set(PCM_SKIP_NONE);
  __atomic_add_fetch(&audio_mixed, 1, __ATOMIC_RELAXED);
  return pcm_scratch;
}

long snd_pcm_writei(snd_pcm_t* pcm, const void* buf, unsigned long frames) {
  static long (*real)(snd_pcm_t*, const void*, unsigned long);
  if (real == NULL) real = (long (*)(snd_pcm_t*, const void*, unsigned long))
      pcm_real("snd_pcm_writei");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const unsigned long long t_in = pcm_now_ns();
    const unsigned long long t_cpu = pcm_cpu_ns();
    const unsigned long long prev =
        __atomic_load_n(&pcm_prev_ret_ns, __ATOMIC_RELAXED);
    const unsigned long long gap = prev != 0 ? t_in - prev : 0;
    unsigned long long t_write, t_done, t_cpu_write, cpu_hook, cpu_write;
    const void* mixed;
    long got;

    if (prev != 0) {
      __atomic_store_n(&pcm_gap_ns_last, gap, __ATOMIC_RELAXED);
      pcm_note_max(&pcm_gap_ns_max, gap);
    }
    mixed = pcm_mix(pcm, buf, frames);
    cpu_hook = pcm_cpu_ns() - t_cpu;

    /* The stage is set between the two calls on purpose: the mix has returned,
     * so a block reported as WRITE is one the library is holding, not one of
     * ours. That is the difference between a slow module and a dead stream. */
    __atomic_store_n(&pcm_stage, PCM_STAGE_WRITE, __ATOMIC_RELAXED);
    t_write = pcm_now_ns();
    t_cpu_write = pcm_cpu_ns();
    __atomic_store_n(&pcm_beat_ms, t_write / 1000000, __ATOMIC_RELAXED);
    got = real(pcm, mixed, frames);
    t_done = pcm_now_ns();
    cpu_write = pcm_cpu_ns() - t_cpu_write;
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_beat_ms, t_done / 1000000, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_prev_ret_ns, t_done, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_write_ns_last, t_done - t_write, __ATOMIC_RELAXED);
    pcm_note_max(&pcm_write_ns_max, t_done - t_write);
    pcm_block_cpu(t_in, t_cpu, cpu_hook, cpu_write);
    __atomic_add_fetch(&audio_calls, 1, __ATOMIC_RELAXED);
    if (got < 0) {
      __atomic_add_fetch(&audio_errors, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_write_err, (int)got, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_gap_ns, gap, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_mix_ns,
                       __atomic_load_n(&pcm_mix_ns_last, __ATOMIC_RELAXED),
                       __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_mod_ns,
                       __atomic_load_n(&pcm_mod_ns_last, __ATOMIC_RELAXED),
                       __ATOMIC_RELAXED);
    }
    return got;
  }
}

long snd_pcm_mmap_writei(snd_pcm_t* pcm, const void* buf, unsigned long frames) {
  static long (*real)(snd_pcm_t*, const void*, unsigned long);
  if (real == NULL)
    real = (long (*)(snd_pcm_t*, const void*, unsigned long))
        pcm_real("snd_pcm_mmap_writei");
  if (real == NULL) {
    errno = ENOSYS;
    return -1;
  }
  {
    const unsigned long long t_in = pcm_now_ns();
    const unsigned long long t_cpu = pcm_cpu_ns();
    const unsigned long long prev =
        __atomic_load_n(&pcm_prev_ret_ns, __ATOMIC_RELAXED);
    const unsigned long long gap = prev != 0 ? t_in - prev : 0;
    unsigned long long t_write, t_done, t_cpu_write, cpu_hook, cpu_write;
    const void* mixed;
    long got;

    if (prev != 0) {
      __atomic_store_n(&pcm_gap_ns_last, gap, __ATOMIC_RELAXED);
      pcm_note_max(&pcm_gap_ns_max, gap);
    }
    mixed = pcm_mix(pcm, buf, frames);
    cpu_hook = pcm_cpu_ns() - t_cpu;

    __atomic_store_n(&pcm_stage, PCM_STAGE_WRITE, __ATOMIC_RELAXED);
    t_write = pcm_now_ns();
    t_cpu_write = pcm_cpu_ns();
    __atomic_store_n(&pcm_beat_ms, t_write / 1000000, __ATOMIC_RELAXED);
    got = real(pcm, mixed, frames);
    t_done = pcm_now_ns();
    cpu_write = pcm_cpu_ns() - t_cpu_write;
    __atomic_store_n(&pcm_stage, PCM_STAGE_IDLE, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_beat_ms, t_done / 1000000, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_prev_ret_ns, t_done, __ATOMIC_RELAXED);
    __atomic_store_n(&pcm_write_ns_last, t_done - t_write, __ATOMIC_RELAXED);
    pcm_note_max(&pcm_write_ns_max, t_done - t_write);
    pcm_block_cpu(t_in, t_cpu, cpu_hook, cpu_write);
    __atomic_add_fetch(&audio_calls, 1, __ATOMIC_RELAXED);
    if (got < 0) {
      __atomic_add_fetch(&audio_errors, 1, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_write_err, (int)got, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_gap_ns, gap, __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_mix_ns,
                       __atomic_load_n(&pcm_mix_ns_last, __ATOMIC_RELAXED),
                       __ATOMIC_RELAXED);
      __atomic_store_n(&pcm_err_mod_ns,
                       __atomic_load_n(&pcm_mod_ns_last, __ATOMIC_RELAXED),
                       __ATOMIC_RELAXED);
    }
    return got;
  }
}

#ifdef BORG_AUDIO_PROBE
/* Test-only, deliberately absent from the shipped object: the same numbers the
 * STATUS page draws, for a harness that drives this object against a real
 * libasound without a panel. Built with -DBORG_AUDIO_PROBE. */
int borg_audio_probe(int* format, unsigned* channels, unsigned long* mixed,
                     unsigned long* calls) {
  if (format != NULL)
    *format = __atomic_load_n(&audio_pcm_format, __ATOMIC_RELAXED);
  if (channels != NULL)
    *channels = __atomic_load_n(&audio_pcm_channels, __ATOMIC_RELAXED);
  if (mixed != NULL) *mixed = __atomic_load_n(&audio_mixed, __ATOMIC_RELAXED);
  if (calls != NULL) *calls = __atomic_load_n(&audio_calls, __ATOMIC_RELAXED);
  return 0;
}
#endif
