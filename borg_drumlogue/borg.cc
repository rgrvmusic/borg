// Panel UI and in-place daemon switch.

#include "borg.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

extern char** environ;

namespace borg {
namespace {

// Paths can be overridden by native tests.

#ifndef BORG_PANEL_PATH
#define BORG_PANEL_PATH "/dev/ttymxc2"
#endif
#ifndef BORG_SELF
#define BORG_SELF "/proc/self/exe"
#endif
#ifndef BORG_SHIM
#define BORG_SHIM "/tmp/libborg.so"
#endif
#ifndef BORG_EXEC_LOG
#define BORG_EXEC_LOG "/var/lib/drumlogued/userfs/Programs/borg-exec.log"
#endif
#ifndef BORG_UNIT_LOG
#define BORG_UNIT_LOG "/tmp/logue-borg-unit.log"
#endif

/* Bound staging work on the render thread. */
constexpr size_t kMaxBlobBytes = 1024 * 1024;

/* Refuse oversized argv or environments rather than truncate them. */
constexpr int kEnvCap = 128;
constexpr int kArgvCap = 32;
constexpr size_t kPreloadBytes = 512;
constexpr size_t kCmdlineBytes = 4096;

/* Descriptors above this limit are not included in the close-on-exec sweep. */
constexpr int kFdSweep = 1024;

/* Panel text frame limits. */
constexpr uint8_t kScreenClass = 0x85;
constexpr int kScreenHeader = 10;
constexpr size_t kScreenTextMax = 51;
constexpr size_t kScreenFrameMax = 3 + kScreenHeader + kScreenTextMax;

/* Panel box roles. */
constexpr uint8_t kTitleRole = 0x20; /* role 0, style 1 */
constexpr uint8_t kBodyRole = 0x22;  /* role 2, style 1 */
constexpr uint8_t kFg = 15;
constexpr uint8_t kBg = 0;

enum Phase { kReady, kSwitching, kActive, kFailed };

Phase g_phase = kReady;
uint32_t g_rate = 48000;
uint64_t g_frames = 0;

/* Written by MIDI callbacks and consumed by render. */
volatile int g_request = 0;

/* Redraw after the daemon repaints its screen. */
constexpr uint64_t kRedrawMs[] = {0, 250, 1000, 3000};
constexpr int kRedraws = static_cast<int>(sizeof kRedrawMs / sizeof kRedrawMs[0]);
int g_redraw = 0;
uint64_t g_redraw_at = 0;

const char* PhaseTitle() {
  switch (g_phase) {
    case kSwitching: return "BORG SWITCHING";
    case kActive:    return "BORG ACTIVE";
    case kFailed:    return "BORG FAILED";
    case kReady:
    default:         return "BORG READY";
  }
}

const char* PhaseBody() {
  switch (g_phase) {
    case kSwitching: return "LOADING EXTENSIONS";
    case kActive:    return "EXTENSIONS LOADED";
    case kFailed:    return "NOTHING CHANGED";
    case kReady:
    default:         return "PRESS A PAD";
  }
}

void WriteAll(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    const ssize_t wrote = write(fd, p, len);
    if (wrote > 0) {
      p += wrote;
      len -= static_cast<size_t>(wrote);
      continue;
    }
    if (wrote < 0 && errno == EINTR) continue;
    return;
  }
}

void AppendLog(const char* path, const char* line) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return;
  WriteAll(fd, line, strlen(line));
  close(fd);
}

void LogUnit(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  AppendLog(BORG_UNIT_LOG, line);
}

/* Build one class-0x85 text frame. */
size_t BuildTextFrame(uint8_t* out, size_t cap, uint8_t role, unsigned x,
                      unsigned y, unsigned w, unsigned h, uint8_t kind,
                      const char* text) {
  size_t n = strlen(text);
  if (n > kScreenTextMax) n = kScreenTextMax;
  const size_t len = 3 + kScreenHeader + n;
  if (cap < len) return 0;

  out[0] = kScreenClass;
  out[1] = static_cast<uint8_t>(len);
  out[2] = 0x00; /* id 0: screen text */
  out[3] = role;
  out[4] = kFg;
  out[5] = kBg;
  out[6] = static_cast<uint8_t>(x & 0x7f);
  out[7] = static_cast<uint8_t>((x >> 7) & 0x7f);
  out[8] = static_cast<uint8_t>(y & 0xff);
  out[9] = static_cast<uint8_t>(w & 0x7f);
  out[10] = static_cast<uint8_t>((w >> 7) & 0x7f);
  out[11] = static_cast<uint8_t>(h & 0xff);
  out[12] = kind;
  memcpy(out + 3 + kScreenHeader, text, n);
  return len;
}

