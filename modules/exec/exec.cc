/* exec.borgmodule: rgexec's in-place commands, on the daemon's own MIDI ports.
 *
 * WHY THIS IS A MODULE AND NOT A UNIT. A unit is handed no MIDI-out API, so the
 * old rgexec unit opened a CME interface in the instrument's host port itself.
 * That left the port occupied, needed a dongle, and could only answer on the
 * cable it had taken. borg runs inside drumlogued, which already holds every
 * port it reads and writes. A module that asks for BORG_CAP_MIDI is handed each
 * complete SysEx message from every one of those ports, and a reply goes out on
 * the output paired with the input it arrived on. So the same request works from
 * the gadget, the DIN sockets and the host port, with no second device.
 *
 * WHAT CAN BE ANSWERED HERE. Nothing in this process may create a process or a
 * thread. That leaves the questions rgexec learned to answer without one: the
 * counters behind the refusal (`info`), whether the one call that needs no pid is
 * allowed (`probe`), a file (`cat`), a directory (`ls`), the instrument's storage
 * (`storage`). Anything else is run by the kernel's usermode helper, which is
 * the one route to command execution that survives the refusal: `/proc/sys/` is
 * pointed at a script in /tmp, a uevent is poked, and the helper runs it.
 *
 * THE NAMESPACE. Every request begins `@exec`. A built-in word (`info`, `probe`,
 * `cat`, `ls`, `storage`, `help`) is answered in place; any other word is passed
 * as a command to the kernel's helper. The prefix is what lets several modules
 * share one input without a protocol of their own.
 *
 * THE CALLBACKS ARE SERIALIZED by the resident object: midi_in, tick and
 * page_draw never run at the same time. A module therefore carries no lock, and
 * a slow reply does not race its own answer. Nothing here creates a process or
 * a thread. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "borg_module.h"
#include "wire.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

extern char** environ;

namespace {

using borgwire::kMaxChunk;
using borgwire::kMaxEncoded;
using borgwire::Reader;

/* The paths the commands read. They are the instrument's own; the overrides
 * exist so a test can lay out a scratch tree instead. The device build passes
 * nothing. */
#ifndef RGEXEC_RUN_DIR
#define RGEXEC_RUN_DIR "/tmp"
#endif
#ifndef RGEXEC_HOTPLUG
#define RGEXEC_HOTPLUG "/proc/sys/kernel/hotplug"
#endif
#ifndef RGEXEC_HELPER
#define RGEXEC_HELPER "/sbin/mdev"
#endif
#ifndef RGEXEC_PROBE_DIR
#define RGEXEC_PROBE_DIR "/tmp"
#endif
#ifndef RGEXEC_SYSFS_DIR
#define RGEXEC_SYSFS_DIR "/sys"
#endif
#ifndef RGEXEC_PROC_DIR
#define RGEXEC_PROC_DIR "/proc"
#endif
#ifndef RGEXEC_MOUNTS_PATH
#define RGEXEC_MOUNTS_PATH RGEXEC_PROC_DIR "/mounts"
#endif
#ifndef RGEXEC_DEV_DIR
#define RGEXEC_DEV_DIR "/dev"
#endif
#ifndef RGEXEC_USERFS_PATH
#define RGEXEC_USERFS_PATH "/var/lib/drumlogued/userfs"
#endif
#ifndef RGEXEC_USERFS_MNT
#define RGEXEC_USERFS_MNT "/mnt/userfs"
#endif
#ifndef RGEXEC_USERFS_IMG
#define RGEXEC_USERFS_IMG "/mnt/userfs.img"
#endif
#ifndef RGEXEC_STORAGE_MNT
#define RGEXEC_STORAGE_MNT RGEXEC_PROBE_DIR "/exec-mnt"
#endif
#ifndef RGEXEC_UEVENT
#define RGEXEC_UEVENT RGEXEC_SYSFS_DIR "/class/udc/ci_hdrc.0/uevent"
#endif
#ifndef RGEXEC_UEVENTS
#define RGEXEC_UEVENTS \
  RGEXEC_SYSFS_DIR "/class/sound/card%u/uevent:" RGEXEC_UEVENT
#endif

constexpr int kCommandSeconds = 60;
constexpr int kCatBytes = 2048;
constexpr int kReadBytes = 512;
constexpr int kHotWaitMs = 125;
constexpr int kStatusTimeout = 124;
constexpr int kStatusBusy = 75;

const char kProbeText[] = "rgexec: a probe file, not a program\n";

struct Ctx {
  const borg_host* host;
  Reader reader;

  void* last_port;
  char last[40];

  unsigned long commands;
  unsigned long produced;
  int write_failed;

  /* The arming whose answer is being read back. The command is run by the
   * kernel, not by this process, so there is no pid to wait on: the answer is a
   * file, and its status file appearing is what says it is complete. */
  int hot_active;
  void* hot_port;
  int hot_fd;
  long hot_next_ms;
  long hot_deadline_ms;
  char hot_out[192];
  char hot_exit[192];
  char hot_cmd[64];
};

Ctx g;

/* ---------------------------------------------------------------- logging */

void Logf(const char* fmt, ...) {
  char msg[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (g.host != NULL && g.host->log != NULL) g.host->log(msg);
}

/* ------------------------------------------------------------------ output */

void Raw(void* port, const char* buf, int len) {
  if (g.write_failed || port == NULL || len <= 0) return;
  if (g.host->midi_write(port, buf, static_cast<size_t>(len)) != 0) {
    g.write_failed = 1;
    Logf("midi write failed");
  }
}

void Emit(void* port, const char* buf, size_t len) {
  char enc[kMaxEncoded];
  g.produced += static_cast<unsigned long>(len);
  for (size_t i = 0; i < len; i += kMaxChunk) {
    int take = static_cast<int>(len - i);
    if (take > kMaxChunk) take = kMaxChunk;
    Raw(port, enc, borgwire::EncodeOutput(
                       reinterpret_cast<const uint8_t*>(buf + i), take, enc));
  }
}

void Say(void* port, const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  const int len = n < static_cast<int>(sizeof line) ? n
                                                    : static_cast<int>(sizeof line) - 1;
  Emit(port, line, static_cast<size_t>(len));
}

/* Truncated to one chunk: EncodeNotice is not bounded by anything, and nothing
 * except the arming sentence has any business being longer than a line. */
void Notice(void* port, const char* text) {
  char line[kMaxChunk + 1];
  char enc[kMaxEncoded];
  snprintf(line, sizeof line, "%s", text);
  Raw(port, enc, borgwire::EncodeNotice(line, enc));
}

void AtEnd(void* port, int code) {
  char enc[16];
  Raw(port, enc, borgwire::EncodeExit(code, enc));
}

/* ------------------------------------------------------------------- small */

const char* ErrnoName(int e) {
  switch (e) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case ENOEXEC: return "ENOEXEC";
    case EPERM: return "EPERM";
    case EAGAIN: return "EAGAIN";
    case EBUSY: return "EBUSY";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case ENOSYS: return "ENOSYS";
    case EINVAL: return "EINVAL";
    case ENOTDIR: return "ENOTDIR";
    case ELOOP: return "ELOOP";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case E2BIG: return "E2BIG";
    case ENOMEM: return "ENOMEM";
    case ETXTBSY: return "ETXTBSY";
    default: return "?";
  }
}

