/**
 * @file borg.cc
 * @brief The switch, the panel paint and the chord menu on the microKORG2.
 *
 * Three things, in the order they happen.
 *
 * 1. The switch. Any C (kActivatePitchClass) is the request. It is read from
 *    the oscillator runtime context on the render callback: this platform
 *    gives an osc unit no note callback, and that context is where a note
 *    exists. On a fresh trigger the resident libborg.so is staged to /tmp and
 *    this process execve(2)s /proc/self/exe with the object in LD_PRELOAD.
 *    Same pid, fresh image. execve is what the microKORG2 permits, as the
 *    drumlogue does; borg_drumlogue's carrying-across code (argv, LD_PRELOAD,
 *    scheduler, FD_CLOEXEC above 2) is used unchanged.
 *
 *    A note and not a file: a file trigger has to be found at unit_init, before
 *    anyone has touched the instrument. A note is something the player does,
 *    and it proves the unit is live in the daemon when it asks.
 *
 * 2. The menu. FUNCTION 5 + FUNCTION 3 opens it, once the switch has happened:
 *    before that the shim is not in the process, so the link does not exist.
 *    The link is the struct in borg_api.h. The shim rewrites the chord's second
 *    key to FUNCTION 2, so microkorgd opens its COPY screen underneath, and the
 *    menu is drawn after that page. See the menu section below.
 *
 * 3. The panel. Id-0 text frames are the panel's console, not a positioned
 *    draw: they take the display and cannot be erased. The row of field labels
 *    (id 0x36) is a layer over that console and survives only if it is written
 *    after it.
 *
 * NO FLASH WRITES FOR DIAGNOSTICS. The unit log is in /tmp, which is tmpfs on
 * this instrument. The only userfs write is the staged object, and only as a
 * fallback when /tmp cannot hold it.
 */

#include "borg.h"

#include "borg_api.h"   /* the in-process link to the shim */
#include "unit_osc.h"   /* unit_runtime_osc_context_t, for the activation note */

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Compile-time literals so a native test can redirect them with -D; a build
 * without overrides is exactly the device build. */
#ifndef BORG_USERFS_DIR
#define BORG_USERFS_DIR "/var/lib/microkorgd/userfs/Programs"
#endif
#ifndef BORG_TMP_DIR
#define BORG_TMP_DIR "/tmp"
#endif
#ifndef BORG_LOG_PATH
#define BORG_LOG_PATH BORG_TMP_DIR "/logue-borg-unit.log"
#endif
#ifndef BORG_SHIM_LOG
#define BORG_SHIM_LOG BORG_TMP_DIR "/logue-borg.log"
#endif
#ifndef BORG_SHIM_TMP
#define BORG_SHIM_TMP BORG_TMP_DIR "/libborg.so"
#endif
#ifndef BORG_SHIM_USERFS
#define BORG_SHIM_USERFS BORG_USERFS_DIR "/libborg.so"
#endif
/* NO PATH FOR THE CHORD, THE MENU, THE INPUT OR THE LABELS. That was four
 * files under /tmp, polled by the unit and written by the shim, and it is gone:
 * see borg_api.h for what the board said about it. What is left under /tmp is
 * logging, and the staged shim. */
#ifndef BORG_PANEL_PATH
#define BORG_PANEL_PATH "/dev/ttymxc2"
#endif
#ifndef BORG_SELF
#define BORG_SELF "/proc/self/exe"
#endif
#ifndef BORG_CMDLINE
#define BORG_CMDLINE "/proc/self/cmdline"
#endif

/* The resident object, assembled into this unit by shim_blob.S. */
extern "C" {
extern const unsigned char shim_blob_start[];
extern const unsigned char shim_blob_end[];
}

namespace borg {

/* The STATE string, defined at the end of this file outside the anonymous
 * namespace because the unit's parameter getter calls it by name. Declared here
 * so the menu below can read the phase. */
const char* StateText();

namespace {

/* The same four phases as borg_drumlogue, with the same STATE tags, because the
 * instrument's parameter readout is how the two are compared: !ready before the
 * switch, !active once this image is running under the shim. The mk2's first
 * version used its own names and reported !idle/!run, so a working switch
 * still read as something the drumlogue never shows. */
enum Phase { kReady = 0, kSwitching, kActive, kFailed };
Phase g_phase = kReady;

/* The request. Any C -- pitch class 0, whatever octave the OCTAVE SHIFT
 * buttons are on. A fixed MIDI note was the first choice and is wrong twice
 * over: note 0 cannot be played on the keyboard at all, and note 60 is only
 * middle C for one octave-shift setting, so the key to press would depend on a
 * switch the player is not looking at. Every C key sends some note == 0 mod 12,
 * so the rule survives the octave switch and the text can say "any C" without
 * naming a note number. Change this one constant to move it; the on-screen
 * text has to change with it. */
constexpr int kActivatePitchClass = 0;   /* 0 = C */

/* Edge detection on the trigger bit array: `trigger` is a level (the bit is
 * set while the voice is held), so a held note must not re-request on every
 * callback. */
uint8_t g_prev_trigger = 0;

/* One paint per state change, held a quarter of a second so the daemon's own
 * page burst has finished first. */
constexpr uint32_t kPaintDelayMs = 250;
bool g_paint_due = false;
uint64_t g_paint_at = 0;

uint32_t g_rate = 48000;
uint64_t g_frames = 0;

bool g_panel_warned = false;

constexpr int kEnvCap = 128;
constexpr int kArgCap = 64;
constexpr int kFdSweep = 1024;

/* The chord is the shim's (FUNCTION 5 + FUNCTION 3, ids 0x0f and 0x11), because
 * the shim is what reads the panel. FUNCTION 3 and not FUNCTION 1: shifted
 * FUNCTION 1 is Initialize Target, one confirmation away from wiping the
 * program, and shifted FUNCTION 3 is the MANUAL QR code, which changes nothing.
 * The daemon never sees the second key anyway: the shim rewrites it to
 * FUNCTION 2 so its COPY screen opens underneath the menu. */

/* SCHED_RESET_ON_FORK is a flag ORed into a policy, not a policy of its own. */
#ifdef SCHED_RESET_ON_FORK
#define POLICY_NUMBER(p) ((p) & ~SCHED_RESET_ON_FORK)
#else
#define POLICY_NUMBER(p) (p)
#endif

/* ---- logging ------------------------------------------------------------- */

void WriteAll(int fd, const char* s, size_t n) {
  while (n > 0) {
    const ssize_t wrote = write(fd, s, n);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return;
    }
    s += static_cast<size_t>(wrote);
    n -= static_cast<size_t>(wrote);
  }
}

/* One line, one open/write/close, to tmpfs. No stdio and no state: the switch
 * can fail before there is anywhere to keep anything, and a lost log line must
 * not be able to take the process with it. */
void Say(const char* fmt, ...) {
  char line[768];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n <= 0) return;