void DrawScreen() {
  uint8_t title[kScreenFrameMax];
  uint8_t body[kScreenFrameMax];
  const size_t title_len =
      BuildTextFrame(title, sizeof title, kTitleRole, 64, 0, 128, 8, 0,
                     PhaseTitle());
  const size_t body_len =
      BuildTextFrame(body, sizeof body, kBodyRole, 0, 36, 256, 24, 0,
                     PhaseBody());
  if (title_len == 0 || body_len == 0) return;

  /* Avoid blocking on the panel tty; truncate file-backed test output. */
  const int fd = open(BORG_PANEL_PATH,
                      O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_TRUNC);
  if (fd < 0) {
    LogUnit("borg: open %s: %s (%d)\n", BORG_PANEL_PATH, strerror(errno),
            errno);
    return;
  }
  WriteAll(fd, title, title_len);
  WriteAll(fd, body, body_len);
  close(fd);
}

void RedrawNow() {
  g_redraw = 0;
  g_redraw_at = g_frames;
}

extern "C" {
extern const unsigned char borg_blob_start[];
extern const unsigned char borg_blob_end[];
}

bool WriteBlob(const unsigned char* data, size_t len, const char* dst, char* why,
               size_t n) {
  if (len == 0) {
    snprintf(why, n, "this unit carries no resident object");
    return false;
  }
  if (len > kMaxBlobBytes) {
    snprintf(why, n, "the resident object is %lu bytes; the limit is %lu",
             static_cast<unsigned long>(len),
             static_cast<unsigned long>(kMaxBlobBytes));
    return false;
  }

  const int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    snprintf(why, n, "%s: %s (%d)", dst, strerror(errno), errno);
    return false;
  }
  bool ok = true;
  size_t off = 0;
  while (off < len) {
    const ssize_t wrote = write(out, data + off, len - off);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      snprintf(why, n, "writing %s: %s (%d)", dst, strerror(errno), errno);
      ok = false;
      break;
    }
    off += static_cast<size_t>(wrote);
  }
  if (close(out) != 0 && ok) {
    snprintf(why, n, "writing %s: %s (%d)", dst, strerror(errno), errno);
    ok = false;
  }
  if (!ok) unlink(dst);
  return ok;
}

/* Validate the staged object before exec, which does not validate LD_PRELOAD. */
bool LooksLikeArmShim(const char* path, char* why, size_t n) {
  unsigned char h[EI_NIDENT + 4];
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    return false;
  }
  const ssize_t got = read(fd, h, sizeof h);
  close(fd);
  if (got < static_cast<ssize_t>(sizeof h)) {
    snprintf(why, n, "%s is too short to be an ELF object", path);
    return false;
  }
  if (memcmp(h, ELFMAG, SELFMAG) != 0) {
    snprintf(why, n, "%s is not an ELF object", path);
    return false;
  }
  if (h[EI_CLASS] != ELFCLASS32) {
    snprintf(why, n, "%s is not a 32-bit object", path);
    return false;
  }
  if (h[EI_DATA] != ELFDATA2LSB) {
    snprintf(why, n, "%s is not little-endian", path);
    return false;
  }
  const unsigned type = static_cast<unsigned>(h[16]) |
                        (static_cast<unsigned>(h[17]) << 8);
  if (type != ET_DYN) {
    snprintf(why, n, "%s is not a shared object (ELF type %u)", path, type);
    return false;
  }
  const unsigned machine = static_cast<unsigned>(h[18]) |
                           (static_cast<unsigned>(h[19]) << 8);
  if (machine != EM_ARM) {
    snprintf(why, n, "%s is not an ARM object (machine %u)", path, machine);
    return false;
  }
  return true;
}

/* The loader cannot map the shim from a noexec filesystem. */
bool ShimMayExecute(const char* path, char* why, size_t n) {
  struct statvfs vfs;
  if (statvfs(path, &vfs) != 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    return false;
  }
  if ((vfs.f_flag & ST_NOEXEC) != 0) {
    snprintf(why, n, "%s is on a filesystem mounted noexec", path);
    return false;
  }
  return true;
}