void ReadSmall(const char* path, char* out, size_t n) {
  out[0] = '\0';
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  const ssize_t got = read(fd, out, n - 1);
  close(fd);
  if (got <= 0) return;
  out[got] = '\0';
  for (ssize_t i = got - 1; i >= 0; --i) {
    if (out[i] == '\n' || out[i] == ' ' || out[i] == '\t') {
      out[i] = '\0';
      continue;
    }
    break;
  }
}

long ReadSmallLong(const char* path) {
  char buf[64];
  ReadSmall(path, buf, sizeof buf);
  if (buf[0] == '\0') return -1;
  char* end = nullptr;
  const long v = strtol(buf, &end, 10);
  if (end == buf) return -1;
  return v;
}

int CountNumericEntries(const char* dir) {
  DIR* d = opendir(dir);
  if (d == nullptr) return -1;
  int n = 0;
  for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
    const char* p = e->d_name;
    if (*p < '0' || *p > '9') continue;
    while (*p >= '0' && *p <= '9') ++p;
    if (*p == '\0') ++n;
  }
  closedir(d);
  return n;
}

enum StatusFieldResult {
  kStatusUnreadable = 0,
  kStatusAbsent,
  kStatusValue,
};

StatusFieldResult StatusField(const char* key, char* out, size_t n) {
  const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return kStatusUnreadable;

  char buf[2048];
  const ssize_t got = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (got <= 0) return kStatusUnreadable;
  buf[got] = '\0';

  const size_t klen = strlen(key);
  for (char* line = buf; line != nullptr && *line != '\0';) {
    char* nl = strchr(line, '\n');
    if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
      const char* v = line + klen + 1;
      while (*v == ' ' || *v == '\t') ++v;
      size_t i = 0;
      while (v[i] != '\0' && v[i] != '\n' && i + 1 < n) {
        out[i] = v[i];
        ++i;
      }
      out[i] = '\0';
      return kStatusValue;
    }
    if (nl == nullptr) break;
    line = nl + 1;
  }
  return kStatusAbsent;
}

const char* StatusWord(StatusFieldResult r, const char* value) {
  if (r == kStatusValue) return value;
  if (r == kStatusAbsent) return "absent (this kernel does not print the field)";
  return "unreadable (/proc/self/status could not be read)";
}

int WriteAll(const char* path, const char* text, size_t len, bool create) {
  const int flags = O_WRONLY | (create ? (O_CREAT | O_TRUNC) : 0);
  const int fd = open(path, flags, 0755);
  if (fd < 0) return errno;

  size_t done = 0;
  int rc = 0;
  while (done < len) {
    const ssize_t n = write(fd, text + done, len - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    rc = (n == 0) ? EIO : errno;
    break;
  }
  close(fd);
  return rc;
}

int ProbeExec(const char* path) {
  char* const argv[] = {const_cast<char*>(path), nullptr};
  execve(path, argv, environ);
  return errno;
}

int MakeProbeFile(const char* path, mode_t mode) {
  const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  const ssize_t len = static_cast<ssize_t>(strlen(kProbeText));
  const ssize_t wrote = write(fd, kProbeText, static_cast<size_t>(len));
  fchmod(fd, mode);
  close(fd);
  if (wrote != len) {
    unlink(path);
    return -1;
  }
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return static_cast<int>(st.st_mode & 07777);
}

bool ReadAttrLine(const char* path, char* out, size_t n) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[256];
  const ssize_t got = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (got <= 0) return false;
  buf[got] = '\0';
  char* nl = strchr(buf, '\n');
  if (nl != nullptr) *nl = '\0';
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' ||
                     buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  snprintf(out, n, "%s", buf);
  return true;
}

bool ReadAttr(const char* dir, const char* name, char* out, size_t n) {
  char path[256];
  const int wrote = snprintf(path, sizeof path, "%s/%s", dir, name);
  if (wrote < 0 || wrote >= static_cast<int>(sizeof path)) return false;
  return ReadAttrLine(path, out, n);
}

bool PathIsMountPoint(const char* path, char* src, size_t src_n, char* fstype,
                      size_t fstype_n) {
  char real[192];
  const char* want = path;
  if (realpath(path, real) != nullptr) want = real;

  FILE* f = fopen(RGEXEC_MOUNTS_PATH, "r");
  if (f == nullptr) return false;

  char line[512];
  bool found = false;
  while (fgets(line, sizeof line, f) != nullptr) {
    char source[256];
    char target[256];
    char type[64];
    if (sscanf(line, "%255s %255s %63s", source, target, type) != 3) continue;
    if (strcmp(target, want) != 0 && strcmp(target, path) != 0) continue;
    if (src != nullptr) snprintf(src, src_n, "%s", source);
    if (fstype != nullptr) snprintf(fstype, fstype_n, "%s", type);
    found = true;
    break;
  }
  fclose(f);
  return found;
}

/* -------------------------------------------------------------------- time */

long NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---------------------------------------------------------------- commands */