  size_t len = static_cast<size_t>(n);
  if (len > sizeof line - 1) len = sizeof line - 1;
  if (line[len - 1] != '\n') {
    if (len + 1 >= sizeof line) len = sizeof line - 2;
    line[len++] = '\n';
  }

  const int fd = open(BORG_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                      0644);
  if (fd < 0) return;
  WriteAll(fd, line, len);
  close(fd);
}

/* ---- argv and environment ------------------------------------------------ */

bool ReadArgv(char* storage, size_t cap, char** argv, int cap_argv) {
  const int fd = open(BORG_CMDLINE, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;

  size_t off = 0;
  while (off < cap - 1) {
    const ssize_t got = read(fd, storage + off, cap - 1 - off);
    if (got < 0) {
      if (errno == EINTR) continue;
      close(fd);
      return false;
    }
    if (got == 0) break;
    off += static_cast<size_t>(got);
  }
  close(fd);
  storage[off] = '\0';
  if (off == 0) return false;

  int count = 0;
  size_t pos = 0;
  while (pos < off && count < cap_argv - 1) {
    argv[count++] = storage + pos;
    const size_t len = strlen(storage + pos);
    pos += len + 1;
    if (len == 0) {
      --count;  /* the trailing empty string is not an argument */
      break;
    }
  }
  if (count == 0 || argv[0][0] == '\0') return false;
  argv[count] = nullptr;
  return true;
}

/* The environment the new image gets: the old one, minus the entries this
 * switch owns, plus the shim first in LD_PRELOAD. The shim sets the pid marker
 * itself; the unit trusts the marker only because the object that sets it has
 * demonstrably loaded. */
bool BuildEnv(char** out, int cap, const char* shim) {
  static char preload[512];
  static char logenv[256];

  const char* old = getenv("LD_PRELOAD");
  char prior[384];
  snprintf(prior, sizeof prior, "%s", old != nullptr ? old : "");

  char value[448];
  if (prior[0] == '\0') {
    snprintf(value, sizeof value, "%s", shim);
  } else if (strstr(prior, shim) != nullptr) {
    snprintf(value, sizeof value, "%s", prior);
  } else {
    snprintf(value, sizeof value, "%s:%s", shim, prior);
  }

  const int p = snprintf(preload, sizeof preload, "LD_PRELOAD=%s", value);
  const int l = snprintf(logenv, sizeof logenv, "LOGUE_BORG_LOG=%s",
                         BORG_SHIM_LOG);
  if (p < 0 || static_cast<size_t>(p) >= sizeof preload) return false;
  if (l < 0 || static_cast<size_t>(l) >= sizeof logenv) return false;

  int n = 0;
  for (char** e = environ; *e != nullptr; ++e) {
    if (strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_", 11) == 0) continue;
    if (n >= cap - 3) return false;
    out[n++] = *e;
  }
  out[n++] = preload;
  out[n++] = logenv;
  out[n] = nullptr;
  return true;
}

/* ---- the object ---------------------------------------------------------- */

bool WriteBlob(const unsigned char* data, size_t len, const char* path, char* why,
               size_t n) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    return false;
  }
  size_t off = 0;
  while (off < len) {
    const ssize_t wrote = write(fd, data + off, len - off);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
      close(fd);
      unlink(path);
      return false;
    }
    off += static_cast<size_t>(wrote);
  }
  if (close(fd) != 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    unlink(path);
    return false;
  }
  return true;
}

/* ET_DYN, ELF32, little-endian, EM_ARM. The loader validates this too; the
 * check exists so a blob from the wrong build is named in the log rather than
 * appearing as `ERROR: ld.so: object ... cannot be preloaded` where nobody
 * reads it. */
bool LooksLikeArmShim(const char* path, char* why, size_t n) {
  unsigned char h[EI_NIDENT + 4];
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    return false;
  }
  size_t off = 0;
  while (off < sizeof h) {
    const ssize_t got = read(fd, h + off, sizeof h - off);
    if (got < 0) {
      if (errno == EINTR) continue;
      close(fd);
      snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
      return false;
    }
    if (got == 0) break;
    off += static_cast<size_t>(got);
  }
  close(fd);
  if (off < sizeof h) {
    snprintf(why, n, "%s: short read", path);
    return false;
  }

  const uint16_t type = static_cast<uint16_t>(h[EI_NIDENT] | (h[EI_NIDENT + 1] << 8));
  const uint16_t machine = static_cast<uint16_t>(h[EI_NIDENT + 2] |
                                                 (h[EI_NIDENT + 3] << 8));
  if (h[EI_MAG0] != ELFMAG0 || h[EI_MAG1] != ELFMAG1 ||
      h[EI_MAG2] != ELFMAG2 || h[EI_MAG3] != ELFMAG3 || h[EI_CLASS] != ELFCLASS32 ||
      h[EI_DATA] != ELFDATA2LSB || type != ET_DYN || machine != EM_ARM) {
    snprintf(why, n, "%s is not an armhf shared object", path);
    return false;
  }
  return true;
}

