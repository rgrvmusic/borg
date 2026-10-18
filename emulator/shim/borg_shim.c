/* Logs and marks a preload attachment, then removes this object from the
 * environment inherited by child processes. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Tests can redirect these paths at compile time. */
#ifndef LOGUE_BORG_LOG
#define LOGUE_BORG_LOG "/tmp/logue-borg.log"
#endif
#ifndef LOGUE_BORG_LOG_PERSIST
#define LOGUE_BORG_LOG_PERSIST "/var/lib/drumlogued/userfs/Programs/borg-shim.log"
#endif
#ifndef LOGUE_BORG_MARK
#define LOGUE_BORG_MARK "/tmp/logue-borg.active"
#endif

#define BORG_LINE 1024

static int borg_open_raw(const char* path, int flags) {
  return (int)syscall(SYS_openat, AT_FDCWD, path, flags, 0644);
}

static void borg_write_all(int fd, const char* s, size_t n) {
  while (n > 0) {
    const long wrote = syscall(SYS_write, fd, s, n);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return;
    }
    s += (size_t)wrote;
    n -= (size_t)wrote;
  }
}

/* Only the daemon's attach is copied to persistent storage. */
static int borg_is_daemon(const char* image) {
  const char* base = strrchr(image, '/');
  base = base != NULL ? base + 1 : image;
  return strstr(base, "drumlogued") != NULL;
}

/* Remove this shim from LD_PRELOAD while preserving the other entries. */
static void borg_forget_preload(const char* preload) {
  static const char kBase[] = "libborg.so";
  const size_t base_len = sizeof kBase - 1;
  char original[BORG_LINE];
  char kept[BORG_LINE];
  size_t t = 0;

  if (preload == NULL || preload[0] == '\0') return;
  snprintf(original, sizeof original, "%s", preload);
  setenv("LOGUE_BORG_PRELOAD", original, 1);

  const char* p = original;
  while (*p != '\0') {
    const char* colon = strchr(p, ':');
    const size_t len = colon != NULL ? (size_t)(colon - p) : strlen(p);
    const int own =
        len >= base_len && memcmp(p + len - base_len, kBase, base_len) == 0 &&
        (len == base_len || p[len - base_len - 1] == '/');
    if (!own && len > 0) {
      if (t != 0 && t + 1 < sizeof kept) kept[t++] = ':';
      if (t + len < sizeof kept) {
        memcpy(kept + t, p, len);
        t += len;
      }
    }
    if (colon == NULL) break;
    p = colon + 1;
  }
  kept[t] = '\0';
  if (t == 0) {
    unsetenv("LD_PRELOAD");
  } else {
    setenv("LD_PRELOAD", kept, 1);
  }
}

static void borg_image(char* out, size_t n) {
  const long got = syscall(SYS_readlink, "/proc/self/exe", out, n - 1);
  if (got <= 0) {
    snprintf(out, n, "?");
    return;
  }
  size_t len = (size_t)got;
  if (len > n - 1) len = n - 1;
  out[len] = '\0';
}

static void borg_log(const char* path, const char* line) {
  const int fd = borg_open_raw(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
  if (fd < 0) return;
  borg_write_all(fd, line, strlen(line));
  syscall(SYS_close, fd);
}

__attribute__((constructor)) static void borg_init(void) {
  char image[256];
  char line[BORG_LINE];
  char pidbuf[32];
  char preload[512];
  const int pid = (int)getpid();
  const int uid = (int)getuid();
  const long now = (long)time(NULL);

  /* Keep startup output in the redirected log. */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  borg_image(image, sizeof image);

  /* Copy before setenv can reallocate environ. */
  {
    const char* in = getenv("LD_PRELOAD");
    snprintf(preload, sizeof preload, "%s", in != NULL ? in : "");
  }

  snprintf(line, sizeof line,
           "[borg] attached pid=%d uid=%d image=%s preload=%s\n",
           pid, uid, image, preload[0] != '\0' ? preload : "(unset)");

  borg_log(LOGUE_BORG_LOG, line);
  if (borg_is_daemon(image)) borg_log(LOGUE_BORG_LOG_PERSIST, line);

  /* The marker describes the current attachment. */
  const int fd = borg_open_raw(LOGUE_BORG_MARK,
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  if (fd >= 0) {
    snprintf(line, sizeof line, "pid %d\nuid %d\nattached %ld\nimage %s\n",
             pid, uid, now, image);
    borg_write_all(fd, line, strlen(line));
    syscall(SYS_close, fd);
  }

  /* Used by the unit's status readout. */
  snprintf(pidbuf, sizeof pidbuf, "%d", pid);
  setenv("LOGUE_BORG_ACTIVE", "1", 1);
  setenv("LOGUE_BORG_PID", pidbuf, 1);

  borg_forget_preload(preload);
}