void AtInfo(void* port) {
  char buf[128];

  const long pid_max = ReadSmallLong("/proc/sys/kernel/pid_max");
  const long threads_max = ReadSmallLong("/proc/sys/kernel/threads-max");

  long running = -1;
  long threads = -1;
  long last_pid = -1;
  ReadSmall("/proc/loadavg", buf, sizeof buf);
  if (buf[0] == '\0' ||
      sscanf(buf, "%*s %*s %*s %ld/%ld %ld", &running, &threads, &last_pid) != 3) {
    running = threads = last_pid = -1;
  }

  const int procs = CountNumericEntries("/proc");

  struct rlimit rl;
  const bool have_rl = getrlimit(RLIMIT_NPROC, &rl) == 0;

  char nnp[32] = {0};
  char sec[32] = {0};
  const StatusFieldResult nnp_r = StatusField("NoNewPrivs", nnp, sizeof nnp);
  const StatusFieldResult sec_r = StatusField("Seccomp", sec, sizeof sec);

  struct utsname un;
  const bool have_un = uname(&un) == 0;

  Say(port, "pid_max        %ld\n", pid_max);
  Say(port, "threads-max    %ld\n", threads_max);
  Say(port, "threads        %ld  (%ld running, from /proc/loadavg)\n", threads,
      running);
  Say(port, "processes      %d  (numeric entries in /proc, thread groups)\n",
      procs);
  Say(port, "last pid       %ld  (most recently allocated; it wraps, so it is "
            "not a count)\n", last_pid);
  Say(port, "RLIMIT_NPROC   %ld  (not applied at uid 0)\n",
      have_rl ? static_cast<long>(rl.rlim_cur) : -1L);
  Say(port, "uid            %u euid %u\n", static_cast<unsigned>(getuid()),
      static_cast<unsigned>(geteuid()));
  Say(port, "NoNewPrivs     %s\n", StatusWord(nnp_r, nnp));
  Say(port, "Seccomp        %s\n", StatusWord(sec_r, sec));
  Say(port, "kernel         %s\n", have_un ? un.release : "uname could not say");

  long ceiling = threads_max;
  if (pid_max > 0 && (ceiling <= 0 || pid_max < ceiling)) ceiling = pid_max;

  if (threads >= 0 && ceiling > 0) {
    if (threads * 10 >= ceiling * 9) {
      Say(port, "verdict: %ld threads against a ceiling of %ld. The pid/thread "
                "space is nearly full, so a refusal to start a process is "
                "exhaustion.\n", threads, ceiling);
    } else {
      Say(port, "verdict: %ld threads against a ceiling of %ld, nowhere near it, "
                "and this kernel has no LSM, no seccomp, no cgroup and no pid "
                "namespace. The counters do not explain the refusal.\n",
          threads, ceiling);
    }
  } else {
    Say(port, "verdict: the counters could not be read, so nothing here settles "
              "anything.\n");
  }

  if (pid_max <= 0 || threads_max <= 0 || threads < 0 || procs < 0) {
    Logf("info: a counter would not read");
    AtEnd(port, 1);
    return;
  }
  AtEnd(port, 0);
}

/* `@exec probe`: is execve permitted here at all? The four paths are chosen so
 * their answers mean something together:
 *
 *   a name nothing created   ENOENT   the lookup happened
 *   a directory              EACCES   something was found and refused
 *   a file with mode 0644    EACCES   the mode check happened and said no
 *   a file with mode 0755
 *   holding text             ENOEXEC  the mode check passed, the file was
 *                                     opened, and the binary handlers ran far
 *                                     enough to reject its contents
 *
 * EPERM anywhere, above all on the file with the execute bit, would mean the
 * call is refused before the file is looked at. No file of ours can cause that.
 * Nothing may actually run: every path handed to execve is one that cannot. */
void AtProbe(void* port) {
  const char* dir = RGEXEC_PROBE_DIR;
  char missing[128];
  char nox[128];
  char bad[128];
  snprintf(missing, sizeof missing, "%s/exec-probe-missing", dir);
  snprintf(nox, sizeof nox, "%s/exec-probe-nox", dir);
  snprintf(bad, sizeof bad, "%s/exec-probe-bad", dir);

  struct statvfs vfs;
  const bool have_vfs = statvfs(dir, &vfs) == 0;
  const bool noexec = have_vfs && (vfs.f_flag & ST_NOEXEC) != 0;

  Say(port, "exec probe in %s\n", dir);
  Say(port, "mount: %s\n",
      !have_vfs ? "statvfs could not read it, so the mount is unknown"
                : (noexec ? "execution forbidden by the mount (noexec)"
                          : "execution allowed by the mount"));

  unlink(missing);

  const int nox_mode = MakeProbeFile(nox, 0644);
  const int bad_mode = MakeProbeFile(bad, 0755);
  if (nox_mode < 0 || bad_mode < 0) {
    const int why = errno;
    Say(port, "could not write in %s: %s (%d)\n", dir, strerror(why), why);
    Logf("probe: cannot write a probe file in %s: %s (%d)", dir, strerror(why),
         why);
    AtEnd(port, 1);
    return;
  }
  Say(port, "wrote %s mode %04o and %s mode %04o\n", nox, nox_mode, bad,
      bad_mode);

  const int e_missing = ProbeExec(missing);
  const int e_dir = ProbeExec(dir);
  const int e_nox = ProbeExec(nox);
  const int e_bad = ProbeExec(bad);

  unlink(nox);
  unlink(bad);

  Say(port, "%-11s %-8s %2d  %s\n", "missing", ErrnoName(e_missing), e_missing,
      missing);
  Say(port, "%-11s %-8s %2d  %s\n", "directory", ErrnoName(e_dir), e_dir, dir);
  Say(port, "%-11s %-8s %2d  %s\n", "mode 0644", ErrnoName(e_nox), e_nox, nox);
  Say(port, "%-11s %-8s %2d  %s\n", "mode 0755", ErrnoName(e_bad), e_bad, bad);

  const bool refused_call = e_missing == EPERM || e_dir == EPERM ||
                            e_nox == EPERM || e_bad == EPERM;
  const bool file_shaped = e_missing == ENOENT && e_dir == EACCES &&
                           e_nox == EACCES && e_bad == ENOEXEC;

  if (file_shaped) {
    Say(port, "verdict: execve is permitted. Every refusal above is the file's "
              "own, and the last one got past the mode check and into the "
              "binary handlers, so this kernel would not refuse an exec in "
              "place.\n");
    AtEnd(port, 0);
    return;
  }
  if (e_bad == EACCES && noexec) {
    Say(port, "verdict: the mount forbids execution, so neither file could have "
              "run and this says nothing about execve. Point RGEXEC_PROBE_DIR "
              "at a directory that is not mounted noexec.\n");
    Logf("probe: %s is mounted noexec, so the probe proved nothing", dir);
    AtEnd(port, 1);
    return;
  }
  if (refused_call) {
    Say(port, "verdict: execve answered EPERM, and no file of ours can cause "
              "that. It is the call being refused before the file is looked at, "
              "which would close this route.\n");
    Logf("probe: execve refused with EPERM, whatever the path");
    AtEnd(port, 1);
    return;
  }
  Say(port, "verdict: neither shape. The four answers above are the whole "
            "result, and what they mean is not something this command knows.\n");
  Logf("probe: answered %d %d %d %d, neither shape", e_missing, e_dir, e_nox,
       e_bad);
  AtEnd(port, 1);
}