bool ReadArgv(char** argv, int cap) {
  static char storage[kCmdlineBytes];
  const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t got = read(fd, storage, sizeof storage - 1);
  const int read_errno = errno;
  close(fd);
  if (got <= 0 || got >= static_cast<ssize_t>(sizeof storage) - 1) {
    errno = got <= 0 ? read_errno : E2BIG;
    return false;
  }
  storage[got] = '\0';

  int count = 0;
  size_t off = 0;
  while (off < static_cast<size_t>(got) && count < cap) {
    argv[count++] = storage + off;
    const size_t len = strlen(storage + off);
    off += len + 1;
    if (len == 0) break;
  }
  if (count == 0 || argv[0][0] == '\0') return false;
  argv[count] = nullptr;
  return true;
}

/* Add the shim to LD_PRELOAD and drop its stale status markers. */
bool BuildEnv(char** envp, int cap, char* preload, size_t plen) {
  const char* old = getenv("LD_PRELOAD");
  if (old == nullptr) old = "";

  char value[kPreloadBytes];
  if (old[0] == '\0') {
    snprintf(value, sizeof value, "%s", BORG_SHIM);
  } else if (strstr(old, BORG_SHIM) != nullptr) {
    snprintf(value, sizeof value, "%s", old);
  } else {
    snprintf(value, sizeof value, "%s:%s", BORG_SHIM, old);
  }
  if (value[0] == '\0') return false;

  int count = 0;
  bool had_preload = false;
  for (char** e = environ; *e != nullptr; ++e) {
    if (strncmp(*e, "LD_PRELOAD=", 11) == 0) {
      had_preload = true;
      const int wrote = snprintf(preload, plen, "LD_PRELOAD=%s", value);
      if (wrote < 0 || static_cast<size_t>(wrote) >= plen) return false;
      if (count >= cap) return false;
      envp[count++] = preload;
      continue;
    }
    if (strncmp(*e, "LOGUE_BORG_ACTIVE=", 18) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_SHIM=", 16) == 0) continue;
    if (strncmp(*e, "LOGUE_BORG_PRELOAD=", 19) == 0) continue;
    if (count >= cap) return false;
    envp[count++] = *e;
  }
  if (!had_preload) {
    const int wrote = snprintf(preload, plen, "LD_PRELOAD=%s", value);
    if (wrote < 0 || static_cast<size_t>(wrote) >= plen) return false;
    if (count >= cap) return false;
    envp[count++] = preload;
  }
  envp[count] = nullptr;
  return true;
}

/* execve preserves thread scheduling state; reset it before starting the daemon. */

#if !defined(__NR_sched_getattr) && defined(__arm__)
#define __NR_sched_getattr 381
#endif
#if !defined(__NR_sched_setattr) && defined(__arm__)
#define __NR_sched_setattr 380
#endif

struct SchedAttr {
  uint32_t size;
  uint32_t sched_policy;
  uint64_t sched_flags;
  int32_t sched_nice;
  uint32_t sched_priority;
  uint64_t sched_runtime;
  uint64_t sched_deadline;
  uint64_t sched_period;
  uint32_t sched_util_min;
  uint32_t sched_util_max;
};

struct ThreadState {
  int policy = -1;
  struct sched_param param = {};
  bool have_attr = false;
  SchedAttr attr = {};
  bool have_nice = false;
  int nice = 0;
  bool have_mask = false;
  sigset_t mask = {};
  bool have_affinity = false;
  cpu_set_t affinity = {};
};

const char* SchedName(int policy) {
  if (policy < 0) return "unknown";
#ifdef SCHED_RESET_ON_FORK
  policy &= ~SCHED_RESET_ON_FORK;
#endif
  switch (policy) {
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE: return "deadline";
#else
    case 6: return "deadline";
#endif
    case SCHED_OTHER: return "other";
    case SCHED_FIFO: return "fifo";
    case SCHED_RR: return "rr";
    case SCHED_BATCH: return "batch";
    case SCHED_IDLE: return "idle";
    default: return "unknown";
  }
}

int SchedNumber(int policy) {
  if (policy < 0) return -1;
#ifdef SCHED_RESET_ON_FORK
  policy &= ~SCHED_RESET_ON_FORK;
#endif
  return policy;
}

void ReadThread(ThreadState* t) {
  t->policy = sched_getscheduler(0);
  t->param = sched_param{};
  if (t->policy >= 0 && sched_getparam(0, &t->param) != 0) {
    t->param.sched_priority = 0;
  }
  t->have_attr = false;
  t->attr = SchedAttr{};
#ifdef __NR_sched_getattr
  t->have_attr =
      syscall(__NR_sched_getattr, 0, &t->attr, sizeof t->attr, 0) == 0;
#endif
  t->have_nice = false;
  t->nice = 0;
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

bool ResetThread(const ThreadState& t, char* why, size_t n) {
  struct sched_param zero;
  memset(&zero, 0, sizeof zero);
  if (sched_setscheduler(0, SCHED_OTHER, &zero) != 0) {
    snprintf(why, n,
             "could not leave the inherited scheduler %s (%d): %s (%d)",
             SchedName(t.policy), SchedNumber(t.policy), strerror(errno),
             errno);
    return false;
  }

  size_t used = 0;
  why[0] = '\0';
  auto warn = [&](const char* what) {
    if (used + 1 >= n) return;
    const int wrote = snprintf(why + used, n - used, "%s%s",
                               used == 0 ? "" : "; ", what);
    if (wrote > 0) {
      used += static_cast<size_t>(wrote);
      if (used >= n) used = n - 1;
    }
  };

  if (setpriority(PRIO_PROCESS, 0, 0) != 0) warn("nice was not reset");
  sigset_t none;
  sigemptyset(&none);
  if (pthread_sigmask(SIG_SETMASK, &none, nullptr) != 0) {
    warn("the signal mask was not reset");
  }
  const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpus > 0) {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (long i = 0; i < cpus && i < CPU_SETSIZE; ++i) CPU_SET(i, &all);
    if (sched_setaffinity(0, sizeof all, &all) != 0) {
      warn("CPU affinity was not reset");
    }
  }
  return true;
}

void RestoreThread(const ThreadState& t) {
#ifdef __NR_sched_setattr
  if (t.have_attr && syscall(__NR_sched_setattr, 0, &t.attr, 0) == 0) {
  } else
#endif
  if (t.policy >= 0) {
    sched_setscheduler(0, t.policy, &t.param);
  }
  if (t.have_nice) setpriority(PRIO_PROCESS, 0, t.nice);
  if (t.have_mask) pthread_sigmask(SIG_SETMASK, &t.mask, nullptr);
  if (t.have_affinity) sched_setaffinity(0, sizeof t.affinity, &t.affinity);
}

struct Console {
  int out = -1;
  int err = -1;
  bool out_was_closed = false;
  bool err_was_closed = false;
};

/* Redirect stdout and stderr to the persistent switch log. */
bool ConsoleStart(Console* c, const char* path, const char* first, char* why,
                  size_t n) {
  c->out = fcntl(STDOUT_FILENO, F_DUPFD, 3);
  if (c->out < 0 && errno == EBADF) c->out_was_closed = true;
  c->err = fcntl(STDERR_FILENO, F_DUPFD, 3);
  if (c->err < 0 && errno == EBADF) c->err_was_closed = true;

  const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    snprintf(why, n, "%s: %s (%d)", path, strerror(errno), errno);
    if (c->out >= 0) close(c->out);
    if (c->err >= 0) close(c->err);
    c->out = c->err = -1;
    return false;
  }
  if (first != nullptr) WriteAll(fd, first, strlen(first));
  if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
    snprintf(why, n, "redirecting to %s: %s (%d)", path, strerror(errno),
             errno);
    close(fd);
    if (c->out >= 0) {
      dup2(c->out, STDOUT_FILENO);
      close(c->out);
    }
    if (c->err >= 0) {
      dup2(c->err, STDERR_FILENO);
      close(c->err);
    }
    if (c->out_was_closed) close(STDOUT_FILENO);
    if (c->err_was_closed) close(STDERR_FILENO);
    c->out = c->err = -1;
    return false;
  }
  close(fd);
  return true;
}