/* A mount can be noexec; LD_PRELOAD is mapped PROT_EXEC. */
bool MountAllowsExec(const char* dir, char* why, size_t n) {
  struct statvfs vfs;
  if (statvfs(dir, &vfs) != 0) {
    snprintf(why, n, "%s: %s (%d)", dir, strerror(errno), errno);
    return false;
  }
  if ((vfs.f_flag & ST_NOEXEC) != 0) {
    snprintf(why, n, "%s is mounted noexec", dir);
    return false;
  }
  return true;
}

/* ---- the calling thread -------------------------------------------------- */

struct ThreadState {
  int policy = -1;
  struct sched_param param;
  bool have_nice = false;
  int nice = 0;
  bool have_mask = false;
  sigset_t mask;
  bool have_affinity = false;
  cpu_set_t affinity;
};

const char* PolicyName(int policy) {
  switch (POLICY_NUMBER(policy)) {
    case SCHED_OTHER: return "other";
    case SCHED_FIFO: return "fifo";
    case SCHED_RR: return "rr";
    case SCHED_BATCH: return "batch";
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE: return "deadline";
#endif
    default: return "unknown";
  }
}

void ReadThread(ThreadState* t) {
  t->policy = sched_getscheduler(0);
  memset(&t->param, 0, sizeof t->param);
  if (t->policy >= 0 && sched_getparam(0, &t->param) != 0) {
    t->param.sched_priority = 0;
  }

  errno = 0;
  const int nice = getpriority(PRIO_PROCESS, 0);
  if (!(nice == -1 && errno != 0)) {
    t->have_nice = true;
    t->nice = nice;
  }

  sigemptyset(&t->mask);
  t->have_mask = pthread_sigmask(SIG_SETMASK, nullptr, &t->mask) == 0;

  CPU_ZERO(&t->affinity);
  t->have_affinity = sched_getaffinity(0, sizeof t->affinity, &t->affinity) == 0;
}

/* The scheduler is the one state execve deliberately does not reset. Returning
 * false matters: a daemon whose main is running at a render thread's real-time
 * policy may fail in a way that looks like a bad unit, and the switch is better
 * off refused and logged. */
bool ResetThread(const ThreadState* t, char* why, size_t n) {
  struct sched_param zero;
  memset(&zero, 0, sizeof zero);
  if (sched_setscheduler(0, SCHED_OTHER, &zero) != 0) {
    snprintf(why, n, "could not leave the inherited scheduler %s (%d): %s (%d)",
             PolicyName(t->policy), POLICY_NUMBER(t->policy), strerror(errno),
             errno);
    return false;
  }

  size_t used = 0;
  why[0] = '\0';
  char one[96];
#define WARN(...)                                                          \
  do {                                                                     \
    snprintf(one, sizeof one, __VA_ARGS__);                                \
    const int wrote = snprintf(why + used, n - used, "%s%s",               \
                               used == 0 ? "" : "; ", one);                \
    if (wrote > 0) {                                                       \
      used += static_cast<size_t>(wrote);                                  \
      if (used >= n) used = n - 1;                                         \
    }                                                                      \
  } while (0)

  if (setpriority(PRIO_PROCESS, 0, 0) != 0) WARN("nice was not reset");
  sigset_t none;
  sigemptyset(&none);
  if (pthread_sigmask(SIG_SETMASK, &none, nullptr) != 0) {
    WARN("the signal mask was not reset");
  }
  const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpus > 0) {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (long i = 0; i < cpus && i < CPU_SETSIZE; ++i) CPU_SET(i, &all);
    if (sched_setaffinity(0, sizeof all, &all) != 0) {
      WARN("CPU affinity was not reset");
    }
  }
#undef WARN
  return true;
}

void RestoreThread(const ThreadState* t) {
  if (t->policy >= 0) {
    sched_setscheduler(0, t->policy, &t->param);
  }
  if (t->have_nice) setpriority(PRIO_PROCESS, 0, t->nice);
  if (t->have_mask) pthread_sigmask(SIG_SETMASK, &t->mask, nullptr);
  if (t->have_affinity) sched_setaffinity(0, sizeof t->affinity, &t->affinity);
}

/* ---- the switch ---------------------------------------------------------- */