void AtCat(void* port, const char* rest) {
  if (rest[0] == '\0') {
    Notice(port, "!cat: no path; @exec cat <path> [offset]");
    Logf("cat asked with no path");
    AtEnd(port, 2);
    return;
  }

  char path[512];
  if (rest[0] == '/') {
    snprintf(path, sizeof path, "%s", rest);
  } else {
    snprintf(path, sizeof path, "%s/%s", RGEXEC_RUN_DIR, rest);
  }

  long offset = 0;
  char* space = strchr(path, ' ');
  if (space != nullptr) {
    *space = '\0';
    char* end = nullptr;
    offset = strtol(space + 1, &end, 10);
    if (end == space + 1 || offset < 0) offset = 0;
  }

  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    char line[224];
    snprintf(line, sizeof line, "!cat: %.120s: %s (%d)", path, strerror(errno),
             errno);
    Notice(port, line);
    Logf("%s", line);
    AtEnd(port, 1);
    return;
  }
  if (offset > 0 && lseek(fd, offset, SEEK_SET) < 0) offset = 0;

  char buf[kCatBytes];
  const ssize_t got = read(fd, buf, sizeof buf);
  const int read_errno = errno;
  close(fd);
  if (got < 0) {
    char line[224];
    snprintf(line, sizeof line, "!cat: %.120s: %s (%d)", path,
             strerror(read_errno), read_errno);
    Notice(port, line);
    Logf("%s", line);
    AtEnd(port, 1);
    return;
  }

  Emit(port, buf, static_cast<size_t>(got));
  if (got == static_cast<ssize_t>(sizeof buf)) {
    char line[224];
    snprintf(line, sizeof line,
             "!cat: %.120s continues past %ld; ask again with that offset",
             path, offset + static_cast<long>(got));
    Notice(port, line);
  }
  AtEnd(port, 0);
}

void AtLs(void* port, const char* rest) {
  char path[512];
  if (rest[0] == '\0') {
    snprintf(path, sizeof path, "/");
  } else {
    snprintf(path, sizeof path, "%s", rest);
  }

  DIR* dir = opendir(path);
  if (dir == nullptr) {
    char line[224];
    snprintf(line, sizeof line, "!ls: %.120s: %s (%d)", path, strerror(errno),
             errno);
    Notice(port, line);
    Logf("%s", line);
    AtEnd(port, 1);
    return;
  }

  Say(port, "ls: %s\n", path);
  const int kMaxEntries = 64;
  int shown = 0;
  bool more = false;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    if (shown >= kMaxEntries) {
      more = true;
      break;
    }

    char full[800];
    const int wrote = snprintf(full, sizeof full, "%s/%s", path, ent->d_name);
    if (wrote < 0 || wrote >= static_cast<int>(sizeof full)) {
      Say(port, "? %s (path too long)\n", ent->d_name);
      ++shown;
      continue;
    }

    struct stat st;
    if (lstat(full, &st) != 0) {
      Say(port, "? %s\n", ent->d_name);
    } else if (S_ISLNK(st.st_mode)) {
      char target[128];
      const ssize_t len = readlink(full, target, sizeof target - 1);
      if (len > 0) {
        target[len] = '\0';
        Say(port, "l %s -> %s\n", ent->d_name, target);
      } else {
        Say(port, "l %s -> ?\n", ent->d_name);
      }
    } else if (S_ISDIR(st.st_mode)) {
      Say(port, "d %s/\n", ent->d_name);
    } else if (S_ISREG(st.st_mode)) {
      Say(port, "f %s %lld\n", ent->d_name,
          static_cast<long long>(st.st_size));
    } else if (S_ISBLK(st.st_mode)) {
      Say(port, "b %s\n", ent->d_name);
    } else if (S_ISCHR(st.st_mode)) {
      Say(port, "c %s\n", ent->d_name);
    } else if (S_ISFIFO(st.st_mode)) {
      Say(port, "p %s\n", ent->d_name);
    } else if (S_ISSOCK(st.st_mode)) {
      Say(port, "s %s\n", ent->d_name);
    } else {
      Say(port, "? %s\n", ent->d_name);
    }
    ++shown;
  }
  closedir(dir);

  if (more) {
    Say(port, "ls: first %d entries, more follow\n", shown);
  } else {
    Say(port, "ls: %d entries\n", shown);
  }
  AtEnd(port, 0);
}

/* ---------------------------------------------------------------- storage */

void StorageSummary(void* port) {
  char dir[256];
  snprintf(dir, sizeof dir, "%s/bus/usb/devices", RGEXEC_SYSFS_DIR);
  DIR* d = opendir(dir);
  int roots = 0;
  int devices = 0;
  char first[400] = "";
  if (d != nullptr) {
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      if (ent->d_name[0] == '.' || strchr(ent->d_name, ':') != nullptr) {
        continue;
      }
      if (strncmp(ent->d_name, "usb", 3) == 0) {
        ++roots;
        continue;
      }
      ++devices;
      if (first[0] == '\0') {
        char full[520];
        char vid[32] = "?";
        char pid[32] = "?";
        char product[64] = "?";
        snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
        ReadAttr(full, "idVendor", vid, sizeof vid);
        ReadAttr(full, "idProduct", pid, sizeof pid);
        if (!ReadAttr(full, "product", product, sizeof product)) {
          snprintf(product, sizeof product, "?");
        }
        snprintf(first, sizeof first, "%s %s:%s \"%s\"", ent->d_name, vid, pid,
                 product);
      }
    }
    closedir(d);
  }
  Say(port, "storage: usb %d root hub%s, %d device%s\n", roots,
      roots == 1 ? "" : "s", devices, devices == 1 ? "" : "s");
  if (first[0] != '\0') Say(port, "storage: usb device %s\n", first);

  snprintf(dir, sizeof dir, "%s/class/block", RGEXEC_SYSFS_DIR);
  d = opendir(dir);
  char disk[64] = "";
  char disk_size[32] = "?";
  if (d != nullptr) {
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      const char* name = ent->d_name;
      if (strncmp(name, "sd", 2) != 0) continue;
      /* A disk is sda or sdb; a partition is sda1 and longer. */
      if (strlen(name) != 3) continue;
      snprintf(disk, sizeof disk, "%.3s", name);
      char full[520];
      snprintf(full, sizeof full, "%s/%s", dir, name);
      ReadAttr(full, "size", disk_size, sizeof disk_size);
      break;
    }
    closedir(d);
  }
  if (disk[0] != '\0') {
    const unsigned long long sectors = strtoull(disk_size, nullptr, 10);
    Say(port, "storage: sd %s %llu MiB\n", disk, sectors / 2048ULL);
  } else {
    Say(port, "storage: sd none\n");
  }

  char src[128];
  char fstype[64];
  if (PathIsMountPoint(RGEXEC_USERFS_PATH, src, sizeof src, fstype,
                       sizeof fstype)) {
    Say(port, "storage: userfs external %s (%s)\n", src, fstype);
  } else {
    Say(port, "storage: userfs internal %s\n", RGEXEC_USERFS_PATH);
  }

  char value[32];
  snprintf(dir, sizeof dir, "%s/class/gpio/gpio6", RGEXEC_SYSFS_DIR);
  if (ReadAttr(dir, "value", value, sizeof value)) {
    Say(port, "storage: bus power gpio6=%s\n", value);
  } else {
    Say(port, "storage: bus power gpio6 not exported\n");
  }
  Say(port, "storage: try devs block mounts userfs gpio probe\n");
}