void ConsoleStop(Console* c) {
  if (c->out >= 0) {
    dup2(c->out, STDOUT_FILENO);
    close(c->out);
  } else if (c->out_was_closed) {
    close(STDOUT_FILENO);
  }
  if (c->err >= 0) {
    dup2(c->err, STDERR_FILENO);
    close(c->err);
  } else if (c->err_was_closed) {
    close(STDERR_FILENO);
  }
  c->out = c->err = -1;
  c->out_was_closed = c->err_was_closed = false;
}

void Fail(const char* why) {
  g_phase = kFailed;
  RedrawNow();
  LogUnit("borg: switch refused: %s\n", why);
  char line[384];
  snprintf(line, sizeof line, "=== borg unit: switch refused: %s\n", why);
  AppendLog(BORG_EXEC_LOG, line);
  DrawScreen();
  DrawScreen();
}

void Switch() {
  __atomic_store_n(&g_request, 0, __ATOMIC_RELEASE);
  if (g_phase == kActive) return;

  g_phase = kSwitching;
  RedrawNow();
  DrawScreen();

  char why[192];
  why[0] = '\0';

  if (!WriteBlob(borg_blob_start,
                 static_cast<size_t>(borg_blob_end - borg_blob_start), BORG_SHIM,
                 why, sizeof why)) {
    Fail(why);
    return;
  }
  if (!LooksLikeArmShim(BORG_SHIM, why, sizeof why)) {
    Fail(why);
    return;
  }
  if (!ShimMayExecute(BORG_SHIM, why, sizeof why)) {
    Fail(why);
    return;
  }

  char* argv[kArgvCap + 1];
  char* envp[kEnvCap + 1];
  char preload[kPreloadBytes];
  if (!ReadArgv(argv, kArgvCap)) {
    Fail("could not read /proc/self/cmdline");
    return;
  }
  if (!BuildEnv(envp, kEnvCap, preload, sizeof preload)) {
    Fail("the environment is too large to carry across");
    return;
  }

  ThreadState thread;
  ReadThread(&thread);
  char warn[160];
  if (!ResetThread(thread, warn, sizeof warn)) {
    Fail(warn);
    return;
  }

  char line[1024];
  snprintf(line, sizeof line,
           "=== borg unit: exec %s pid %ld\n"
           "    render thread was sched %s (%d) priority %d, nice %d\n"
           "    render thread is now sched other; %s\n"
           "    preload %s\n",
           BORG_SELF, static_cast<long>(getpid()), SchedName(thread.policy),
           SchedNumber(thread.policy), thread.param.sched_priority,
           thread.have_nice ? thread.nice : 0,
           warn[0] != '\0' ? warn : "no further reset was refused", preload);

  fflush(nullptr);
  Console console;
  char log_why[192];
  if (!ConsoleStart(&console, BORG_EXEC_LOG, line, log_why, sizeof log_why)) {
    LogUnit("borg: no persistent switch log (%s); carrying on\n", log_why);
  }

  for (int fd = 3; fd < kFdSweep; ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }

  execve(BORG_SELF, argv, envp);

  const int err = errno;
  ConsoleStop(&console);
  RestoreThread(thread);
  char refused[192];
  snprintf(refused, sizeof refused, "execve(%s): %s (%d)", BORG_SELF,
           strerror(err), err);
  Fail(refused);
}