bool Switch(char* why, size_t n) {
  char storage[4096];
  char* argv[kArgCap];
  if (!ReadArgv(storage, sizeof storage, argv, kArgCap)) {
    snprintf(why, n, "could not read %s", BORG_CMDLINE);
    return false;
  }

  const size_t blob_len =
      static_cast<size_t>(shim_blob_end - shim_blob_start);
  if (blob_len == 0) {
    snprintf(why, n, "the unit was built without its shim");
    return false;
  }

  /* /tmp first: it is tmpfs, so staging there costs no flash life at all. The
   * userfs is tried only if /tmp cannot hold it, and the log says so, because
   * that is the one write in this unit that touches internal storage. The
   * fallback fires on the *write*, not only on the noexec check: /tmp allowing
   * exec does not make it writable. */
  char tmp_why[192];
  char fs_why[192] = "";
  const char* shim = nullptr;
  if (MountAllowsExec(BORG_TMP_DIR, tmp_why, sizeof tmp_why) &&
      WriteBlob(shim_blob_start, blob_len, BORG_SHIM_TMP, tmp_why,
                sizeof tmp_why)) {
    shim = BORG_SHIM_TMP;
  }
  if (shim == nullptr &&
      MountAllowsExec(BORG_USERFS_DIR, fs_why, sizeof fs_why) &&
      WriteBlob(shim_blob_start, blob_len, BORG_SHIM_USERFS, fs_why,
                sizeof fs_why)) {
    shim = BORG_SHIM_USERFS;
  }
  if (shim == nullptr) {
    snprintf(why, n, "no place for the shim: %s; %s", tmp_why,
             fs_why[0] != '\0' ? fs_why : "the userfs was not tried");
    return false;
  }

  char write_why[192];
  if (!LooksLikeArmShim(shim, write_why, sizeof write_why)) {
    unlink(shim);
    snprintf(why, n, "%s", write_why);
    return false;
  }

  char* envp[kEnvCap];
  if (!BuildEnv(envp, kEnvCap, shim)) {
    unlink(shim);
    snprintf(why, n, "the environment is too large to carry across");
    return false;
  }

  ThreadState thread;
  ReadThread(&thread);
  char warn[200];
  if (!ResetThread(&thread, warn, sizeof warn)) {
    unlink(shim);
    snprintf(why, n, "%s", warn);
    return false;
  }

  Say("=== borg: exec %s pid %ld", BORG_SELF, static_cast<long>(getpid()));
  Say("    scheduler was %s (%d) priority %d", PolicyName(thread.policy),
      POLICY_NUMBER(thread.policy), thread.param.sched_priority);
  Say("    %s", warn[0] != '\0' ? warn : "scheduler reset, no further refusal");
  Say("    shim %s (%lu bytes)%s", shim, static_cast<unsigned long>(blob_len),
      strcmp(shim, BORG_SHIM_TMP) == 0 ? " (tmpfs)" : " (USERFS: one flash write)");
  Say("    shim log %s", BORG_SHIM_LOG);

  /* The new image inherits stdout and stderr and nothing else above fd 2. */
  for (int fd = 3; fd < kFdSweep; ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }

  fflush(nullptr);
  execve(BORG_SELF, argv, envp);

  const int err = errno;
  RestoreThread(&thread);
  snprintf(why, n, "execve(%s): %s (%d)", BORG_SELF, strerror(err), err);
  return false;
}

/* ---- the panel ----------------------------------------------------------- */

/* TWO THINGS ON THIS PANEL, AND NEITHER IS A POSITIONED DRAW.
 *
 * The id-0 text frame (`85 40 00 ...`) is not a positioned draw while
 * microkorgd runs. The panel's dispatcher only draws it when an external state
 * check says the app UI is not up; with the daemon running the frame falls
 * through to a console print that takes the whole display. Two markers at y=6
 * and y=100 (the screen is 64 pixels tall) both appeared, in order, which is
 * what settled it on the board. That is where the menu is written.
 *
 * The daemon's field-text message,
 *
 *     85 <len> 36 <field> <count> <text>
 *
 * fills one cell of the soft-key row under the display. The row has five
 * fields, and microkorgd writes INIT/COPY/MANUAL into the first three of them.
 * A label written there is one of five cells and nothing more, which is why the
 * menu's body is the console and this row only names the keys. */
constexpr unsigned kLabelFields = BORG_API_LABELS;   /* five, the row's width */
constexpr size_t kLabelMax = 12;
constexpr uint8_t kIdButtonLabel = 0x36;

/* THE BANNER, WHICH IS THE CONSOLE. It is kept because it is the only way a
 * message appears at all, and the cost of it is stated here rather than hidden:
 * the panel prints the string as a line of its text console, the console takes
 * the display until microkorgd draws again, and its top row is not repainted by
 * that. The layout below is still the frame the firmware decodes (x at bytes
 * 9-12, string from byte 19); what it does NOT do is position the text. */
constexpr size_t kFrame = 64;
constexpr size_t kTextAt = 19;
constexpr size_t kTextMax = kFrame - kTextAt;
constexpr uint8_t kFont = 1;
constexpr unsigned kCell = 7;
constexpr unsigned kScreenW = 320;
constexpr unsigned kRow0 = 26;
constexpr unsigned kRow1 = 46;

struct Row {
  unsigned x;
  unsigned y;
  size_t n;
};

Row g_rows[2] = {{0, 0, 0}, {0, 0, 0}};

/* ONE FRAME, ONE WRITE. Every writer known to work on this panel hands it a
 * single frame per write; a 90-byte write carrying a fill and two text boxes
 * came out six characters short, whatever the string, so the burst is removed
 * rather than explained.
 *
 * The descriptor is opened non-blocking and a full port is waited on with a
 * budget. A blocking open would park the audio thread here for as long as the
 * daemon keeps the port busy, and every caller of this runs on a thread the
 * daemon cannot afford to lose.
 *
 * FIFTY MILLISECONDS, NOT TWO HUNDRED. The panel-write hook calls the unit from
 * the daemon's panel thread (the menu is drawn there), so this is the one place
 * that thread can be parked. A 21-byte frame does not need more. */
bool SendFrame(const char* frame, size_t n) {
  if (n == 0) return false;
  const int fd = open(BORG_PANEL_PATH,
                      O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (!g_panel_warned) {
      g_panel_warned = true;
      Say("panel: %s: %s (%d)", BORG_PANEL_PATH, strerror(errno), errno);
    }
    return false;
  }

  size_t off = 0;
  int budget_ms = 50;
  while (off < n) {
    const ssize_t wrote = write(fd, frame + off, n - off);
    if (wrote > 0) {
      off += static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) continue;
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        budget_ms > 0) {
      struct pollfd p;
      p.fd = fd;
      p.events = POLLOUT;
      p.revents = 0;
      const int ms = budget_ms > 20 ? 20 : budget_ms;
      const int ready = poll(&p, 1, ms);
      budget_ms -= ms;
      if (ready > 0) continue;
      if (ready == 0) continue;      /* the budget decides, not one wait */
    }
    Say("panel: %u of %u bytes written, then %s (%d)",
        static_cast<unsigned>(off), static_cast<unsigned>(n), strerror(errno),
        errno);
    close(fd);
    return false;
  }
  close(fd);
  return true;
}