void StorageDevs(void* port) {
  char dir[256];
  snprintf(dir, sizeof dir, "%s/bus/usb/devices", RGEXEC_SYSFS_DIR);
  Say(port, "devices: %s\n", dir);

  DIR* d = opendir(dir);
  if (d == nullptr) {
    Say(port, "devices: cannot read: %s (%d)\n", strerror(errno), errno);
    return;
  }

  int shown = 0;
  bool more = false;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.' || strchr(ent->d_name, ':') != nullptr) continue;
    if (shown >= 24) {
      more = true;
      break;
    }

    char full[520];
    char vid[32] = "?";
    char pid[32] = "?";
    char product[64] = "?";
    char manufacturer[64] = "?";
    char speed[32] = "?";
    snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
    ReadAttr(full, "idVendor", vid, sizeof vid);
    ReadAttr(full, "idProduct", pid, sizeof pid);
    ReadAttr(full, "product", product, sizeof product);
    ReadAttr(full, "manufacturer", manufacturer, sizeof manufacturer);
    ReadAttr(full, "speed", speed, sizeof speed);

    if (strncmp(ent->d_name, "usb", 3) == 0) {
      Say(port, "%-8s %s:%s root hub\n", ent->d_name, vid, pid);
    } else {
      Say(port, "%-8s %s:%s \"%s\" %s speed=%s\n", ent->d_name, vid, pid,
          product, manufacturer, speed);
    }
    ++shown;
  }
  closedir(d);
  if (more) Say(port, "devices: first %d shown, more follow\n", shown);
  if (shown == 0) Say(port, "devices: none\n");
}

void StorageBlock(void* port) {
  char dir[256];
  snprintf(dir, sizeof dir, "%s/class/block", RGEXEC_SYSFS_DIR);
  Say(port, "block: %s\n", dir);

  DIR* d = opendir(dir);
  if (d == nullptr) {
    Say(port, "block: cannot read: %s (%d)\n", strerror(errno), errno);
    return;
  }

  int shown = 0;
  bool more = false;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    if (ent->d_name[0] == '.') continue;
    if (shown >= 32) {
      more = true;
      break;
    }

    char full[520];
    char size[32] = "";
    char removable[16] = "?";
    char dev[32] = "?";
    char partition[16] = "";
    snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
    ReadAttr(full, "size", size, sizeof size);
    ReadAttr(full, "removable", removable, sizeof removable);
    ReadAttr(full, "dev", dev, sizeof dev);
    ReadAttr(full, "partition", partition, sizeof partition);

    const unsigned long long sectors = strtoull(size, nullptr, 10);
    char node[520];
    snprintf(node, sizeof node, "%s/%s", RGEXEC_DEV_DIR, ent->d_name);
    const bool present = access(node, F_OK) == 0;
    Say(port, "%-10s %-7s %10llu sectors %6llu MiB removable=%s part=%s "
              "node=%s\n",
        ent->d_name, dev, sectors, sectors / 2048ULL, removable,
        partition[0] == '1' ? "yes" : "no", present ? "yes" : "no");
    ++shown;
  }
  closedir(d);
  if (more) Say(port, "block: first %d shown, more follow\n", shown);
  if (shown == 0) Say(port, "block: none\n");
}

void StorageMounts(void* port) {
  Say(port, "mounts: %s\n", RGEXEC_MOUNTS_PATH);
  FILE* f = fopen(RGEXEC_MOUNTS_PATH, "r");
  if (f == nullptr) {
    Say(port, "mounts: cannot read: %s (%d)\n", strerror(errno), errno);
    Logf("mounts: %s: %s", RGEXEC_MOUNTS_PATH, strerror(errno));
    return;
  }

  char real_userfs[192];
  const char* want = RGEXEC_USERFS_PATH;
  if (realpath(RGEXEC_USERFS_PATH, real_userfs) != nullptr) want = real_userfs;

  int shown = 0;
  bool more = false;
  char line[512];
  while (fgets(line, sizeof line, f) != nullptr) {
    char source[256];
    char target[256];
    char type[64];
    if (sscanf(line, "%255s %255s %63s", source, target, type) != 3) continue;
    if (shown >= 32) {
      more = true;
      break;
    }
    const bool is_userfs = strcmp(target, want) == 0 ||
                           strcmp(target, RGEXEC_USERFS_PATH) == 0;
    Say(port, "%s -> %s (%s)%s\n", source, target, type,
        is_userfs ? " [userfs]" : "");
    ++shown;
  }
  fclose(f);
  if (more) Say(port, "mounts: first %d shown, more follow\n", shown);
  if (shown == 0) Say(port, "mounts: none\n");

  char src[128];
  char fstype[64];
  if (PathIsMountPoint(RGEXEC_USERFS_PATH, src, sizeof src, fstype,
                       sizeof fstype)) {
    Say(port, "userfs external: %s -> %s (%s)\n", src, RGEXEC_USERFS_PATH,
        fstype);
  } else {
    Say(port, "userfs internal: %s is not a mount point\n", RGEXEC_USERFS_PATH);
  }
}

void StorageUserfs(void* port) {
  struct stat st;
  if (stat(RGEXEC_USERFS_PATH, &st) != 0) {
    Say(port, "userfs: %s: %s (%d)\n", RGEXEC_USERFS_PATH, strerror(errno),
        errno);
    return;
  }
  if (!S_ISDIR(st.st_mode)) {
    Say(port, "userfs: %s is not a directory\n", RGEXEC_USERFS_PATH);
    return;
  }

  Say(port, "userfs: %s\n", RGEXEC_USERFS_PATH);
  Say(port, "userfs: mode %04o uid %u gid %u dev %lu\n",
      static_cast<unsigned>(st.st_mode & 07777),
      static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_gid),
      static_cast<unsigned long>(st.st_dev));

  char src[128];
  char fstype[64];
  if (PathIsMountPoint(RGEXEC_USERFS_PATH, src, sizeof src, fstype,
                       sizeof fstype)) {
    Say(port, "userfs: external, %s (%s)\n", src, fstype);
  } else {
    Say(port, "userfs: internal, on the root filesystem\n");
  }

  struct statvfs vfs;
  if (statvfs(RGEXEC_USERFS_PATH, &vfs) == 0) {
    const unsigned long long unit =
        vfs.f_frsize != 0 ? vfs.f_frsize : vfs.f_bsize;
    const unsigned long long total = static_cast<unsigned long long>(
                                         vfs.f_blocks) * unit / (1024ULL * 1024ULL);
    const unsigned long long free = static_cast<unsigned long long>(
                                        vfs.f_bavail) * unit / (1024ULL * 1024ULL);
    Say(port, "userfs: %llu MiB total, %llu MiB free, %s, %s\n", total, free,
        (vfs.f_flag & ST_RDONLY) != 0 ? "ro" : "rw",
        (vfs.f_flag & ST_NOEXEC) != 0 ? "noexec" : "exec");
  }

  if (PathIsMountPoint(RGEXEC_USERFS_MNT, src, sizeof src, fstype,
                       sizeof fstype)) {
    Say(port, "userfs: ramdisk mounted at %s\n", RGEXEC_USERFS_MNT);
  } else if (access(RGEXEC_USERFS_MNT, F_OK) == 0) {
    Say(port, "userfs: %s present, not mounted\n", RGEXEC_USERFS_MNT);
  } else {
    Say(port, "userfs: ramdisk not present\n");
  }

  if (access(RGEXEC_USERFS_IMG, F_OK) == 0) {
    Say(port, "userfs: legacy image present at %s\n", RGEXEC_USERFS_IMG);
  } else {
    Say(port, "userfs: legacy image absent\n");
  }
}