void SetUp(uint32_t samplerate) {
  g_rate = samplerate ? samplerate : 48000;
  g_frames = 0;
  g_request = 0;

  /* The shim marks the daemon it attached to. A child inherits the variable
   * but is not the marked process, so the pid has to name this one. Without
   * that check a stale marker would report ACTIVE and swallow GateOn in a
   * daemon that has no shim. */
  const char* active = getenv("LOGUE_BORG_ACTIVE");
  const char* marked = getenv("LOGUE_BORG_PID");
  g_phase = kReady;
  if (active != nullptr && active[0] == '1') {
    if (marked != nullptr && atoi(marked) == static_cast<int>(getpid())) {
      g_phase = kActive;
    } else {
      LogUnit("borg: marker ignored: attached pid %s, this pid %ld\n",
              marked != nullptr ? marked : "(unset)",
              static_cast<long>(getpid()));
    }
  }
  RedrawNow();
}

}  // namespace

void Init(uint32_t samplerate) { SetUp(samplerate); }

void Request() {
  /* Allow retry after a refused switch. */
  if (g_phase == kActive || g_phase == kSwitching) return;
  __atomic_store_n(&g_request, 1, __ATOMIC_RELEASE);
}

void Render(float* out, uint32_t frames) {
  for (uint32_t i = 0; i < frames * 2; ++i) out[i] = 0.0f;

  const bool requested = __atomic_load_n(&g_request, __ATOMIC_ACQUIRE) != 0;
  if (requested && g_phase != kActive) {
    Switch();
    return;
  }

  g_frames += frames;
  if (g_redraw < kRedraws && g_frames >= g_redraw_at) {
    DrawScreen();
    ++g_redraw;
    if (g_redraw < kRedraws) {
      g_redraw_at = g_frames + (static_cast<uint64_t>(kRedrawMs[g_redraw]) *
                                g_rate) / 1000;
    }
  }
}

const char* StateTag() {
  switch (g_phase) {
    case kSwitching: return "!switch";
    case kActive:    return "!active";
    case kFailed:    return "!err";
    case kReady:
    default:         return "!ready";
  }
}

}  // namespace borg
