/* storage.borgmodule: an external userfs on a USB stick, chosen from the panel.
 *
 * WHY THIS EXISTS. The daemon opens /var/lib/drumlogued/userfs once, at start,
 * and sanitizes what it finds. A stick cannot be mounted there while it runs.
 * It can, however, be mounted there and then the daemon re-executed in place:
 * same process, same pid, its main runs again and opens the new tree. borg
 * already knows how to do that, so this module does the two things around it:
 * it finds and mounts the stick, and it binds one session's userfs onto the
 * directory the daemon expects.
 *
 * WHAT A SESSION IS. A directory on the stick:
 *
 *   borg/sessions/<name>/userfs/{Programs,Units,Samples,Kits}
 *
 * The userfs subtree is what the daemon opens, so it carries the layout the
 * daemon itself would have created, not the stick's own. Nothing outside the
 * chosen subtree is ever shown to the daemon.
 *
 * WHAT IS NOT TOUCHED. No file is written to the stick, to the rootfs or to
 * the internal userfs. The only state is in the kernel's mount table, and it
 * is gone at the next power cycle. A session is refused when the daemon's own
 * sanitizer would change it, so a switch cannot quietly edit the stick: hidden
 * files, links, special files, unexpected top-level entries and names with
 * spaces are all reasons to leave it alone.
 *
 * WHY THE PANEL. The page lists sessions, the first knob scrolls, and MASTER
 * takes the highlighted row: the first press marks it, the second switches, or
 * returns to the internal userfs when it is the active one. NO cancels. The
 * wire answers the same actions under @storage.
 *
 * The callbacks are serialized by the resident object, so there is no lock and
 * no thread here. Nothing in this module creates a process or a thread; the
 * switch is an execve of the process that already exists, through borg. */

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using borgwire::kMaxChunk;
using borgwire::kMaxEncoded;
using borgwire::Reader;

/* The paths this module uses. The overrides exist so a test can lay out a
 * scratch tree; the device build passes nothing. */
#ifndef STORAGE_BASE
#define STORAGE_BASE "/tmp/logue-borg"
#endif
#ifndef STORAGE_STICK
#define STORAGE_STICK STORAGE_BASE "/stick"
#endif
#ifndef STORAGE_USERFS
#define STORAGE_USERFS "/var/lib/drumlogued/userfs"
#endif
#ifndef STORAGE_SESSIONS
#define STORAGE_SESSIONS "borg/sessions"
#endif
#ifndef STORAGE_SYSFS
#define STORAGE_SYSFS "/sys"
#endif
#ifndef STORAGE_DEV
#define STORAGE_DEV "/dev"
#endif
#ifndef STORAGE_MOUNTINFO
#define STORAGE_MOUNTINFO "/proc/self/mountinfo"
#endif

constexpr int kMaxSessions = 32;
constexpr int kNameMax = 40;
constexpr int kRows = 3;

/* MASTER takes the highlighted row; NO cancels. The knobs under the display
 * only turn. */
constexpr int kKeyMaster = 0x0f;
constexpr int kKeyNo = 0x30;
constexpr int kRowY = 35;
constexpr int kRowStep = 9;
constexpr int kFileBudget = 20000;
constexpr int kDepthMax = 6;
constexpr int kRetryTicks = 10;

struct State {
  const borg_host* host = nullptr;
  Reader reader;

  bool mounted = false;
  bool need_scan = false;
  int retry = 0;

  int count = 0;
  char session[kMaxSessions][kNameMax] = {{0}};
  char active[kNameMax] = {0};
  char device[64] = {0};
  char fstype[24] = {0};
  char error[48] = {0};

  int cursor = 0;
  int first = 0;
  int press_row = -1;
};

State g;

/* The switch, exported by the resident object. It is weak so this module still
 * loads when borg's switch is not there; the page then says it cannot reload
 * instead of the whole module failing to resolve. */
extern "C" int borg_reexec(void) __attribute__((weak));

/* --------------------------------------------------------------- speaking */

void Logf(const char* fmt, ...) {
  char msg[192];
  va_list ap;

  if (g.host == nullptr || g.host->log == nullptr) return;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  g.host->log(msg);
}

bool CanWrite(void* port) {
  return port != nullptr && g.host != nullptr && g.host->midi_write != nullptr;
}

void Say(void* port, const char* fmt, ...) {
  char line[256];
  char enc[kMaxEncoded];
  va_list ap;
  size_t len, off;

  if (!CanWrite(port)) return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  len = strlen(line);
  for (off = 0; off < len; off += kMaxChunk) {
    size_t n = len - off;
    int encoded;
    if (n > static_cast<size_t>(kMaxChunk)) n = kMaxChunk;
    encoded = borgwire::EncodeOutput(
        reinterpret_cast<const uint8_t*>(line + off), static_cast<int>(n), enc);
    g.host->midi_write(port, enc, static_cast<size_t>(encoded));
  }
}