void StorageGpio(void* port) {
  Say(port, "gpio: %s/class/gpio\n", RGEXEC_SYSFS_DIR);
  struct {
    int pin;
    const char* what;
  } pins[] = {{6, "usb bus power"}, {7, "bus power flag"}};

  for (size_t i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/class/gpio/gpio%d", RGEXEC_SYSFS_DIR,
             pins[i].pin);
    char direction[32];
    char value[32];
    const bool have_direction = ReadAttr(dir, "direction", direction,
                                         sizeof direction);
    const bool have_value = ReadAttr(dir, "value", value, sizeof value);
    if (!have_direction && !have_value) {
      Say(port, "gpio%d %s: not exported\n", pins[i].pin, pins[i].what);
    } else {
      Say(port, "gpio%d %s: direction=%s value=%s\n", pins[i].pin,
          pins[i].what, have_direction ? direction : "?",
          have_value ? value : "?");
    }
  }
}

/* Whether mount(2) is permitted, asked with the one operation the external
 * storage design needs: a tmpfs, a directory on it, a bind mount, then all of
 * it taken away. Every path is inside RGEXEC_STORAGE_MNT, which is under /tmp
 * on the instrument; nothing on the rootfs or the userfs is touched. */
int StorageProbe(void* port) {
  const char* mnt = RGEXEC_STORAGE_MNT;
  char src[256];
  char dst[256];
  char marker[288];
  snprintf(src, sizeof src, "%s/src", mnt);
  snprintf(dst, sizeof dst, "%s/dst", mnt);
  snprintf(marker, sizeof marker, "%s/marker", src);

  Say(port, "probe: %s\n", mnt);
  char mounted_from[128];
  char mounted_type[64];
  if (PathIsMountPoint(mnt, mounted_from, sizeof mounted_from, mounted_type,
                       sizeof mounted_type)) {
    Say(port, "probe: already a mount point; not touching it\n");
    return 2;
  }

  if (mkdir(mnt, 0755) != 0 && errno != EEXIST) {
    Say(port, "probe: mkdir %s: %s (%d)\n", mnt, strerror(errno), errno);
    Logf("probe: mkdir %s: %s", mnt, strerror(errno));
    return 2;
  }

  if (mount("none", mnt, "tmpfs", 0, "size=64k") != 0) {
    const int e = errno;
    Say(port, "probe: mount tmpfs: refused: %s (%d)\n", strerror(e), e);
    rmdir(mnt);
    Say(port, "probe: done, mount is not permitted\n");
    return 1;
  }
  Say(port, "probe: mount tmpfs: permitted\n");

  if (mkdir(src, 0755) != 0 || mkdir(dst, 0755) != 0) {
    const int e = errno;
    Say(port, "probe: mkdir on the tmpfs: %s (%d)\n", strerror(e), e);
    Logf("probe: mkdir on the tmpfs: %s", strerror(e));
    umount2(mnt, 0);
    rmdir(mnt);
    return 2;
  }

  const int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    const int e = errno;
    Say(port, "probe: write %s: %s (%d)\n", marker, strerror(e), e);
    Logf("probe: write %s: %s", marker, strerror(e));
    umount2(mnt, 0);
    rmdir(mnt);
    return 2;
  }
  const char text[] = "probe\n";
  write(fd, text, sizeof text - 1);
  close(fd);

  if (mount(src, dst, nullptr, MS_BIND, nullptr) != 0) {
    const int e = errno;
    Say(port, "probe: bind mount: refused: %s (%d)\n", strerror(e), e);
    Logf("probe: bind mount: %s", strerror(e));
    umount2(mnt, 0);
    rmdir(mnt);
    return 2;
  }
  Say(port, "probe: bind mount: permitted\n");

  char seen[288];
  snprintf(seen, sizeof seen, "%s/marker", dst);
  struct stat st;
  Say(port, "probe: bind visible: %s\n", stat(seen, &st) == 0 ? "yes" : "no");

  int status = 0;
  if (umount2(dst, 0) != 0) {
    const int e = errno;
    if (umount2(dst, MNT_DETACH) != 0) {
      Say(port, "probe: umount bind: %s (%d)\n", strerror(e), e);
      status = 2;
    } else {
      Say(port, "probe: umount bind: detached\n");
    }
  } else {
    Say(port, "probe: umount bind: ok\n");
  }

  if (umount2(mnt, 0) != 0) {
    const int e = errno;
    if (umount2(mnt, MNT_DETACH) != 0) {
      Say(port, "probe: umount tmpfs: %s (%d)\n", strerror(e), e);
      status = 2;
    } else {
      Say(port, "probe: umount tmpfs: detached\n");
    }
  } else {
    Say(port, "probe: umount tmpfs: ok\n");
  }

  rmdir(src);
  rmdir(dst);
  if (rmdir(mnt) != 0 && errno != ENOENT) {
    Say(port, "probe: rmdir %s: %s (%d)\n", mnt, strerror(errno), errno);
    status = 2;
  }

  if (status == 0) {
    Say(port, "probe: done, mount is permitted\n");
  } else {
    Say(port, "probe: done, mount was permitted but cleanup failed\n");
  }
  return status;
}

void AtStorage(void* port, const char* rest) {
  char word[24];
  size_t n = 0;
  while (rest[n] != '\0' && rest[n] != ' ' && n + 1 < sizeof word) {
    word[n] = rest[n];
    ++n;
  }
  word[n] = '\0';

  if (word[0] == '\0' || strcmp(word, "status") == 0) {
    StorageSummary(port);
  } else if (strcmp(word, "devs") == 0) {
    StorageDevs(port);
  } else if (strcmp(word, "block") == 0) {
    StorageBlock(port);
  } else if (strcmp(word, "mounts") == 0) {
    StorageMounts(port);
  } else if (strcmp(word, "userfs") == 0) {
    StorageUserfs(port);
  } else if (strcmp(word, "gpio") == 0) {
    StorageGpio(port);
  } else if (strcmp(word, "all") == 0) {
    StorageDevs(port);
    StorageBlock(port);
    StorageMounts(port);
    StorageUserfs(port);
    StorageGpio(port);
  } else if (strcmp(word, "probe") == 0) {
    AtEnd(port, StorageProbe(port));
    return;
  } else {
    Notice(port, "!storage: try status devs block mounts userfs gpio probe all");
    Logf("storage: no such word: %s", word);
    AtEnd(port, 2);
    return;
  }
  AtEnd(port, 0);
}