/* Write one soft-key label. The frame is microkorgd's own: class 0x85, the
 * whole frame length in byte 1, id 0x36, the field, the character count, then
 * the text, zero-padded to 21 bytes -- which is the frame length the daemon
 * uses whatever the text is, and the one the board accepted when these fields
 * were first written from the display module. */
bool SendLabel(unsigned field, const char* text) {
  if (field >= kLabelFields) return false;
  size_t n = strlen(text);
  if (n > kLabelMax) n = kLabelMax;
  constexpr size_t kLabelFrame = 21;
  char frame[kLabelFrame];
  memset(frame, 0, sizeof frame);
  frame[0] = static_cast<char>(0x85);
  frame[1] = static_cast<char>(kLabelFrame);
  frame[2] = static_cast<char>(kIdButtonLabel);
  frame[3] = static_cast<char>(field);
  frame[4] = static_cast<char>(n);
  memcpy(frame + 5, text, n);
  return SendFrame(frame, kLabelFrame);
}

void BuildTextFrame(char* out, unsigned x, unsigned y, const char* text) {
  memset(out, 0, kFrame);
  out[0] = static_cast<char>(0x85);
  out[1] = static_cast<char>(kFrame);
  out[2] = 0;                             /* id 0: text */
  out[3] = static_cast<char>(kFont);
  out[4] = 0x0e;
  out[5] = 0x00;
  out[6] = 0x7b;
  out[7] = 0x7c;
  out[8] = 0x7f;
  out[9] = static_cast<char>(x & 0x7f);
  out[10] = static_cast<char>((x >> 7) & 0x7f);
  out[11] = static_cast<char>(y & 0x7f);
  out[12] = static_cast<char>((y >> 7) & 0x7f);
  out[13] = 0x20;
  out[14] = 0x01;
  out[15] = 0x0f;
  out[16] = 0x00;
  out[17] = 0x00;
  out[18] = 0x01;
  size_t n = strlen(text);
  if (n > kTextMax) n = kTextMax;
  memcpy(out + kTextAt, text, n);
}

void DrawString(const char* text, unsigned x, unsigned y) {
  char frame[kFrame];
  BuildTextFrame(frame, x, y, text);
  SendFrame(frame, kFrame);
}

/* ---- the console ---------------------------------------------------------- */

/* THE MENU'S BODY IS CONSOLE TEXT. The panel's screen is a fixed set of fields:
 * a 0x34 name written for slot 0 appears in the first parameter slot and
 * nowhere else, so no frame places text at a free position while the app UI is
 * up. Id 0 is the exception. With microkorgd running the panel does not
 * position it: the frame prints a line that takes the whole 64-pixel display,
 * y is ignored, and each write appends one more line.
 *
 * The console cannot be cleared from here: spaces append a line rather than
 * erasing one. It is left by a view change, which is what the menu's close
 * relies on. */
void ConsoleSay(const char* text) { DrawString(text, 0, 0); }

/* The same cells, drawn over with spaces. Whether the panel paints a space
 * cell's background is not established; on the board it is what takes a
 * message down, and the display module is the bench for it. */
void ClearRow(Row* row) {
  if (row->n == 0) return;
  char blanks[kTextMax + 1];
  memset(blanks, ' ', row->n);
  blanks[row->n] = '\0';
  DrawString(blanks, row->x, row->y);
  row->n = 0;
}

void PaintRow(Row* row, const char* text, unsigned y) {
  ClearRow(row);
  size_t n = strlen(text);
  if (n > kTextMax) n = kTextMax;
  const unsigned x = kScreenW / 2U - static_cast<unsigned>(kCell * n) / 2U;
  DrawString(text, x, y);
  row->x = x;
  row->y = y;
  row->n = n;
}

bool PaintRows(const char* line1, const char* line2) {
  PaintRow(&g_rows[0], line1, kRow0);
  PaintRow(&g_rows[1], line2, kRow1);
  return true;
}

/* ---- the banner ---------------------------------------------------------- */

/* THE BANNER IS THE CONSOLE, AND THAT IS ACCEPTED. The ready banner is painted
 * once, late, on the ready path. Nothing erases it; a later view change leaves
 * console mode, and until then the STATE parameter is the readout.
 *
 * The switch banner is the same console, and it cannot be taken down: the image
 * that draws it is replaced by execve(2) before it could clear it. It is drawn
 * anyway, because seeing "BORG SWITCHING" is how the player knows the key was
 * the one the unit acted on, and a switch that gives no sign leaves them
 * pressing C again. Do not try to clear either one with spaces. */
constexpr const char* kReadyTitle = "Borg for microKORG2";
constexpr const char* kReadyBody = "PLAY ANY C";
constexpr const char* kSwitchTitle = "BORG SWITCHING";
constexpr const char* kSwitchBody = "PLEASE WAIT";

bool Paint() {
  if (g_phase == kActive || g_phase == kFailed) return false;
  return PaintRows(kReadyTitle, kReadyBody);
}

/* One paint per load, a quarter second in: the daemon's page burst for a fresh
 * OSC page arrives in the first frames, and a console line drawn into it is
 * appended to that burst rather than merged with it. */