void Notice(void* port, const char* fmt, ...) {
  char line[192];
  char enc[kMaxEncoded];
  va_list ap;

  if (!CanWrite(port)) return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  g.host->midi_write(port, enc, static_cast<size_t>(
      borgwire::EncodeNotice(line, enc)));
}

void End(void* port, int status) {
  char enc[16];

  if (!CanWrite(port)) return;
  g.host->midi_write(port, enc, static_cast<size_t>(
      borgwire::EncodeExit(status, enc)));
}

/* -------------------------------------------------------- the mount table */

void Unescape(const char* in, char* out, size_t cap) {
  size_t o = 0;

  for (size_t i = 0; in[i] != '\0' && o + 1 < cap; ++i) {
    if (in[i] == '\\' && in[i + 1] >= '0' && in[i + 1] <= '7' &&
        in[i + 2] >= '0' && in[i + 2] <= '7' &&
        in[i + 3] >= '0' && in[i + 3] <= '7') {
      out[o++] = static_cast<char>(((in[i + 1] - '0') << 6) |
                                   ((in[i + 2] - '0') << 3) |
                                   (in[i + 3] - '0'));
      i += 3;
    } else {
      out[o++] = in[i];
    }
  }
  out[o] = '\0';
}

/* One line of mountinfo for `target`: the root inside the source filesystem,
 * the filesystem type, and the source. For a bind mount the root is the path
 * inside the source filesystem, which is how the active session is recovered
 * after a re-exec without keeping a note anywhere. */
struct MountInfo {
  char root[192];
  char source[192];
  char type[48];
};

bool LookupMount(const char* target, MountInfo* out) {
  FILE* f = fopen(STORAGE_MOUNTINFO, "r");
  char line[1024];
  bool found = false;

  if (f == nullptr) return false;
  while (fgets(line, sizeof line, f) != nullptr) {
    char* sep = strstr(line, " - ");
    char* field[6] = {nullptr};
    char* p = line;
    int nf = 0;

    if (sep == nullptr) continue;
    *sep = '\0';
    while (nf < 6 && *p != '\0') {
      while (*p == ' ') ++p;
      if (*p == '\0') break;
      field[nf++] = p;
      while (*p != '\0' && *p != ' ') ++p;
      if (*p != '\0') *p++ = '\0';
    }
    if (nf < 5 || strcmp(field[4], target) != 0) continue;

    Unescape(field[3], out->root, sizeof out->root);
    p = sep + 3;
    nf = 0;
    while (nf < 3 && *p != '\0') {
      while (*p == ' ') ++p;
      if (*p == '\0') break;
      field[nf++] = p;
      while (*p != '\0' && *p != ' ') ++p;
      if (*p != '\0') *p++ = '\0';
    }
    out->type[0] = '\0';
    out->source[0] = '\0';
    if (nf >= 1) Unescape(field[0], out->type, sizeof out->type);
    if (nf >= 2) Unescape(field[1], out->source, sizeof out->source);
    found = true;
    break;
  }
  fclose(f);
  return found;
}

bool MountRoot(const char* target, char* root, size_t cap) {
  MountInfo info;

  if (!LookupMount(target, &info)) return false;
  snprintf(root, cap, "%s", info.root);
  return true;
}

bool IsMountPoint(const char* target) {
  MountInfo info;
  return LookupMount(target, &info);
}

/* A session is active when the userfs mount's root is one of ours. An external
 * userfs set up at boot has root "/" and is not ours to replace. */
void DetectActive() {
  static const char kPrefix[] = "/" STORAGE_SESSIONS "/";
  char root[256];
  size_t plen, rlen, n;

  g.active[0] = '\0';
  if (!MountRoot(STORAGE_USERFS, root, sizeof root)) return;

  plen = sizeof kPrefix - 1;
  rlen = strlen(root);
  if (rlen <= plen + 7) return;
  if (strncmp(root, kPrefix, plen) != 0) return;
  if (strcmp(root + rlen - 7, "/userfs") != 0) return;

  n = rlen - plen - 7;
  if (n >= sizeof g.active) return;
  memcpy(g.active, root + plen, n);
  g.active[n] = '\0';
}

bool UserfsIsForeign() {
  MountInfo info;
  if (!LookupMount(STORAGE_USERFS, &info)) return false;
  return g.active[0] == '\0';
}

/* ------------------------------------------------------------ the stick */

struct Device {
  char name[24];
  int partition;
};

int CompareDevices(const void* a, const void* b) {
  const Device* da = static_cast<const Device*>(a);
  const Device* db = static_cast<const Device*>(b);

  if (da->partition != db->partition) return db->partition - da->partition;
  return strcmp(da->name, db->name);
}