/* -------------------------------------------------------------------- hot */

const char* PickUevent(void* port, char* buf, size_t len) {
  unsigned card = 0;
  const char* name = (g.host->port_name != NULL) ? g.host->port_name(port) : "";
  for (const char* s = name; *s != '\0'; ++s) {
    if (*s != 'C' || s[1] < '0' || s[1] > '9') continue;
    char* end = nullptr;
    const long v = strtol(s + 1, &end, 10);
    if (end == s + 1 || v < 0 || v > 99) continue;
    card = static_cast<unsigned>(v);
    break;
  }

  char first[176] = {0};
  const char* p = RGEXEC_UEVENTS;
  while (*p != '\0') {
    const char* colon = strchr(p, ':');
    const size_t n = (colon != nullptr) ? static_cast<size_t>(colon - p)
                                        : strlen(p);

    char fmt[160];
    if (n > 0 && n < sizeof fmt) {
      memcpy(fmt, p, n);
      fmt[n] = '\0';
      if (strstr(fmt, "%u") != nullptr) {
        snprintf(buf, len, fmt, card);
      } else {
        snprintf(buf, len, "%s", fmt);
      }
      if (access(buf, F_OK) == 0) return buf;
      if (first[0] == '\0') snprintf(first, sizeof first, "%.175s", buf);
    }

    if (colon == nullptr) break;
    p = colon + 1;
  }

  snprintf(buf, len, "%s", (first[0] != '\0') ? first : RGEXEC_UEVENT);
  return buf;
}

int ReadStatus(const char* path) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  char buf[16];
  const ssize_t got = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (got < 2 || buf[got - 1] != '\n') return -1;
  buf[got] = '\0';
  char* end = nullptr;
  const long value = strtol(buf, &end, 10);
  if (end == buf || value < 0 || value > 255) return -1;
  return static_cast<int>(value);
}

void FinishHot(int code) {
  if (g.hot_fd >= 0) {
    close(g.hot_fd);
    g.hot_fd = -1;
  }
  if (code != 0 && code != kStatusTimeout) {
    Logf("exit %d: %s", code, g.hot_cmd[0] != '\0' ? g.hot_cmd : "helper");
  }
  AtEnd(g.hot_port, code);
  g.hot_active = 0;
  g.hot_port = NULL;
  g.hot_next_ms = 0;
  g.hot_deadline_ms = 0;
  g.hot_out[0] = '\0';
  g.hot_exit[0] = '\0';
  g.hot_cmd[0] = '\0';
}

/* `@exec <command>`: have the kernel run it. This is the one route to command
 * execution that survives every way of creating a process being refused: the
 * process is created by the kernel's own helper thread, not by this process.
 * The script leaves /tmp, points /proc/sys/kernel/hotplug at itself, sends the
 * command's output to a file, and puts the helper back to /sbin/mdev before the
 * command runs. The answer is read by tick(), not here: the command is not this
 * process's child, and the reply is not finished when this returns. */
void AtHot(void* port, const char* command) {
  if (command[0] == '\0') {
    Notice(port, "!exec: nothing to run");
    Logf("hot asked with no command");
    AtEnd(port, 2);
    return;
  }

  char stem[160];
  snprintf(stem, sizeof stem, "%s/rgx.%lu", RGEXEC_RUN_DIR, g.commands);
  char script_path[192];
  char out_path[192];
  char exit_path[192];
  char done_path[192];
  snprintf(script_path, sizeof script_path, "%s.sh", stem);
  snprintf(out_path, sizeof out_path, "%s.out", stem);
  snprintf(exit_path, sizeof exit_path, "%s.exit", stem);
  snprintf(done_path, sizeof done_path, "%s.done", stem);

  char script[2048];
  const int n = snprintf(
      script, sizeof script,
      "#!/bin/sh\n"
      "# left by the borg exec module. One run per arming, then the helper goes "
      "back.\n"
      "[ -f %s ] && exec %s\n"
      "touch %s\n"
      "echo %s > %s\n"
      "export PATH=/bin:/sbin:/usr/bin:/usr/sbin\n"
      "{\n%s\n} > %s 2>&1\n"
      "printf '%%d\\n' $? > %s\n"
      "exec %s\n",
      done_path, RGEXEC_HELPER, done_path, RGEXEC_HELPER, RGEXEC_HOTPLUG,
      command, out_path, exit_path, RGEXEC_HELPER);
  if (n < 0 || static_cast<size_t>(n) >= sizeof script) {
    Notice(port, "!exec: that command does not fit in one script");
    Logf("hot: command of %d bytes does not fit",
         static_cast<int>(strlen(command)));
    AtEnd(port, 2);
    return;
  }

  char uevent[176];
  const char* last = script_path;

  int rc = WriteAll(script_path, script, strlen(script), true);
  if (rc == 0) {
    chmod(script_path, 0755);
    char aim[224];
    snprintf(aim, sizeof aim, "%.200s\n", script_path);
    last = RGEXEC_HOTPLUG;
    rc = WriteAll(RGEXEC_HOTPLUG, aim, strlen(aim), false);
    if (rc == 0) {
      last = PickUevent(port, uevent, sizeof uevent);
      rc = WriteAll(last, "add\n", 4, false);
    }
  }

  char line[512];
  if (rc != 0) {
    if (last == script_path) {
      snprintf(line, sizeof line, "!exec: refused (%s %d) writing %.100s",
               ErrnoName(rc), rc, last);
    } else {
      snprintf(line, sizeof line,
               "!exec: refused (%s %d) writing %.100s; the script is at %.150s",
               ErrnoName(rc), rc, last, script_path);
    }
    Logf("%s", line);
    Notice(port, line);
    AtEnd(port, 1);
    return;
  }

  snprintf(g.hot_out, sizeof g.hot_out, "%s", out_path);
  snprintf(g.hot_exit, sizeof g.hot_exit, "%s", exit_path);
  snprintf(g.hot_cmd, sizeof g.hot_cmd, "%s", command);
  g.hot_fd = -1;
  g.hot_next_ms = 0;
  g.hot_deadline_ms = NowMs() + kCommandSeconds * 1000L;
  g.hot_port = port;
  g.hot_active = 1;

  snprintf(line, sizeof line, "!armed: running through %.100s; the answer "
                              "follows on this cable", last);
  Notice(port, line);
}