void SchedulePaint() {
  g_paint_due = true;
  g_paint_at = g_frames + static_cast<uint64_t>(kPaintDelayMs) * g_rate / 1000U;
}

/* ---- the menu ------------------------------------------------------------ */

/* FIVE BUTTONS, AND THE KEYS UNDER THEM. Opening the menu writes five labels
 * into the panel's button row and raises the flag the shim reads, so FUNCTION
 * 1-5 are kept away from microkorgd and handed here instead. Closing it lowers
 * the flag and lets the daemon repaint.
 *
 * THE MENU IS DRAWN OVER A DAEMON SCREEN, AND THAT IS THE POINT. The panel's
 * console -- the only free-form text a unit gets -- cannot be cleared from
 * here: spaces append a line. What clears it is a view change, so the chord's
 * second key is rewritten by the shim to the daemon's COPY screen, the menu is
 * drawn after that screen has gone up, and FUNCTION 5 on close is forwarded to
 * the daemon as COPY's BACK. The daemon's repaint restores the display, and
 * nothing of ours is left behind. DRAWING THE MENU IS THEREFORE NOT DONE FROM
 * THE READ THAT CARRIED THE KEY: see MenuChord below.
 *
 * THE SHIM CALLS IN, NOT THE OTHER WAY ROUND. This section used to be driven
 * from the render callback, polling a file at 12 Hz. On the board that lost the
 * chord every single time, while the shim's key log showed it seeing and
 * withholding that same press seven times over: a level sampled by two
 * independent clocks misses an event as short as a button press. The shim now
 * calls MenuChord() and MenuKey(), so nothing here depends on the render
 * callback running at all.
 *
 * The menu itself is written to the console (ConsoleSay), not into the button
 * row: the row is a fixed set of cells and a label in it is all it can ever
 * be, so the row names the keys and the console carries the text. */

/* FIVE FIELDS, AND THE FIFTH IS CLOSE. The row is five wide -- the daemon
 * leaves field 4 empty on the OSC page -- and CLOSE sits there, under
 * FUNCTION 5, rather than in field 3. Field 3 stays blank: the shim rewrites
 * it to an empty label while the menu is open, so the daemon's own text cannot
 * show through a menu that does not own that button. */
constexpr const char* kMenuLabels[kLabelFields] = {"MODS", "SELF", "LOG", "",
                                                   "CLOSE"};
constexpr uint32_t kNoteHoldMs = 4000;

/* The shim's copy of the daemon's own labels, filled through the registered
 * pointer (borg_api.h). The sizes are the struct's, not this file's, so the two
 * objects cannot disagree about them. */
char g_saved[BORG_API_LABELS][BORG_API_LABEL_MAX];
unsigned char g_saved_ok[BORG_API_LABELS];

/* THE FLAG THE SHIM READS. Written here, read in another translation unit (and
 * through another object entirely), so it is volatile -- for the compiler, not
 * for the CPU: both accesses are on the shim's panel thread. */
volatile int g_menu_open = 0;            /* the menu is ours and shown */

/* The struct the shim keeps a pointer to. It lives here, in the unit, so a shim
 * outliving a unit is not possible -- the unit clears the pointer in its
 * teardown. */
borg_unit_api g_api;

char g_note[kLabelMax * 3 + 1];
uint64_t g_note_until = 0;               /* CLOCK_MONOTONIC, milliseconds */

/* The monotonic clock, not rendered frames. Everything timed here -- how long
 * an action's answer stays in STATE -- has to expire whether or not the render
 * callback is running. */
uint64_t NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000U +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000U;
}

void SendMenuLabels() {
  for (unsigned i = 0; i < kLabelFields; ++i) SendLabel(i, kMenuLabels[i]);
}

/* CONSOLE FIRST, LABELS SECOND. The console takes the whole display and the
 * button row is a separate layer over it -- but the row survives only if it is
 * written after the console line that shares that write. The USB storage screen
 * is the proof that the layers coexist: it prints its own body and still labels
 * a soft key `EXIT`. Opening the menu the other way round (labels, then
 * console) is what left the row blank on the board. Every console line this
 * unit writes is therefore followed by the row. */
void MenuConsole(const char* text) {
  ConsoleSay(text);
  SendMenuLabels();
}

/* The key ids are FUNCTION 1-5, i.e. 0x0d-0x11; the button row is the first
 * four. */
void DoModules() {
  DIR* dir = opendir(BORG_USERFS_DIR);
  if (dir == nullptr) {
    snprintf(g_note, sizeof g_note, "mods: %s", strerror(errno));
    return;
  }
  unsigned count = 0;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    const size_t len = strlen(ent->d_name);
    if (len > 11 && strcmp(ent->d_name + len - 11, ".borgmodule") == 0) ++count;
  }
  closedir(dir);
  snprintf(g_note, sizeof g_note, "mods %u", count);
}

void DoSelf() {
  /* Cleared first: StateText() answers with g_note itself while a note is
   * live, and formatting into the buffer being read from is not defined. */
  g_note[0] = '\0';
  snprintf(g_note, sizeof g_note, "pid %d %s", static_cast<int>(getpid()),
           StateText());
}

/* The last line of this unit's own log, truncated: the panel has room for a
 * few characters and the log is where the detail already is. */
void DoLog() {
  const int fd = open(BORG_LOG_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    snprintf(g_note, sizeof g_note, "nolog");
    return;
  }
  char buf[1024];
  const ssize_t got = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (got <= 0) {
    snprintf(g_note, sizeof g_note, "empty");
    return;
  }
  buf[got] = '\0';
  if (buf[got - 1] == '\n') buf[got - 1] = '\0';
  const char* last = strrchr(buf, '\n');
  if (last != nullptr && last[1] != '\0') ++last;
  else if (last != nullptr) last = buf;
  snprintf(g_note, sizeof g_note, "%s", last);
}