int ListDevices(Device* out, int max) {
  DIR* d = opendir(STORAGE_SYSFS "/class/block");
  struct dirent* ent;
  int n = 0;

  if (d == nullptr) return 0;
  while ((ent = readdir(d)) != nullptr) {
    const char* name = ent->d_name;
    size_t len = strlen(name);
    int partition;

    if (strncmp(name, "sd", 2) != 0) continue;
    if (len < 3 || len >= sizeof out[0].name) continue;
    if (name[2] < 'a' || name[2] > 'z') continue;
    partition = len > 3;
    if (partition && (name[3] < '0' || name[3] > '9')) continue;
    if (n >= max) break;

    memcpy(out[n].name, name, len + 1);
    out[n].partition = partition;
    ++n;
  }
  closedir(d);

  qsort(out, static_cast<size_t>(n), sizeof out[0], CompareDevices);
  return n;
}

/* What a mounted filesystem type would be, read from the medium itself rather
 * than guessed. The kernel has vfat and ext4 built in; exFAT and NTFS are not
 * available, and saying so is more useful than a bare mount failure. */
int DetectFs(const char* node, char* fs, size_t cap, const char** why) {
  unsigned char boot[4096];
  ssize_t n;
  int fd;

  fd = open(node, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *why = "NO DEVICE NODE";
    return -1;
  }
  n = read(fd, boot, sizeof boot);
  close(fd);
  if (n < 512) {
    *why = "STICK NOT READABLE";
    return -1;
  }

  if (n >= 11 && memcmp(boot + 3, "EXFAT   ", 8) == 0) {
    *why = "EXFAT NOT SUPPORTED";
    return -1;
  }
  if (boot[54] == 'F' && boot[55] == 'A' && boot[56] == 'T') {
    snprintf(fs, cap, "vfat");
    return 0;
  }
  if (n >= 1082 && boot[1080] == 0x53 && boot[1081] == 0xEF) {
    snprintf(fs, cap, "ext4");
    return 0;
  }
  if (n >= 11 && memcmp(boot + 3, "NTFS    ", 8) == 0) {
    *why = "NTFS NOT SUPPORTED";
    return -1;
  }
  *why = "FORMAT NOT SUPPORTED";
  return -1;
}

void SetError(const char* text) {
  snprintf(g.error, sizeof g.error, "%s", text);
}

int MountStick() {
  Device devs[16];
  const char* why = "NO STICK FOUND";
  int n;

  g.error[0] = '\0';
  if (IsMountPoint(STORAGE_STICK)) {
    MountInfo info;
    if (LookupMount(STORAGE_STICK, &info)) {
      snprintf(g.device, sizeof g.device, "%.*s",
               static_cast<int>(sizeof g.device) - 1, info.source);
      snprintf(g.fstype, sizeof g.fstype, "%.*s",
               static_cast<int>(sizeof g.fstype) - 1, info.type);
    }
    g.mounted = true;
    return 0;
  }

  if ((mkdir(STORAGE_BASE, 0755) != 0 && errno != EEXIST) ||
      (mkdir(STORAGE_STICK, 0755) != 0 && errno != EEXIST)) {
    SetError("CANNOT PREPARE");
    Logf("mkdir %s: %s", STORAGE_STICK, strerror(errno));
    return -1;
  }

  n = ListDevices(devs, 16);
  if (n == 0) {
    SetError(why);
    return -1;
  }

  for (int i = 0; i < n; ++i) {
    char node[512];
    char fs[16];

    snprintf(node, sizeof node, "%s/%.200s", STORAGE_DEV, devs[i].name);
    if (DetectFs(node, fs, sizeof fs, &why) != 0) continue;

    if (mount(node, STORAGE_STICK, fs, MS_NOSUID | MS_NODEV, nullptr) == 0) {
      g.mounted = true;
      snprintf(g.device, sizeof g.device, "%.*s",
               static_cast<int>(sizeof g.device) - 1, node);
      snprintf(g.fstype, sizeof g.fstype, "%.*s",
               static_cast<int>(sizeof g.fstype) - 1, fs);
      g.error[0] = '\0';
      Logf("mounted %s at %s (%s)", node, STORAGE_STICK, fs);
      return 0;
    }

    why = errno == EPERM ? "MOUNT REFUSED" : "STICK NOT READABLE";
    Logf("mount %s: %s", node, strerror(errno));
  }

  SetError(why);
  return -1;
}