/* One tick's worth of an outstanding arming. The answer file is opened when it
 * appears, read in bounded pieces, and the status file is read only once the
 * output has been read to its end in this tick, so a long answer is never cut
 * off by its own exit status. */
void TickHot(void) {
  char buf[kReadBytes];

  if (g.hot_fd < 0) {
    const long now = NowMs();
    if (g.hot_next_ms != 0 && now < g.hot_next_ms) return;
    g.hot_fd = open(g.hot_out, O_RDONLY | O_CLOEXEC);
    if (g.hot_fd < 0) {
      g.hot_next_ms = now + kHotWaitMs;
      if (now >= g.hot_deadline_ms) {
        char line[320];
        snprintf(line, sizeof line,
                 "!timeout: no answer after %d seconds; the command is still "
                 "running, and %.150s holds what it has written",
                 kCommandSeconds, g.hot_out);
        Notice(g.hot_port, line);
        Logf("deadline with an arming outstanding: %s", g.hot_out);
        FinishHot(kStatusTimeout);
      }
      return;
    }
    g.hot_next_ms = 0;
  }

  bool at_end = false;
  int budget = 4096;
  for (;;) {
    const ssize_t got = read(g.hot_fd, buf, sizeof buf);
    if (got > 0) {
      Emit(g.hot_port, buf, static_cast<size_t>(got));
      budget -= static_cast<int>(got);
      if (budget <= 0) break;
      continue;
    }
    if (got == 0) at_end = true;
    if (got < 0 && errno == EINTR) continue;
    break;
  }

  if (at_end && g.hot_exit[0] != '\0') {
    const int code = ReadStatus(g.hot_exit);
    if (code >= 0) {
      FinishHot(code);
      return;
    }
  }

  if (NowMs() >= g.hot_deadline_ms) {
    char line[320];
    snprintf(line, sizeof line,
             "!timeout: no answer after %d seconds; the command is still running, "
             "and %.150s holds what it has written", kCommandSeconds, g.hot_out);
    Notice(g.hot_port, line);
    Logf("deadline with an arming outstanding: %s", g.hot_out);
    FinishHot(kStatusTimeout);
  }
}

/* --------------------------------------------------------------- dispatch */

void Help(void* port) {
  Say(port, "@exec info      process and thread counters, and a verdict\n");
  Say(port, "@exec probe     whether execve is permitted\n");
  Say(port, "@exec cat <path> [offset]\n");
  Say(port, "@exec ls [path]\n");
  Say(port, "@exec storage [status|devs|block|mounts|userfs|gpio|probe|all]\n");
  Say(port, "@exec <command> run it through the kernel's helper\n");
  AtEnd(port, 0);
}

void AtCommand(void* port, const char* rest) {
  while (*rest == ' ') ++rest;

  char word[24];
  size_t n = 0;
  while (rest[n] != '\0' && rest[n] != ' ' && n + 1 < sizeof word) {
    word[n] = rest[n];
    ++n;
  }
  word[n] = '\0';
  const char* after = rest + n;
  while (*after == ' ') ++after;

  if (word[0] == '\0' || strcmp(word, "help") == 0) {
    Help(port);
    return;
  }
  if (strcmp(word, "info") == 0) {
    AtInfo(port);
    return;
  }
  if (strcmp(word, "probe") == 0) {
    AtProbe(port);
    return;
  }
  if (strcmp(word, "cat") == 0) {
    AtCat(port, after);
    return;
  }
  if (strcmp(word, "ls") == 0) {
    AtLs(port, after);
    return;
  }
  if (strcmp(word, "storage") == 0) {
    AtStorage(port, after);
    return;
  }
  if (strcmp(word, "hot") == 0) {
    AtHot(port, after);
    return;
  }

  /* Not a built-in: the whole remainder is the command. */
  AtHot(port, rest);
}

void Handle(void* port, const char* cmd) {
  ++g.commands;
  g.last_port = port;
  snprintf(g.last, sizeof g.last, "%.39s", cmd[0] != '\0' ? cmd : "-");
  g.write_failed = 0;

  if (g.hot_active) {
    Logf("busy, refused: %s", cmd);
    Notice(port, "!busy");
    AtEnd(port, kStatusBusy);
    return;
  }

  if (strncmp(cmd, "@exec", 5) != 0 || (cmd[5] != '\0' && cmd[5] != ' ')) {
    /* An @ that is not @exec names another module's namespace. Every module is
     * offered every message, so answering one that is not ours would race the
     * module that owns it and put this refusal in front of its reply. */
    if (cmd[0] == '@') return;
    Notice(port, "!ns: commands start with @exec");
    Logf("not an @exec command: %s", cmd);
    AtEnd(port, 2);
    return;
  }

  AtCommand(port, cmd[5] == ' ' ? cmd + 6 : "");
}

/* -------------------------------------------------------------- callbacks */

void exec_midi_in(void* port, const unsigned char* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (g.reader.Feed(data[i])) {
      Handle(port, g.reader.Command());
      continue;
    }
    const char* text = borgwire::RefusalText(g.reader.TakeRefusal());
    if (text != nullptr) Notice(port, text);
  }
}

void exec_tick(void) {
  if (g.hot_active) TickHot();
}

void exec_page(void) {
  if (g.host == NULL || g.host->row == NULL) return;

  char value[24];
  g.host->row(26, "STATE:", g.hot_active ? "RUNNING" : "READY");
  snprintf(value, sizeof value, "%lu", g.commands);
  g.host->row(35, "COMMANDS:", value);
  snprintf(value, sizeof value, "%.23s", g.last[0] != '\0' ? g.last : "-");
  g.host->row(44, "LAST:", value);
  const char* name = "";
  if (g.last_port != NULL && g.host->port_name != NULL) {
    name = g.host->port_name(g.last_port);
  }
  g.host->row(53, "PORT:", (name != NULL && name[0] != '\0') ? name : "-");
}

int exec_init(const borg_host* host, const char* module_path) {
  (void)module_path;
  g.host = host;
  if (host == NULL || host->midi_write == NULL) return 1;
  Logf("exec module ready");
  return 0;
}

const borg_module exec_module = {
  BORG_MODULE_ABI,
  "EXEC",
  "1.0.0",
  BORG_CAP_PAGE | BORG_CAP_MIDI,
  exec_init,
  exec_page,
  exec_midi_in,
  exec_tick,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,  // audio_render (ABI 4)
  nullptr,  // midi_msg (ABI 4)
  nullptr   // midi_transform (ABI 5)
};

}  // namespace

extern "C" const borg_module* borg_module_entry(uint32_t host_abi) {
  if (host_abi < 2) return NULL;
  return &exec_module;
}