void CloseMenu() {
  if (!g_menu_open) return;
  /* THE SHIM HAS ALREADY FORWARDED THE FUNCTION 5 THAT GOT US HERE, so
   * microkorgd is about to cancel the COPY screen this menu was drawn over and
   * repaint the oscillator page -- and that repaint is what takes the console
   * line and the menu's labels off the display. Nothing here can clear the
   * console; the daemon's page change is the only thing that does.
   *
   * The flag goes down first, so the shim stops rewriting the row from this
   * moment on and the daemon's own labels go back with its page.
   *
   * The restore below is the fallback for a daemon that does not repaint: it
   * writes the row the shim last saw before the chord -- captured before the
   * open, so it is the oscillator page's row and not COPY's. A field the shim
   * never saw is left alone; a label guessed at is worse than one left blank. */
  g_menu_open = 0;
  ConsoleSay("BORG CLOSED");
  /* What the shim captured is written into the log, not just the panel: "the
   * row did not come back" has two causes -- nothing was captured, or it was
   * captured and not written -- and they are indistinguishable from the panel
   * alone. One line per close settles which. */
  Say("menu: closed, captured %u%u%u%u%u", g_saved_ok[0], g_saved_ok[1],
      g_saved_ok[2], g_saved_ok[3], g_saved_ok[4]);
  for (unsigned i = 0; i < kLabelFields; ++i) {
    if (g_saved_ok[i] == 0) continue;
    Say("menu: restore %u [%s]", i, g_saved[i]);
    if (g_saved[i][0] != '\0') SendLabel(i, g_saved[i]);
  }
}

void OpenMenu() {
  if (g_menu_open) return;
  g_menu_open = 1;                 /* from here the shim withholds FUNCTION 1-5 */
  MenuConsole("BORG MENU");
  g_note[0] = '\0';
  /* The frame count is in this line on purpose. "Does the render callback run
   * in this image?" is the one question the board could not answer while the
   * menu was driven by a polled file; this says it outright. */
  Say("menu: open, %s (%lu frames rendered)", kMenuLabels[0],
      static_cast<unsigned long>(g_frames));
}

void AtMenuKey(unsigned key) {
  switch (key) {
    case 0x0d: DoModules(); break;   /* field 0 */
    case 0x0e: DoSelf(); break;      /* field 1 */
    case 0x0f: DoLog(); break;       /* field 2 */
    case 0x10: return;               /* field 3 is blank and does nothing */
    case 0x11: CloseMenu(); return;  /* field 4 */
    default: return;
  }
  g_note_until = NowMs() + kNoteHoldMs;
  /* The answer goes to the console as a new line: the menu is a scroll, not a
   * frame with a value cell in it. The STATE note stays as well, because it is
   * readable when the console has been dismissed with an encoder. */
  char line[80];                   /* a note is 48 at most; the panel shows 45 */
  snprintf(line, sizeof line, "> %s", g_note);
  MenuConsole(line);
  Say("menu: key %02x -> %s", key, g_note);
}

/* ---- the states ---------------------------------------------------------- */

/* THE SHIM'S TWO CALLS. MenuChord is the chord, and the shim calls it from its
 * panel-write hook -- once microkorgd has painted the page the chord's second
 * key opened underneath. That is why only the open arrives here: the close is
 * FUNCTION 5, forwarded to the daemon as the BACK/CANCEL of the view the menu is
 * drawn over, and it reaches this file as MenuKey. The daemon's own repaint is
 * what takes the menu off the display. */
void MenuChord(int down) {
  if (down == 0) return;
  OpenMenu();
}

void MenuKey(unsigned id) {
  if (!g_menu_open) return;
  AtMenuKey(id);
}

/* ---- registration -------------------------------------------------------- */

/* Hand the shim this object's side of the link. dlsym(RTLD_DEFAULT) finds the
 * preloaded object because the daemon was started with it in LD_PRELOAD, which
 * puts it in the global scope; the unit is dlopen'd into that process after.
 * The call is made at Init, long after the loader settled -- not from a
 * constructor, which is what the shim's own note warns about.
 *
 * Returns false when there is no shim, which is the normal case before the
 * switch: the unit is then simply a unit with no menu. */
bool RegisterWithShim() {
  g_api.version = BORG_API_VERSION;
  g_api.menu_open = &g_menu_open;
  g_api.on_chord = &MenuChord;
  g_api.on_key = &MenuKey;
  g_api.labels = g_saved;
  g_api.label_known = g_saved_ok;
  for (unsigned i = 0; i < kLabelFields; ++i) g_api.menu_labels[i] = kMenuLabels[i];

  void* sym = dlsym(RTLD_DEFAULT, "borg_shim_register");
  if (sym == nullptr) {
    Say("shim: no borg_shim_register in the global scope; no menu");
    return false;
  }
  const borg_register_fn reg = reinterpret_cast<borg_register_fn>(sym);
  const int took = reg(&g_api);
  if (took != 1) {
    Say("shim: register refused (api %d)", g_api.version);
    return false;
  }
  Say("shim: registered, menu on FUNCTION 3 + FUNCTION 5");
  return true;
}

/* True once per press of any C. The context's trigger byte is a bit per voice
 * and is a level, not a pulse: it stays set while the voice sounds, so a
 * request is the rising edge of any bit whose voice is playing the note.
 * `pitch[]` is the MIDI note as a float; rounding to the nearest whole number is
 * the note the key sent, and its pitch class is what the octave switch does not
 * change. */