int UnmountStick() {
  if (g.active[0] != '\0') {
    SetError("SESSION ACTIVE");
    return -1;
  }
  if (!IsMountPoint(STORAGE_STICK)) {
    g.mounted = false;
    g.count = 0;
    return 0;
  }
  if (umount2(STORAGE_STICK, MNT_DETACH) != 0) {
    SetError("STICK IS BUSY");
    Logf("umount %s: %s", STORAGE_STICK, strerror(errno));
    return -1;
  }
  g.mounted = false;
  g.count = 0;
  g.cursor = 0;
  g.first = 0;
  g.device[0] = '\0';
  g.fstype[0] = '\0';
  return 0;
}

int EnsureMounted() {
  if (g.mounted && IsMountPoint(STORAGE_STICK)) return 0;
  g.mounted = false;
  return MountStick();
}

/* ------------------------------------------------------------ the sessions */

bool ValidName(const char* name) {
  size_t n = strlen(name);

  if (n == 0 || n >= kNameMax) return false;
  if (name[0] == '.') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 0x21 || c > 0x7E || c == '/') return false;
  }
  return true;
}

void FitWindow() {
  const int last = g.count > 0 ? g.count - 1 : 0;

  if (g.cursor < 0) g.cursor = 0;
  if (g.cursor > last) g.cursor = last;
  if (g.first > g.cursor) g.first = g.cursor;
  if (g.cursor >= g.first + kRows) g.first = g.cursor - (kRows - 1);
  if (g.count <= kRows) g.first = 0;
  if (g.first < 0) g.first = 0;
  if (g.first > last) g.first = last;
}

void ScanSessions() {
  char dir[256];
  DIR* d;

  g.count = 0;
  snprintf(dir, sizeof dir, "%s/%s", STORAGE_STICK, STORAGE_SESSIONS);
  d = opendir(dir);
  if (d == nullptr) {
    FitWindow();
    return;
  }

  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    char full[640];
    struct stat st;
    const char* name = ent->d_name;

    if (!ValidName(name)) continue;
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (lstat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    if (g.count >= kMaxSessions) break;
    snprintf(g.session[g.count], kNameMax, "%.*s",
             static_cast<int>(kNameMax) - 1, name);
    ++g.count;
  }
  closedir(d);

  for (int i = 1; i < g.count; ++i) {
    char key[kNameMax];
    int j;
    snprintf(key, sizeof key, "%s", g.session[i]);
    for (j = i - 1; j >= 0 && strcmp(g.session[j], key) > 0; --j) {
      memcpy(g.session[j + 1], g.session[j], kNameMax);
    }
    memcpy(g.session[j + 1], key, kNameMax);
  }
  FitWindow();
}

/* The daemon's sanitizer deletes hidden files, links, special files and
 * unexpected top-level entries, and renames names it cannot keep. Refuse any
 * session where that would happen, so choosing a session never edits a stick.
 * The check is deliberately conservative. */
bool WalkUserfs(const char* path, int depth, bool top, const char** why,
                int* budget) {
  DIR* d = opendir(path);
  struct dirent* ent;
  bool ok = true;

  if (d == nullptr) {
    *why = "CANNOT READ SESSION";
    return false;
  }

  while ((ent = readdir(d)) != nullptr) {
    const char* name = ent->d_name;
    char full[512];
    struct stat st;

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
    if (--(*budget) < 0) {
      *why = "SESSION TOO LARGE";
      ok = false;
      break;
    }
    if (name[0] == '.') {
      *why = "HIDDEN FILES";
      ok = false;
      break;
    }
    for (const char* c = name; *c != '\0'; ++c) {
      const unsigned char u = static_cast<unsigned char>(*c);
      if (u < 0x21 || u > 0x7E) {
        *why = "UNSUPPORTED NAME";
        ok = false;
        break;
      }
    }
    if (!ok) break;

    snprintf(full, sizeof full, "%s/%s", path, name);
    if (lstat(full, &st) != 0) {
      *why = "CANNOT READ SESSION";
      ok = false;
      break;
    }
    if (S_ISLNK(st.st_mode)) {
      *why = "LINKS IN SESSION";
      ok = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (top && strcmp(name, "Programs") != 0 && strcmp(name, "Units") != 0 &&
          strcmp(name, "Samples") != 0 && strcmp(name, "Kits") != 0) {
        *why = "UNEXPECTED FOLDER";
        ok = false;
        break;
      }
      if (depth >= kDepthMax) {
        *why = "SESSION TOO DEEP";
        ok = false;
        break;
      }
      if (!WalkUserfs(full, depth + 1, false, why, budget)) {
        ok = false;
        break;
      }
    } else if (!S_ISREG(st.st_mode)) {
      *why = "SPECIAL FILES";
      ok = false;
      break;
    } else if (top) {
      *why = "UNEXPECTED FILE";
      ok = false;
      break;
    }
  }

  closedir(d);
  return ok;
}

bool HasFolder(const char* base, const char* name) {
  char path[512];
  struct stat st;

  snprintf(path, sizeof path, "%s/%s", base, name);
  return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int CheckSession(const char* name, const char** why) {
  char base[420];
  char ufs[460];
  char mod[560];
  struct stat st;
  int budget = kFileBudget;

  if (!ValidName(name)) {
    *why = "NO SUCH SESSION";
    return -1;
  }

  snprintf(base, sizeof base, "%s/%s/%s", STORAGE_STICK, STORAGE_SESSIONS,
           name);
  if (lstat(base, &st) != 0 || !S_ISDIR(st.st_mode)) {
    *why = "NO SUCH SESSION";
    return -1;
  }

  snprintf(ufs, sizeof ufs, "%s/userfs", base);
  if (lstat(ufs, &st) != 0 || !S_ISDIR(st.st_mode)) {
    *why = "NO USERFS FOLDER";
    return -1;
  }

  snprintf(mod, sizeof mod, "%s/Programs/storage.borgmodule", ufs);
  if (lstat(mod, &st) != 0 || !S_ISREG(st.st_mode)) {
    *why = "STORAGE MODULE MISSING";
    return -1;
  }

  if (!WalkUserfs(ufs, 0, true, why, &budget)) return -1;

  /* A session has the same four top-level folders as the internal userfs. */
  static const struct {
    const char* name;
    const char* why;
  } kFolders[] = {
      {"Programs", "NO PROGRAMS"},
      {"Units", "NO UNITS"},
      {"Samples", "NO SAMPLES"},
      {"Kits", "NO KITS"},
  };
  for (size_t i = 0; i < sizeof kFolders / sizeof kFolders[0]; ++i) {
    if (!HasFolder(ufs, kFolders[i].name)) {
      *why = kFolders[i].why;
      return -1;
    }
  }
  return 0;
}

void SessionSource(const char* name, char* out, size_t cap) {
  snprintf(out, cap, "%s/%s/%s/userfs", STORAGE_STICK, STORAGE_SESSIONS, name);
}

int BindSession(const char* name, const char** why) {
  char src[460];
  char old[460] = {0};

  if (UserfsIsForeign()) {
    *why = "USERFS IS MOUNTED";
    return -1;
  }
  if (g.active[0] != '\0') {
    if (strcmp(g.active, name) == 0) return 0;
    SessionSource(g.active, old, sizeof old);
    umount2(STORAGE_USERFS, MNT_DETACH);
  }

  SessionSource(name, src, sizeof src);
  if (mount(src, STORAGE_USERFS, nullptr, MS_BIND, nullptr) != 0) {
    const int e = errno;
    if (old[0] != '\0') {
      mount(old, STORAGE_USERFS, nullptr, MS_BIND, nullptr);
    }
    *why = e == EPERM ? "BIND REFUSED" : "SWITCH FAILED";
    Logf("bind %s: %s", src, strerror(e));
    return -1;
  }

  snprintf(g.active, sizeof g.active, "%s", name);
  return 0;
}

int DetachSession(const char** why) {
  if (g.active[0] == '\0') return 0;
  if (!IsMountPoint(STORAGE_USERFS)) {
    g.active[0] = '\0';
    return 0;
  }
  if (umount2(STORAGE_USERFS, MNT_DETACH) != 0) {
    *why = "USERFS IS BUSY";
    Logf("detach %s: %s", STORAGE_USERFS, strerror(errno));
    return -1;
  }
  g.active[0] = '\0';
  return 0;
}

/* ------------------------------------------------------------- the switch */

bool CanReload() {
  return borg_reexec != nullptr;
}

/* Both actions end in an execve that does not return. What is here after the
 * call is the failure path: put the mount table back the way it was and leave
 * the running daemon with the tree it started on. */
int UseSession(const char* name, void* port) {
  const char* why = "";
  char previous[kNameMax] = {0};
  int rc;

  if (!CanReload()) {
    SetError("RELOAD UNAVAILABLE");
    Notice(port, "!storage: the switch is not available");
    End(port, 2);
    return -1;
  }
  if (EnsureMounted() != 0) {
    Notice(port, "!storage: %s", g.error);
    End(port, 2);
    return -1;
  }
  DetectActive();
  if (CheckSession(name, &why) != 0) {
    SetError(why);
    Notice(port, "!storage: %s", why);
    End(port, 2);
    return -1;
  }
  if (strcmp(name, g.active) == 0) {
    Say(port, "storage: %s is already active\n", name);
    End(port, 0);
    return 0;
  }

  snprintf(previous, sizeof previous, "%s", g.active);
  if (BindSession(name, &why) != 0) {
    SetError(why);
    Notice(port, "!storage: %s", why);
    End(port, 2);
    return -1;
  }

  Say(port, "storage: switching to %s\n", name);
  End(port, 0);
  Logf("switching to session %s", name);

  rc = borg_reexec();

  /* The new image never started. Undo the bind and say so. */
  umount2(STORAGE_USERFS, MNT_DETACH);
  if (previous[0] != '\0') {
    char old[460];
    SessionSource(previous, old, sizeof old);
    if (mount(old, STORAGE_USERFS, nullptr, MS_BIND, nullptr) == 0) {
      snprintf(g.active, sizeof g.active, "%s", previous);
    } else {
      g.active[0] = '\0';
    }
  } else {
    g.active[0] = '\0';
  }

  SetError("RELOAD FAILED");
  Logf("re-exec failed: %s (%d)", strerror(-rc), -rc);
  Notice(port, "!storage: the daemon could not restart");
  End(port, 2);
  return -1;
}

int ReturnInternal(void* port) {
  const char* why = "";
  char previous[kNameMax] = {0};
  int rc;

  if (!CanReload()) {
    SetError("RELOAD UNAVAILABLE");
    Notice(port, "!storage: the switch is not available");
    End(port, 2);
    return -1;
  }
  if (g.active[0] == '\0') {
    Say(port, "storage: internal userfs already\n");
    End(port, 0);
    return 0;
  }

  snprintf(previous, sizeof previous, "%s", g.active);
  if (DetachSession(&why) != 0) {
    SetError(why);
    Notice(port, "!storage: %s", why);
    End(port, 2);
    return -1;
  }

  Say(port, "storage: returning to internal\n");
  End(port, 0);
  Logf("returning to the internal userfs");

  rc = borg_reexec();

  /* Restore the session the daemon is still running on. */
  if (previous[0] != '\0') {
    char old[460];
    SessionSource(previous, old, sizeof old);
    if (mount(old, STORAGE_USERFS, nullptr, MS_BIND, nullptr) == 0) {
      snprintf(g.active, sizeof g.active, "%s", previous);
    }
  }
  SetError("RELOAD FAILED");
  Logf("re-exec failed: %s (%d)", strerror(-rc), -rc);
  Notice(port, "!storage: the daemon could not restart");
  End(port, 2);
  return -1;
}

/* --------------------------------------------------------------- the page */

/* The value column holds twelve characters; g.error keeps the full sentence. */
const char* PageReason(const char* text) {
  static const struct {
    const char* text;
    const char* page;
  } kReasons[] = {
      {"NO STICK FOUND", "NO STICK"},
      {"CANNOT PREPARE", "NO STICK"},
      {"MOUNT REFUSED", "MOUNT DENIED"},
      {"NO DEVICE NODE", "NO DEVICE"},
      {"STICK NOT READABLE", "NOT READABLE"},
      {"EXFAT NOT SUPPORTED", "NO EXFAT"},
      {"NTFS NOT SUPPORTED", "NO NTFS"},
      {"FORMAT NOT SUPPORTED", "BAD FORMAT"},
      {"NO SUCH SESSION", "NO SESSION"},
      {"NO USERFS FOLDER", "NO USERFS"},
      {"STORAGE MODULE MISSING", "NO MODULE"},
      {"NO PROGRAMS", "NO PROGRAMS"},
      {"NO UNITS", "NO UNITS"},
      {"NO SAMPLES", "NO SAMPLES"},
      {"NO KITS", "NO KITS"},
      {"CANNOT READ SESSION", "READ FAILED"},
      {"SESSION TOO LARGE", "TOO LARGE"},
      {"HIDDEN FILES", "HIDDEN FILES"},
      {"UNSUPPORTED NAME", "BAD NAME"},
      {"LINKS IN SESSION", "HAS LINKS"},
      {"UNEXPECTED FOLDER", "BAD FOLDER"},
      {"SESSION TOO DEEP", "TOO DEEP"},
      {"SPECIAL FILES", "SPECIAL FILE"},
      {"UNEXPECTED FILE", "BAD FILE"},
      {"STICK IS BUSY", "STICK BUSY"},
      {"SESSION ACTIVE", "STILL ACTIVE"},
      {"USERFS IS MOUNTED", "USERFS BUSY"},
      {"USERFS IS BUSY", "USERFS BUSY"},
      {"RELOAD UNAVAILABLE", "NO RELOAD"},
      {"RELOAD FAILED", "RELOAD FAIL"},
      {"BIND REFUSED", "BIND DENIED"},
      {"SWITCH FAILED", "SWITCH FAIL"},
  };
  for (size_t i = 0; i < sizeof kReasons / sizeof kReasons[0]; ++i) {
    if (strcmp(text, kReasons[i].text) == 0) return kReasons[i].page;
  }
  return "ERROR";
}

const char* StickLabel() {
  static char label[16];

  if (g.error[0] != '\0') return PageReason(g.error);
  if (g.need_scan) return "READING";
  if (!g.mounted) return "NO STICK";
  snprintf(label, sizeof label, "READY %d", g.count);
  return label;
}

const char* EmptyText() {
  if (g.error[0] != '\0') return g.error;
  if (g.need_scan) return "READING STICK";
  if (!g.mounted) return "INSERT A USB STICK";
  return "NO SESSIONS FOUND";
}

void DrawPage() {
  if (g.host == nullptr || g.host->row == nullptr) return;

  g.host->row(26, "STICK:", StickLabel());
  if (g.count == 0) {
    if (g.host->text != nullptr) g.host->text(0, kRowY, 248, 8, EmptyText());
    return;
  }

  FitWindow();
  for (int i = 0; i < kRows; ++i) {
    const int idx = g.first + i;
    char label[16];
    const char* value;

    if (idx >= g.count) break;
    snprintf(label, sizeof label, "%s%.9s", idx == g.cursor ? "> " : "  ",
             g.session[idx]);
    if (idx == g.press_row) {
      value = "PRESS AGAIN";
    } else if (strcmp(g.session[idx], g.active) == 0) {
      value = "ACTIVE";
    } else {
      value = "USE";
    }
    if (g.host->row != nullptr) g.host->row(kRowY + i * kRowStep, label, value);
  }
}

void PressRow(int idx) {
  const char* name;

  if (idx < 0 || idx >= g.count) return;
  g.cursor = idx;
  if (g.press_row != idx) {
    g.press_row = idx;
    FitWindow();
    return;
  }

  g.press_row = -1;
  name = g.session[idx];
  if (strcmp(name, g.active) == 0) {
    ReturnInternal(nullptr);
  } else {
    UseSession(name, nullptr);
  }
}

/* -------------------------------------------------------------- callbacks */

void storage_page_enter() {
  g.press_row = -1;
  /* The page tests the stick again on every entry, so a refusal from an
   * earlier press is not left standing over a state that has moved on. */
  g.error[0] = '\0';
  g.need_scan = true;
  g.retry = 0;
}

void storage_page_leave() {
  g.press_row = -1;
}

void storage_key(int index, int down) {
  if (!down) return;

  /* NO cancels. MASTER takes the highlighted row: once marks it, twice
   * switches. */
  if (index == kKeyNo) {
    g.press_row = -1;
    return;
  }
  if (index != kKeyMaster) return;
  if (g.need_scan) return;
  if (!g.mounted) {
    g.need_scan = true;
    return;
  }
  PressRow(g.cursor);
}

void storage_encoder(int index, int delta) {
  if (index != 0) return;
  if (!g.mounted || g.count == 0) {
    g.need_scan = true;
    return;
  }
  g.cursor += delta;
  g.press_row = -1;
  FitWindow();
}

void storage_tick() {
  if (g.need_scan) {
    g.need_scan = false;
    if (EnsureMounted() == 0) {
      DetectActive();
      ScanSessions();
    }
    g.retry = g.mounted ? 0 : kRetryTicks;
    return;
  }

  if (!g.mounted && g.retry > 0) {
    if (--g.retry == 0) g.need_scan = true;
  }
}

/* --------------------------------------------------------------- the wire */

void StatusReply(void* port) {
  DetectActive();

  if (g.mounted) {
    Say(port, "storage: stick %s on %s (%s)\n", g.device, STORAGE_STICK,
        g.fstype);
  } else {
    Say(port, "storage: no stick mounted\n");
  }
  if (g.active[0] != '\0') {
    Say(port, "storage: session %s is active\n", g.active);
  } else {
    Say(port, "storage: internal userfs\n");
  }
  Say(port, "storage: %d session%s\n", g.count, g.count == 1 ? "" : "s");
  if (g.error[0] != '\0') Say(port, "storage: %s\n", g.error);
}

void ListReply(void* port) {
  if (EnsureMounted() != 0) {
    Notice(port, "!storage: %s", g.error);
    End(port, 2);
    return;
  }
  DetectActive();
  ScanSessions();

  Say(port, "storage: %d session%s on %s\n", g.count,
      g.count == 1 ? "" : "s", STORAGE_STICK);
  for (int i = 0; i < g.count; ++i) {
    Say(port, "  %-24s%s\n", g.session[i],
        strcmp(g.session[i], g.active) == 0 ? " active" : "");
  }
  End(port, 0);
}

void CheckReply(void* port, const char* name) {
  const char* why = "";

  if (EnsureMounted() != 0) {
    Notice(port, "!storage: %s", g.error);
    End(port, 2);
    return;
  }
  if (CheckSession(name, &why) != 0) {
    Notice(port, "!storage: %s", why);
    End(port, 2);
    return;
  }
  Say(port, "storage: %s is ready\n", name);
  End(port, 0);
}

/* The rest of a request as one bare name, trimmed at both ends. A name with a
 * space in it is refused later by ValidName, so the wire cannot address two
 * sessions with one request. */
bool NameArg(const char* rest, char* out, size_t cap) {
  size_t n;

  while (*rest == ' ') ++rest;
  n = strlen(rest);
  while (n > 0 && rest[n - 1] == ' ') --n;
  if (n == 0 || n >= cap) return false;
  memcpy(out, rest, n);
  out[n] = '\0';
  return true;
}

void HelpReply(void* port) {
  Say(port, "storage: status | sessions | mount | umount\n");
  Say(port, "storage: use NAME | back | check NAME\n");
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
    DetectActive();
    if (g.mounted) ScanSessions();
    StatusReply(port);
  } else if (strcmp(word, "sessions") == 0 || strcmp(word, "list") == 0) {
    ListReply(port);
    return;
  } else if (strcmp(word, "mount") == 0) {
    if (EnsureMounted() != 0) {
      Notice(port, "!storage: %s", g.error);
      End(port, 2);
      return;
    }
    DetectActive();
    ScanSessions();
    Say(port, "storage: %s on %s (%s)\n", g.device, STORAGE_STICK, g.fstype);
  } else if (strcmp(word, "umount") == 0) {
    if (UnmountStick() != 0) {
      Notice(port, "!storage: %s", g.error);
      End(port, 2);
      return;
    }
    Say(port, "storage: stick released\n");
  } else if (strcmp(word, "use") == 0) {
    char target[kNameMax];
    if (!NameArg(rest + n, target, sizeof target)) {
      Notice(port, "!storage: use needs a session name");
      End(port, 2);
      return;
    }
    UseSession(target, port);
    return;
  } else if (strcmp(word, "back") == 0) {
    ReturnInternal(port);
    return;
  } else if (strcmp(word, "check") == 0) {
    char target[kNameMax];
    if (!NameArg(rest + n, target, sizeof target)) {
      Notice(port, "!storage: check needs a session name");
      End(port, 2);
      return;
    }
    CheckReply(port, target);
    return;
  } else if (strcmp(word, "help") == 0) {
    HelpReply(port);
  } else {
    Notice(port, "!storage: no such word");
    Logf("no such storage word: %s", word);
    End(port, 2);
    return;
  }

  End(port, 0);
}