bool ActivationRequested(const void* hooks_context, int* out_note) {
  if (hooks_context == nullptr) return false;
  const unit_runtime_osc_context_t* context =
      static_cast<const unit_runtime_osc_context_t*>(hooks_context);

  const uint8_t trigger = context->trigger;
  const uint8_t fresh = static_cast<uint8_t>(trigger & ~g_prev_trigger);
  g_prev_trigger = trigger;
  if (fresh == 0) return false;

  const int voices = context->voiceLimit > 8 ? 8 : context->voiceLimit;
  for (int v = 0; v < voices; ++v) {
    if ((fresh & (1u << v)) == 0) continue;
    const int note = static_cast<int>(context->pitch[v] + 0.5f);
    /* Written as a pitch class, not a list of notes: the keyboard's OCTAVE
     * SHIFT sends a different note number for the same key, and the player
     * presses "a C", not "MIDI 48". */
    if (note % 12 == kActivatePitchClass) {
      if (out_note != nullptr) *out_note = note;
      return true;
    }
  }
  return false;
}

}  // namespace

void Init(uint32_t samplerate) {
  if (samplerate >= 8000 && samplerate <= 192000) g_rate = samplerate;

  /* A menu that was open when the previous image execve'd must not leave the
   * shim swallowing FUNCTION 1-5 for the rest of the session: this image has no
   * menu, and the flag it registers says so from the first key frame. */
  g_menu_open = 0;

  char me[32];
  snprintf(me, sizeof me, "%d", static_cast<int>(getpid()));

  /* The shim marks the daemon it attached to: LOGUE_BORG_ACTIVE=1 and
   * LOGUE_BORG_PID naming that process. Both are checked, as borg_drumlogue
   * does -- a child inherits the variables but is not the marked process, and a
   * stale marker in a daemon with no shim must not report ACTIVE and swallow
   * the note. */
  const char* active = getenv("LOGUE_BORG_ACTIVE");
  const char* marked = getenv("LOGUE_BORG_PID");
  g_phase = kReady;
  if (active != nullptr && active[0] == '1') {
    if (marked != nullptr && strcmp(marked, me) == 0) {
      g_phase = kActive;
      Say("--- borg: running under the shim, pid %s", marked);
      /* The shim is in this process, so the menu has a trigger from here on.
       * Registration is what makes the chord a call instead of a polled file;
       * without it (a marker but no shim, which the check above makes
       * impossible) the unit simply has no menu. */
      RegisterWithShim();
      /* NO PANEL WHEN ACTIVE. This runs every time the OSC page is opened, so
       * a paint here put "BORG ACTIVE / EXTENSIONS ON" back on the instrument
       * on every visit -- the panel is for the start and for the key
       * combination, and once the shim is in place the STATE parameter
       * (`!active`) is the whole readout. Do not re-add a paint on this
       * path. */
      return;
    }
    Say("--- borg: marker ignored: attached pid %s, this pid %s",
        marked != nullptr ? marked : "(unset)", me);
  }

  /* Ready. The switch is not asked for here; see Render. The banner is
   * scheduled rather than painted from Init -- Init runs inside the daemon's
   * load, before its page burst has gone out, and the console appends rather
   * than repaints (see the banner section). */
  g_phase = kReady;
  SchedulePaint();
  Say("--- borg: ready, pid %d; play any C to activate",
      static_cast<int>(getpid()));
}

/* The unit is being unloaded -- a program change away from the one whose OSC
 * this is. The shim holds a pointer into this image, so it has to be told
 * before the image can go: a chord on a later program would otherwise call
 * into code that is no longer mapped. */
void Teardown() {
  g_menu_open = 0;
  void* sym = dlsym(RTLD_DEFAULT, "borg_shim_unregister");
  if (sym == nullptr) return;
  const borg_unregister_fn un = reinterpret_cast<borg_unregister_fn>(sym);
  un(&g_api);
}

void Render(float* out, uint32_t frames, const void* context) {
  for (uint32_t i = 0; i < frames * 8U; ++i) out[i] = 0.0f;

  /* The ready banner, once, after the daemon's page burst has gone out. */
  if (g_paint_due && g_frames >= g_paint_at) {
    g_paint_due = false;
    Paint();
  }

  /* The request. Nothing here drives the menu any more: the shim calls in from
   * the key it read, so a program whose render callback never runs still has a
   * working menu. A successful Switch never returns; a refused one
   * sets kFailed and the note has to be released and played again, which the
   * trigger edge above enforces. */
  int note = -1;
  if (g_phase != kActive && ActivationRequested(context, &note)) {
    Say("=== borg: C (MIDI %d), pid %d", note, static_cast<int>(getpid()));
    g_phase = kSwitching;
    /* A menu left open over the restart would be four labels the returning
     * image never restores, because the shim's record of the daemon's labels
     * only covers what the daemon itself drew. Take it down first. */
    CloseMenu();
    /* Drawn here and not scheduled: the process is about to be replaced, so
     * this is the last frame this image will ever send. See the note on the
     * switch banner -- it is not taken down. */
    PaintRows(kSwitchTitle, kSwitchBody);
    char why[320];
    if (!Switch(why, sizeof why)) {
      g_phase = kFailed;
      Say("    switch refused: %s", why);
      Say("    the unit stays loaded; play any C again to retry");
    }
  }

  g_frames += frames;
}

/* borg_drumlogue's tags, character for character: the STATE readout is the
 * comparison a person actually makes between the two instruments. */
const char* StateText() {
  /* A menu action's answer replaces the tag for a few seconds. The OSC page is
   * the only place a unit can put text on this instrument without owning the
   * display, so that is where the result goes. */
  if (g_note[0] != '\0' && NowMs() < g_note_until) return g_note;
  switch (g_phase) {
    case kSwitching: return "!switch";
    case kActive: return "!active";
    case kFailed: return "!err";
    case kReady:
    default: return "!ready";
  }
}

}  // namespace borg