void storage_midi_in(void* port, const unsigned char* data, size_t n) {
  static const char kPrefix[] = "@storage";
  const size_t plen = sizeof kPrefix - 1;

  for (size_t i = 0; i < n; ++i) {
    if (g.reader.Feed(data[i])) {
      const char* cmd = g.reader.Command();
      if (strncmp(cmd, kPrefix, plen) != 0) continue;
      if (cmd[plen] != '\0' && cmd[plen] != ' ') continue;
      AtStorage(port, cmd[plen] == ' ' ? cmd + plen + 1 : "");
      continue;
    }
    {
      const char* cmd = g.reader.Command();
      if (strncmp(cmd, kPrefix, plen) == 0 &&
          (cmd[plen] == '\0' || cmd[plen] == ' ')) {
        const char* text = borgwire::RefusalText(g.reader.TakeRefusal());
        if (text != nullptr) Notice(port, "!storage: %s", text + 1);
      }
    }
  }
}

int storage_init(const borg_host* host, const char* module_path) {
  (void)module_path;
  g.host = host;
  DetectActive();
  g.mounted = IsMountPoint(STORAGE_STICK);
  if (g.mounted) MountStick();  /* fills in the source and type */
  /* Nothing is mounted on load. The stick is mounted when the page is entered
   * or a wire request asks for it, so loading the module while a disk is
   * plugged in does not start touching it. */
  g.need_scan = false;
  g.retry = 0;
  Logf("storage module ready; %s", g.active[0] != '\0' ? "session active"
                                                        : "internal userfs");
  return 0;
}

void storage_page() {
  DrawPage();
}

const borg_module storage_module = {
  BORG_MODULE_ABI,
  "STORAGE",
  "1.0.0",
  BORG_CAP_PAGE | BORG_CAP_MIDI | BORG_CAP_INPUT,
  storage_init,
  storage_page,
  storage_midi_in,
  storage_tick,
  storage_page_enter,
  storage_page_leave,
  storage_key,
  storage_encoder,
  nullptr,  // audio_render (ABI 4)
  nullptr,  // midi_msg (ABI 4)
  nullptr   // midi_transform (ABI 5)
};

}  // namespace

extern "C" const borg_module* borg_module_entry(uint32_t host_abi) {
  if (host_abi < 3) return nullptr;
  return &storage_module;
}
