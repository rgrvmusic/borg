/**
 * @file mk2_shim.c
 * @brief The object the unit's switch preloads into microkorgd on the microKORG2.
 *
 * Three jobs, and nothing else:
 *
 * 1. Mark the attachment. The unit's next Init is the same unit in the same
 *    process after execve, and LOGUE_BORG_PID == getpid() is how it knows not
 *    to ask for the switch again. The marker and the log are files under /tmp,
 *    which is tmpfs: a switch that runs on every boot must not spend the
 *    instrument's flash on its own bookkeeping. (borg_shim.c on the drumlogue
 *    also copies its attach line to userfs; that is deliberately not done here,
 *    for the same reason.)
 *
 * 2. Watch the panel. microkorgd owns /dev/ttymxc2, so a unit cannot read the
 *    keys itself. This object interposes the open/read and stdio entry points
 *    the daemon uses on that descriptor, passes every byte through unchanged,
 *    and recognises the panel's key frame
 *
 *      80 <key> <pressed> 00 00
 *
 *    -- five bytes, the last two zero; the device's own trace records them
 *    (Programs/mk2trace.log, `80 0d 01 00 00`). When FUNCTION 3 (0x0f) and
 *    FUNCTION 5 (0x11) are down together, that press is not the daemon's: it is
 *    rewritten to FUNCTION 2, so microkorgd opens its COPY screen underneath,
 *    and the unit is called from the write() hook once that page has gone out
 *    -- which is what draws the menu on top of it (see borg_api.h and
 *    borg_open_if_ready below). While the menu is up every key is withheld and
 *    handed to the unit instead, except FUNCTION 5: that press is COPY's BACK,
 *    so it is forwarded, microkorgd cancels the view, and its repaint is what
 *    takes the menu off the display.
 *
 *    THAT LINK WAS A FILE, AND A FILE COST THREE BOARD RUNS. The chord used to
 *    be published to /tmp and polled by the unit at 12 Hz. The key log showed
 *    the shim see the chord and withhold it every time while the unit never saw
 *    the marker, so the menu never opened -- a level sampled by two independent
 *    clocks loses an event as short as a button press. The unit and this object
 *    are in the same process; the unit registers with this one at Init and the
 *    link is a function call.
 *
 * 3. Record the panel's downlink: every write() to that descriptor, in hex, to
 *    /tmp/logue-borg-panelw.log. This is the direction no capture on file has
 *    ever contained, and it is what settles the frame layout the unit writes.
 *    Capped, and read back over the exec channel, so it costs no flash.
 *
 * WHY NO dlopen/dlsym. This runs before the daemon has initialised its
 * allocator and its libdl, and a dlsym(RTLD_NEXT) from a constructor can
 * recurse through the PLT it is trying to resolve. Raw syscalls for I/O and
 * direct _IO_* calls for stdio need no loader lookup, which is also why the
 * rgexec tracing object works on this platform.
 *
 * A FAILED LOG OR CHORD OPEN IS SILENT. If /tmp is not writable the daemon is
 * left exactly as it was and the switch becomes an expensive no-op. A preload
 * that refuses to let the instrument boot is worse than one that does nothing.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef LOGUE_BORG_LOG
#define LOGUE_BORG_LOG "/tmp/logue-borg.log"
#endif
#ifndef LOGUE_BORG_MARK
#define LOGUE_BORG_MARK "/tmp/logue-borg.active"
#endif
#ifndef LOGUE_BORG_KEYS
#define LOGUE_BORG_KEYS "/tmp/logue-borg-keys.log"
#endif
#ifndef LOGUE_BORG_PANELW
#define LOGUE_BORG_PANELW "/tmp/logue-borg-panelw.log"
#endif

#include "borg_api.h"

/* Large enough for the attach line: image up to 255 bytes and the inherited
 * LD_PRELOAD up to 511, plus the fixed text. 512 was not, and the native test
 * build said so under -Wformat-truncation. */
#define BORG_LINE 1024
#define BORG_WATCH 8

/* FUNCTION 3 and FUNCTION 5 for the chord, FUNCTION 1-4 for the menu. The
 * panel's function keys are ids 0x0d-0x11 for FUNCTION 1-5 -- the same ids the
 * boot code gets back in the six-byte `86 03 11` boot-switch reply
 * (docs/microkorg2-bootarbiterd-panel.md in logue-diagnostic) -- and the uplink
 * frame is the `80 <key> <pressed>` the device's trace records.
 *
 * FUNCTION 3, NOT FUNCTION 1. Shifted FUNCTION 1 is microkorgd's "Initialize
 * Target" -- the daemon's confirmation screen is one key away from wiping the
 * program -- while shifted FUNCTION 3 is the MANUAL QR screen, which changes
 * nothing. The chord takes the harmless one. */
#define BORG_KEY_FUNC1 0x0d
#define BORG_KEY_FUNC2 0x0e
#define BORG_KEY_FUNC3 0x0f
#define BORG_KEY_FUNC4 0x10
#define BORG_KEY_FUNC5 0x11

struct borg_watch {
  int fd;
};

static int g_log = -1;
static struct borg_watch g_watch[BORG_WATCH];
static int g_nwatch;

static unsigned char g_pressed[256];
static int g_key_lines = 0;

/* The daemon's own soft-key labels, as they left it (MANUAL, not the BORG this
 * object rewrites on the way out). Kept here rather than in the unit because
 * this is the side that sees the downlink; published to the unit at
 * registration and on every change. */
static unsigned char g_label[BORG_API_LABELS][BORG_API_LABEL_MAX];
static int g_label_known[BORG_API_LABELS];

/* WHICH HOOK THE KEYS ARRIVED ON. Two board runs were spent on "the shim saw
 * the chord but the daemon still acted on it", and the one fact neither the key
 * log nor the downlink log carried was the read path: `r` is read(2), `f` is
 * fread, `g` is fgets. Without it, "the filter did not run" and "the filter ran
 * and did not match" are indistinguishable on the instrument -- and on the
 * board it was the installed object that turned out to be stale, a question a
 * one-letter tag answers immediately. */
static char g_key_path = '?';
static size_t g_key_bytes;   /* size of the buffer that carried them */

/* The build this object is. Hashes settle it from a computer, but the person
 * at the instrument has no hash to compare: this string is in the marker file,
 * which `@exec cat /tmp/logue-borg.active` prints in one short answer. Bump it
 * whenever the behaviour changes. */
#define BORG_BUILD "2026-11-01 menu-over-copy"

/* Stream parser state, shared across read chunks: a frame can be split by the
 * daemon's read size, and the two zero bytes after a key frame are padding the
 * parser must skip without treating them as a new class. */
static int g_class;
static int g_remain;
static int g_len_pending;
static unsigned char g_key;

/* ---- raw I/O ------------------------------------------------------------- */

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

static int borg_open_raw(const char* path, int flags) {
  return (int)syscall(SYS_openat, AT_FDCWD, path, flags, 0644);
}

static void borg_line(const char* s) {
  if (g_log < 0) return;
  borg_write_all(g_log, s, strlen(s));
}

static void borg_image(char* out, size_t n) {
  const long got = syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", out,
                           n - 1);
  if (got <= 0) {
    snprintf(out, n, "?");
    return;
  }
  size_t len = (size_t)got;
  if (len > n - 1) len = n - 1;
  out[len] = '\0';
}

/* ---- the unit's registration --------------------------------------------- */

/* The struct lives in the unit; this object only keeps the pointer. It is set
 * from the unit's Init through borg_shim_register below and cleared from its
 * teardown, so a program change that unloads the unit cannot leave a call into
 * freed code behind. */
static borg_unit_api* g_api;

static void borg_notify_chord(int down) {
  if (g_api != NULL && g_api->on_chord != NULL) g_api->on_chord(down);
}

static void borg_notify_key(unsigned key) {
  if (g_api != NULL && g_api->on_key != NULL) g_api->on_key(key);
}

static int borg_menu_open(void) {
  return g_api != NULL && g_api->menu_open != NULL && *g_api->menu_open != 0;
}

/* Called by the unit at its Init (dlsym, see borg_api.h). Returns 1 when this
 * object takes the struct; 0 means the unit should run without a menu. */
int borg_shim_register(borg_unit_api* api) {
  if (api == NULL || api->version != BORG_API_VERSION) return 0;
  g_api = api;

  /* Whatever the daemon has already drawn is given to the unit now, so a menu
   * opened before the next page change still restores the instrument's own
   * labels on the way out. */
  if (api->labels != NULL && api->label_known != NULL) {
    for (int i = 0; i < BORG_API_LABELS; ++i) {
      if (g_label_known[i]) {
        memcpy(api->labels[i], g_label[i], BORG_API_LABEL_MAX);
        api->label_known[i] = 1;
      } else {
        api->label_known[i] = 0;
      }
    }
  }
  return 1;
}

void borg_shim_unregister(borg_unit_api* api) {
  if (g_api == api) g_api = NULL;
}

/* ---- the chord state ----------------------------------------------------- */

/* The key log is for the board: it is how a person confirms which index a
 * physical key sends, on the instrument, without a second switch. Releases are
 * logged too -- a log of presses alone cannot show whether two keys were ever
 * down together, which is exactly what a chord question turns on. It is capped
 * so a long session cannot grow a file on tmpfs without bound. */
static void borg_key_line(unsigned char key, int pressed) {
  if (g_key_lines >= 240) return;
  ++g_key_lines;
  const int fd = borg_open_raw(LOGUE_BORG_KEYS,
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
  if (fd < 0) return;
  char line[96];
  const int n = snprintf(line, sizeof line, "key %02x %s %c %u\n", key,
                         pressed ? "pressed" : "released", g_key_path,
                         (unsigned)g_key_bytes);
  if (n > 0) borg_write_all(fd, line, (size_t)n);
  syscall(SYS_close, fd);
}

/* THE FILTER'S OWN VERDICT, IN THE SAME LOG. "The shim saw the chord" and "the
 * shim stopped it" are different claims, and the key log alone cannot tell them
 * apart: it is written while the buffer is parsed, before the filter runs. This
 * line records the decision and the shift state at the moment of the decision,
 * which is what identifies a chord that arrived and ended inside one stdio
 * buffer -- fread fills its whole request before returning, so a press and its
 * release can share a buffer with the key they were meant to modify. */
static void borg_filter_line(unsigned char key, int pressed, int drop,
                             int shift_held) {
  if (g_key_lines >= 240) return;
  ++g_key_lines;
  const int fd = borg_open_raw(LOGUE_BORG_KEYS,
                               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
  if (fd < 0) return;
  char line[96];
  const int n = snprintf(line, sizeof line, "%s %02x %d shift=%d %c %u\n",
                         drop ? "drop" : "pass", key, pressed, shift_held,
                         g_key_path, (unsigned)g_key_bytes);
  if (n > 0) borg_write_all(fd, line, (size_t)n);
  syscall(SYS_close, fd);
}

static void borg_key_event(unsigned char key, int pressed) {
  if (pressed != 0 && pressed != 1) return;
  g_pressed[key] = (unsigned char)pressed;
  borg_key_line(key, pressed);

  /* THE CHORD DOES NOT OPEN THE MENU HERE. It used to: this is the thread that
   * saw the key, and a call from the read that carried it was the shortest path.
   * But the menu is now drawn over a view the daemon opens in answer to that
   * same read, and calling the unit first means the daemon's page lands on top
   * of the console. The filter marks the press and write() calls the unit once
   * the page has gone out -- see borg_open_if_ready. The state above is still
   * kept: which keys are down, walked in stream order, is what the filter
   * consults to recognise the chord at all. */
}

/* Frames on the panel's uplink, by class. Only 0x80 carries key events; the
 * other lengths exist so their payload cannot be mistaken for one. 0x86 is
 * self-length (second byte is the whole frame length). Everything else is a
 * byte to skip. */
static void borg_feed(const unsigned char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char b = p[i];

    if (g_len_pending) {
      g_len_pending = 0;
      g_remain = b >= 2 ? (int)b - 2 : 0;
      continue;
    }
    if (g_remain > 0) {
      if (g_class == 0x80) {
        if (g_remain == 2) {
          g_key = b;
        } else if (g_remain == 1) {
          borg_key_event(g_key, b);
        }
      }
      --g_remain;
      continue;
    }

    switch (b) {
      case 0x80: g_class = 0x80; g_remain = 2; break;  /* 80 key press */
      case 0x81: g_class = 0x81; g_remain = 2; break;  /* 81 enc delta */
      case 0x82: g_class = 0x82; g_remain = 3; break;  /* 82 pot lo hi */
      case 0x83: g_class = 0x83; g_remain = 2; break;
      case 0x86: g_class = 0x86; g_len_pending = 1; break;
      case 0x87: break;                                /* one-byte liveness */
      case 0x00: break;                                /* frame padding */
      default: break;
    }
  }
}

/* ---- the menu ------------------------------------------------------------ */

/* THE MENU IS BORG'S OWN UI, AND microkorgd DOES NOT KNOW ABOUT IT. The panel's
 * button row under the display is five labels, and the first four are the
 * FUNCTION 1-4 keys. The unit writes its own labels into them (id 0x36, fields
 * 0-3 -- the same message microkorgd uses for INIT/COPY/MANUAL) and the keys
 * those buttons send are ids 0x0d-0x10, which this shim already sees on the
 * daemon's reads.
 *
 * Two things have to happen for that to be a menu rather than four labels drawn
 * over the instrument's own: microkorgd must not act on those keys, and the
 * daemon's own labels must be given back when the menu closes. So while the
 * unit's menu flag is up the FUNCTION 1-5 frames are withheld from the daemon's
 * reads and handed to the unit (borg_notify_key), and the daemon's 0x36 frames
 * are recorded for the unit to restore.
 *
 * NOT FILES. Both of those used to be /tmp files, which the unit polled; see
 * the note at the top of this file and borg_api.h. The state is the unit's own
 * flag, read through the pointer it registered here, and the events are calls
 * into it. Nothing in this section does I/O. */

/* The daemon's own button labels, kept so the unit can put them back. Only
 * frames wholly inside one write are read: a label is resent whenever the
 * daemon redraws the row, so a split frame costs nothing. `count` is what the
 * daemon sends -- the exact number of characters -- and the rest of the frame
 * is padding.
 *
 * Published straight into the unit's arrays through the registered pointer,
 * because the unit reads them exactly once, when the menu closes. */
static void borg_labels_capture(const unsigned char* p, size_t n) {
  size_t i = 0;
  int changed = 0;
  while (i + 1 < n) {
    if (p[i] != 0x85) { ++i; continue; }
    const size_t len = p[i + 1];          /* whole frame, self-inclusive */
    if (len < 5 || i + len > n) { ++i; continue; }
    if (p[i + 2] == 0x36) {
      const unsigned field = p[i + 3];
      const size_t count = p[i + 4];
      if (field < BORG_API_LABELS && count < sizeof g_label[0] &&
          5 + count <= len) {
        memcpy(g_label[field], p + i + 5, count);
        g_label[field][count] = '\0';
        g_label_known[field] = 1;
        changed = 1;
      }
    }
    i += len;
  }
  if (changed && g_api != NULL && g_api->labels != NULL &&
      g_api->label_known != NULL) {
    for (int i = 0; i < BORG_API_LABELS; ++i) {
      if (g_label_known[i]) {
        memcpy(g_api->labels[i], g_label[i], BORG_API_LABEL_MAX);
        g_api->label_known[i] = 1;
      }
    }
  }
}

/* Which key frames are the chord: FUNCTION 3 going down while FUNCTION 5 is
 * held. Nothing else.
 *
 * SHIFT FIRST, AND ONLY SHIFT FIRST. FUNCTION 5 is the daemon's shift key and
 * is held while the second key is pressed -- that is how INIT, COPY and MANUAL
 * have always been reached -- so that second press is the one this filter can
 * see coming. A chord pressed the other way round -- FUNCTION 3 first, then
 * FUNCTION 5 -- is the daemon's: by the time FUNCTION 5 arrives the first key
 * has already opened MANUAL, and rewriting the shift key then would leave
 * microkorgd believing FUNCTION 5 was never pressed. It is forwarded untouched,
 * no menu opens, and the instrument behaves exactly as it did before this
 * object existed. */
static int borg_chord_key(const unsigned char* st, unsigned char key,
                          int pressed) {
  if (!pressed) return 0;
  if (key != BORG_KEY_FUNC3) return 0;
  return st[BORG_KEY_FUNC5] != 0;
}


/* THE HOLD THAT WOULD HAVE COVERED THE OTHER ORDER WAS WRITTEN AND REMOVED: it
 * has to keep a press back until its release, and a caller whose read buffer is
 * exactly full leaves no room to put the press back in front of what followed,
 * which would invert the keys instead. A documented order beats a filter that
 * silently reorders input. */
/* THE FILTER KEEPS PARSER STATE ACROSS CALLS, AND THAT IS THE WHOLE FIX.
 *
 * The first version looked for complete `80 <key> <pressed>` frames inside the
 * single buffer it was handed and removed them. That works only if the caller
 * reads at least a whole frame at a time, and the board showed a caller that
 * does not: the key log -- which has always carried its parse state from one
 * call to the next -- reported the chord press and release perfectly, while the
 * daemon still acted on the button. A caller reading the stream in pieces
 * never presents a complete frame, so the filter matched nothing and removed
 * nothing.
 *
 * The second half of that failure is the decision point. Even with whole
 * frames, borg_feed parses the whole buffer before the filter runs, so the
 * filter read end-of-buffer state: a chord whose press and release shared a
 * buffer looked, at decision time, like a chord that had already been let go.
 * The filter therefore carries its own copy of the pressed state, walked in
 * stream order, starting from the state before this buffer.
 *
 * THE FRAME IS ERASED TO PADDING, NOT TURNED INTO A RELEASE. The first attempt
 * rewrote the pressed byte to zero -- `80 <key> 00`, a release -- on the
 * assumption that a daemon which never saw the press would ignore its release.
 * The board said otherwise: SCOPE opened on the chord, because the release of
 * the button is itself an event to microkorgd. A key that must be invisible has
 * to be invisible in both directions, so all three bytes of the frame become
 * zero, which the panel protocol already uses as padding, and the release of a
 * frame that was shielded is shielded too.
 *
 * THE BYTES ARE NEVER REMOVED: removing them would shrink the return value, and
 * a short return from fread means EOF to the caller. Zeroing the class byte as
 * well is what makes the result padding rather than `80 00 00`, whose key 0 is
 * not a key this filter has any reason to trust. */
static int f_class;
static int f_remain;
static int f_len_pending;
static unsigned char f_key;
static long f_key_pos;          /* index of the key byte in this call, or -1 */
static int f_frame_shield;      /* the frame being parsed is shielded */
static int f_frame_menu;        /* it is one of the menu's keys */
static int f_frame_close;       /* it is the key that closes the menu */
static int f_remap;             /* this frame's key, rewritten, or 0 */
static unsigned char f_remap_hold[256];  /* its release is rewritten too */
static unsigned char f_shield[256];  /* its release must be shielded too */

/* THE MENU NEEDS A VIEW UNDER IT, AND microkorgd NEEDS A MOMENT TO DRAW IT.
 *
 * Set when the chord's second press is forwarded; cleared from write(), once
 * the daemon has painted the view that press opened. Until then the unit's own
 * flag is still down, so borg_menu_open() is false and the menu has not been
 * drawn -- and it must not be drawn, because the console line would be the
 * daemon's page to paint over and its soft-key row would land on top of the
 * menu's. Drawn afterwards, the row is rewritten by this object as it leaves
 * and the console outlives the page.
 *
 * The fallback deadline exists because the alternative to a menu drawn a little
 * early is a panel where every key is eaten and nothing was ever drawn: while
 * pending is set, borg_filter treats the menu as up. If the daemon does not
 * paint a page the scan recognises, the menu goes up anyway on the next panel
 * write after the deadline, and FUNCTION 5 still reaches the daemon as BACK. */
static volatile int g_open_pending;
static long g_open_since;       /* monotonic ms, 0 when the clock failed */
#define BORG_OPEN_GRACE_MS 250

static long borg_now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* A page the daemon painted, as far as this hook can tell from one buffer:
 * 0x36 is the soft-key row and 0x19 is the view-change marker sent when a
 * shifted screen opens. Either means microkorgd has answered the forwarded key.
 * The scan is deliberately loose about frame bounds -- this only decides WHEN
 * to draw the menu, and a byte pattern in the middle of a text field that
 * happens to match makes the menu appear, not disappear. */
static int borg_view_painted(const unsigned char* p, size_t n) {
  for (size_t i = 0; i + 3 <= n; ++i) {
    if (p[i] != 0x85) continue;
    if (p[i + 2] == 0x36 || p[i + 2] == 0x19) return 1;
  }
  return 0;
}

/* Draw the menu now, if the chord asked for it and the daemon has had its say.
 * Called at the END of the panel write, after every byte of the daemon's page
 * has gone out: the unit's console line and its labels are written from in
 * here, and they have to leave after the page they are drawn over. */
static void borg_open_if_ready(const unsigned char* p, size_t n) {
  if (!g_open_pending) return;
  if (!borg_view_painted(p, n)) {
    const long now = borg_now_ms();
    if (g_open_since != 0 && now != 0 &&
        now - g_open_since < BORG_OPEN_GRACE_MS)
      return;
    borg_line("menu: open on the grace deadline, no page seen\n");
  } else {
    borg_line("menu: open over the daemon's page\n");
  }
  /* Cleared BEFORE the call: the unit's own labels are written from inside it,
   * and they re-enter this object's write(). The unit raises its flag at the
   * top of OpenMenu, so from the first of those writes on the row is rewritten
   * as the menu's. */
  g_open_pending = 0;
  borg_notify_chord(1);
}

static size_t borg_filter(unsigned char* p, size_t n, unsigned char* st) {
  f_key_pos = -1;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char b = p[i];

    if (f_len_pending) {
      f_len_pending = 0;
      f_remain = b >= 2 ? (int)b - 2 : 0;
      continue;
    }
    if (f_remain > 0) {
      if (f_class == 0x80) {
        if (f_remain == 2) {
          f_key = b;
          f_key_pos = (long)i;
          /* The whole frame is decided at the key byte, because the chord
           * depends only on this key and the state before it. Both the press
           * and the release of a chord key are shielded while the other half
           * is held, and a key whose press was shielded keeps its release
           * shielded even if the other half was let go first. */
          /* WHILE THE MENU IS OPEN THE PANEL IS THE MENU'S, SO EVERY KEY IS
           * WITHHELD -- and FUNCTION 5 is the one exception, because it is the
           * daemon's BACK/CANCEL in every view this instrument has, and the
           * menu is drawn over one of them. Forwarded, it cancels that view,
           * and the daemon's own repaint is what takes the console menu off the
           * display and puts the instrument's labels back. Nothing of ours is
           * left to undo. Everything else, encoders and the function row
           * included, is eaten. */
          const int menu_up = borg_menu_open() != 0 || g_open_pending != 0;
          /* A key whose press was rewritten to another key has a release in
           * flight that says neither FUNCTION 3 nor FUNCTION 2. It belongs to
           * the daemon once, under the name the daemon was given, so it is
           * neither a menu key nor shielded. */
          const int remap_hold = f_remap_hold[f_key] != 0;

          f_frame_close = menu_up != 0 && f_key == BORG_KEY_FUNC5;
          f_frame_menu = menu_up != 0 && !f_frame_close && !remap_hold &&
                         f_shield[f_key] == 0;
          f_frame_shield = f_frame_menu || f_shield[f_key] != 0;

          /* THE SECOND HALF OF THE CHORD IS NOT THE DAEMON'S, BUT ITS KEY IS
           * NOT OURS EITHER. FUNCTION 5 + FUNCTION 3 is the daemon's MANUAL,
           * and MANUAL is a QR code -- but the view that is proven to cancel
           * with FUNCTION 5 is COPY, FUNCTION 5 + FUNCTION 2. So the second
           * press is rewritten from FUNCTION 3 to FUNCTION 2 on its way to the
           * daemon: microkorgd opens COPY underneath, untouched, and it is COPY
           * that FUNCTION 5 cancels when the menu closes and the daemon repaints
           * what it had before. THAT REPAINT IS THE CLEANUP: the console cannot
           * be cleared from here, so the view the daemon restores is the only
           * thing that takes the menu off the display.
           *
           * The rewrite is done on the key byte and remembered for the release,
           * because a daemon that is told FUNCTION 2 went down and FUNCTION 3
           * came up has a key held down for ever. */
          f_remap = 0;
          if (!f_frame_shield) {
            if (menu_up == 0 && borg_chord_key(st, f_key, 1)) {
              f_remap = BORG_KEY_FUNC2;
              if (!g_open_pending) {
                g_open_pending = 1;
                g_open_since = borg_now_ms();
              }
            } else {
              f_remap = f_remap_hold[f_key];
            }
          }
          if (f_remap != 0) {
            f_remap_hold[f_key] = (unsigned char)f_remap;
            p[i] = (unsigned char)f_remap;
          }

          /* The key byte -- and the class byte, when this buffer still holds
           * it -- is zeroed now, not when the press is known: a caller that
           * hands the stream over in pieces has already been given a byte by
           * the time the next one arrives, and these are the last bytes this
           * filter can take back. With the whole frame in one buffer that
           * leaves pure padding; across a split, if the class byte has already
           * gone, what the daemon parses is `80 00 00` -- key 0, which the
           * panel never sends -- rather than the button's release. */
          if (f_frame_shield) {
            p[i] = 0;
            if (f_key_pos > 0) p[f_key_pos - 1] = 0;
          }
        } else if (f_remain == 1) {
          const int pressed = b != 0;
          if (f_frame_shield) {
            p[i] = 0;                      /* the pressed byte */
            if (f_key_pos > 0) {
              p[f_key_pos] = 0;            /* the key byte */
              p[f_key_pos - 1] = 0;        /* the class byte: now padding */
            }
          }
          if (pressed && (f_frame_menu || f_frame_close) &&
              f_key >= BORG_KEY_FUNC1 && f_key <= BORG_KEY_FUNC5)
            borg_notify_key(f_key);
          borg_filter_line(f_key, pressed, f_frame_shield,
                           st[BORG_KEY_FUNC5] != 0);
          if (f_frame_shield && pressed) f_shield[f_key] = 1;
          if (!pressed) f_shield[f_key] = 0;
          /* The release settles the rewrite: the daemon now knows both halves
           * of the key it was given, so the original name is free again. */
          if (!pressed) f_remap_hold[f_key] = 0;
          st[f_key] = pressed ? 1 : 0;
          f_key_pos = -1;
          f_frame_shield = 0;
          f_frame_menu = 0;
          f_frame_close = 0;
        }
      }
      --f_remain;
      continue;
    }

    switch (b) {
      case 0x80: f_class = 0x80; f_remain = 2; break;
      case 0x81: f_class = 0x81; f_remain = 2; break;
      case 0x82: f_class = 0x82; f_remain = 3; break;
      case 0x83: f_class = 0x83; f_remain = 2; break;
      case 0x86: f_class = 0x86; f_len_pending = 1; break;
      case 0x87: case 0x00: default: break;
    }
  }
  return n;
}

/* One entry point for the three hooks: decide from the state as it was before
 * this buffer, then let the parser (which writes the key log and the chord
 * file) advance the shared state. */
static void borg_consume(unsigned char* p, size_t n, char path) {
  unsigned char base[256];
  memcpy(base, g_pressed, sizeof base);
  g_key_path = path;
  g_key_bytes = n;
  borg_feed(p, n);
  borg_filter(p, n, base);
}

/* ---- the panel's downlink ------------------------------------------------ */

/* WHAT THE DAEMON ACTUALLY SENDS. The reads above are the panel talking to the
 * daemon; this is the other direction, and until it was recorded nothing here
 * had ever seen a byte of it. The only panel stream on file
 * (logue-diagnostic/emulator/captures/mk2-links2.links.txt) logs the daemon's
 * read()s alone -- `grep -c tx` is 0 -- so every frame layout written into
 * borg.cc was derived from the wrong direction. That is why a message came out
 * six characters short: the panel was being handed the drumlogue's box header
 * and read the text where its own header would have ended.
 *
 * This is a diagnostic, not a feature: it is capped, it is one hex line per
 * write, and it is read back over the exec channel
 * (`borgexec.py run 'cat /tmp/logue-borg-panelw.log'`) rather than by pulling
 * files off flash. A panel write is cheap here -- the daemon writes a frame at
 * a time, the same discipline its own code follows. */
#define BORG_PANELW_MAX (16u * 1024u)
#define BORG_PANELW_LINE 64

static int g_panelw = -2;            /* -2 not opened, -1 failed, else fd */
static size_t g_panelw_bytes;
/* The daemon's heartbeat (`80 02 00 7f 00 80 03 00 16 00 80 04 00 14 02`) is
 * written continuously and filled two thirds of the first log. Repeats are
 * counted instead of printed, so what is left is the sequence of distinct
 * frames -- the thing being read -- and it fits in one page of the exec `cat`,
 * which is the only way a log leaves this instrument without touching flash. */
static unsigned char g_panelw_last[BORG_PANELW_LINE];
static size_t g_panelw_last_n;
static unsigned g_panelw_repeat;

static void borg_panelw_open(void) {
  if (g_panelw != -2) return;
  g_panelw = borg_open_raw(LOGUE_BORG_PANELW,
                           O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
}

/* `repeat` is the number of extra identical writes already collapsed into the
 * line being emitted; it is printed only when it is not zero. */
static void borg_panelw_line(const unsigned char* p, size_t n, unsigned repeat) {
  static const char hex[] = "0123456789abcdef";
  static const char nl = '\n';
  char line[BORG_PANELW_LINE * 3 + 40];
  size_t i;

  if (g_panelw < 0 || g_panelw_bytes >= BORG_PANELW_MAX) return;

  if (repeat != 0) {
    i = (size_t)snprintf(line, sizeof line, "w %ux%u]", (unsigned)n, repeat);
  } else {
    i = (size_t)snprintf(line, sizeof line, "w %u]", (unsigned)n);
  }
  if (i > sizeof line - 1) i = sizeof line - 1;
  for (size_t k = 0; k < n && k < BORG_PANELW_LINE; ++k) {
    if (i + 4 >= sizeof line) break;
    line[i++] = ' ';
    line[i++] = hex[p[k] >> 4];
    line[i++] = hex[p[k] & 0x0f];
  }
  if (n > BORG_PANELW_LINE && i + 3 < sizeof line) {
    line[i++] = ' ';
    line[i++] = '.';
  }
  borg_write_all(g_panelw, line, i);
  borg_write_all(g_panelw, &nl, 1);
  g_panelw_bytes += i;
}

static void borg_panelw_dump(const unsigned char* p, size_t n) {
  borg_panelw_open();
  if (g_panelw < 0 || g_panelw_bytes >= BORG_PANELW_MAX) return;

  if (g_panelw_last_n != 0 && g_panelw_last_n == n &&
      memcmp(g_panelw_last, p, n) == 0) {
    ++g_panelw_repeat;
    return;
  }
  if (g_panelw_last_n != 0) {
    borg_panelw_line(g_panelw_last, g_panelw_last_n, g_panelw_repeat);
    g_panelw_repeat = 0;
  }
  g_panelw_last_n = n < BORG_PANELW_LINE ? n : BORG_PANELW_LINE;
  memcpy(g_panelw_last, p, g_panelw_last_n);
}

/* ---- the watch table ----------------------------------------------------- */

static int borg_is_panel(const char* path) {
  return path != NULL && strstr(path, "ttymxc2") != NULL;
}

static void borg_add(int fd) {
  if (fd < 0 || g_nwatch >= BORG_WATCH) return;
  for (int i = 0; i < g_nwatch; ++i) {
    if (g_watch[i].fd == fd) return;
  }
  g_watch[g_nwatch++].fd = fd;
}

static int borg_find(int fd) {
  for (int i = 0; i < g_nwatch; ++i) {
    if (g_watch[i].fd == fd) return 1;
  }
  return 0;
}

static void borg_forget(int fd) {
  for (int i = 0; i < g_nwatch; ++i) {
    if (g_watch[i].fd == fd) {
      g_watch[i] = g_watch[g_nwatch - 1];
      --g_nwatch;
      return;
    }
  }
}

/* ---- attachment ---------------------------------------------------------- */

/* Remove this shim from LD_PRELOAD while preserving the other entries, so the
 * daemon's children -- the shell scripts microkorgd runs, busybox -- do not
 * inherit a panel watcher they have no business in. */
static void borg_forget_preload(const char* preload) {
  static const char kBase[] = "libborg.so";
  const size_t base_len = sizeof kBase - 1;
  char original[BORG_LINE];
  char kept[BORG_LINE];
  size_t t = 0;

  if (preload == NULL || preload[0] == '\0') return;
  snprintf(original, sizeof original, "%s", preload);

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

__attribute__((constructor)) static void borg_attach(void) {
  char image[256];
  char line[BORG_LINE];
  char pidbuf[32];
  char preload[512];
  const int pid = (int)getpid();
  const int uid = (int)getuid();
  const long now = (long)time(NULL);

  borg_image(image, sizeof image);

  /* Copy before setenv can reallocate environ. */
  {
    const char* in = getenv("LD_PRELOAD");
    snprintf(preload, sizeof preload, "%s", in != NULL ? in : "");
  }

  g_log = borg_open_raw(LOGUE_BORG_LOG,
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC);
  snprintf(line, sizeof line,
           "[borg] attached pid=%d uid=%d image=%s preload=%s\n", pid, uid,
           image, preload[0] != '\0' ? preload : "(unset)");
  borg_line(line);

  const int fd = borg_open_raw(LOGUE_BORG_MARK,
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
  if (fd >= 0) {
    snprintf(line, sizeof line,
             "pid %d\nuid %d\nattached %ld\nimage %s\nbuild %s\n", pid, uid,
             now, image, BORG_BUILD);
    borg_write_all(fd, line, strlen(line));
    syscall(SYS_close, fd);
  }

  /* The marker the unit checks. LOGUE_BORG_PID is a pid, not a flag: a stale
   * value from before a power cycle cannot match a new process. */
  snprintf(pidbuf, sizeof pidbuf, "%d", pid);
  setenv("LOGUE_BORG_ACTIVE", "1", 1);
  setenv("LOGUE_BORG_PID", pidbuf, 1);

  /* Start from "not held". Nothing on the panel has been read yet at this
   * point, and the unit has not registered -- it is dlopen'd by the daemon
   * after this constructor returns. */
  memset(g_pressed, 0, sizeof g_pressed);
  g_open_pending = 0;

  /* One boot, one downlink log: the daemon's startup burst is the most
   * informative thing it will ever write, and appending would bury it under
   * the previous run. The descriptor is reopened lazily on the first write. */
  {
    const int panelw = borg_open_raw(LOGUE_BORG_PANELW,
                                     O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
    if (panelw >= 0) syscall(SYS_close, panelw);
    g_panelw = -2;
    g_panelw_bytes = 0;
  }

  borg_forget_preload(preload);
}

/* ---- the hooks ----------------------------------------------------------- */

static int borg_open(int dirfd, const char* path, int flags, mode_t mode) {
  const int fd = (int)syscall(SYS_openat, dirfd, path, flags, mode);
  const int err = errno;
  if (fd >= 0 && borg_is_panel(path)) borg_add(fd);
  errno = err;
  return fd;
}

int open(const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return borg_open(AT_FDCWD, path, flags, mode);
}

int open64(const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
#ifdef O_LARGEFILE
  flags |= O_LARGEFILE;
#endif
  return borg_open(AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return borg_open(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char* path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
#ifdef O_LARGEFILE
  flags |= O_LARGEFILE;
#endif
  return borg_open(dirfd, path, flags, mode);
}

ssize_t read(int fd, void* buf, size_t count) {
  const long got = syscall(SYS_read, fd, buf, count);
  const int err = errno;
  if (got > 0 && borg_find(fd)) {
    borg_consume((unsigned char*)buf, (size_t)got, 'r');
  }
  errno = err;
  return (ssize_t)got;
}

/* THE THIRD BUTTON IS BORG'S. microkorgd labels its shifted row INIT / COPY /
 * MANUAL, and the third of those is the button the borg chord takes. Rewriting
 * the daemon's own label on the way out makes the instrument say BORG in that
 * button as soon as FUNCTION 5 is pressed -- the only hint the player gets
 * before trying the chord. Same frame, same length: the count byte and the zero
 * padding carry the shorter text, so nothing else in the burst moves.
 *
 * Only the exact MANUAL text is rewritten. The field index is not part of the
 * match: the capture this was written from had it at field 2, but the index is
 * the panel's own numbering and one capture is not a contract -- the text is
 * what says "this is the button". A field that says something else is a
 * different screen and is left alone. */
/* Offset and length of the next label frame to rewrite, and its new text, or
 * -1 for "nothing in this buffer".
 *
 * TWO REASONS TO REWRITE ONE FRAME.
 *
 * While the unit's menu is open, every field of the row becomes the unit's own
 * label -- including field 4, which the daemon leaves empty, and field 3, which
 * the unit leaves empty so the daemon's text cannot show through a button the
 * menu does not own -- so a page redraw by the daemon cannot take the menu off
 * the panel. That replaces the unit's once-a-second reassert, which only ran
 * when the render callback did -- and the callback is exactly what the board
 * could not confirm.
 *
 * Otherwise the exact text MANUAL becomes BORG. The field index is not part of
 * that match: the capture this was written from had MANUAL at field 2, but the
 * index is the panel's own numbering and one capture is not a contract. A field
 * that says something else is a different screen and is left alone. */
static size_t borg_find_label(const unsigned char* p, size_t n, size_t* len_out,
                              const char** text_out) {
  size_t i = 0;
  while (i + 5 <= n) {
    if (p[i] != 0x85) {
      ++i;
      continue;
    }
    const size_t len = p[i + 1];         /* whole frame, self-inclusive */
    if (len < 6 || len > 256 || i + len > n) {
      ++i;
      continue;
    }
    if (p[i + 2] == 0x36) {
      const unsigned field = p[i + 3];
      const size_t count = p[i + 4];
      const char* repl = NULL;
      if (borg_menu_open() && field < BORG_API_LABELS && g_api != NULL &&
          g_api->menu_labels[field] != NULL) {
        repl = g_api->menu_labels[field];
      } else if (count == 6 && memcmp(p + i + 5, "MANUAL", 6) == 0) {
        repl = "BORG";
      }
      if (repl != NULL && 5 + strlen(repl) <= len) {
        *len_out = len;
        *text_out = repl;
        return i;
      }
    }
    i += len;
  }
  return (size_t)-1;
}

/* One segment out to the panel, recorded exactly as it went out. */
static ssize_t borg_panel_write(int fd, const unsigned char* p, size_t n) {
  size_t done = 0;
  while (done < n) {
    const long wrote = syscall(SYS_write, fd, p + done, n - done);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return done != 0 ? (ssize_t)done : -1;
    }
    if (wrote == 0) break;
    borg_panelw_dump(p + done, (size_t)wrote);
    done += (size_t)wrote;
  }
  return (ssize_t)done;
}

/* The downlink, rewritten on its way to the panel and recorded as written.
 *
 * THE REWRITE IS SEGMENTED, BECAUSE THE BUFFER CAN BE LARGE. The first version
 * copied the whole buffer into a 256-byte shadow and skipped the rewrite
 * entirely for anything larger. On the board that was the whole of "MANUAL is
 * still there": the downlink log showed `4d 41 4e 55 41 4c` leaving the shim,
 * unmodified, and the write that carried it was a panel refresh far larger than
 * the shadow -- which the driver drains 64 bytes at a time, so the log made it
 * look like a 64-byte write and hid the size that had caused the skip.
 *
 * The daemon's buffer is not this object's to modify, so the parts around the
 * label are written in their own calls and only the frame itself is copied.
 * The panel parses a byte stream and already receives frames split across
 * writes, so segmenting changes nothing downstream. */
ssize_t write(int fd, const void* buf, size_t count) {
  if (!borg_find(fd)) return (ssize_t)syscall(SYS_write, fd, buf, count);

  const unsigned char* p = (const unsigned char*)buf;
  size_t start = 0;
  ssize_t total = 0;

  for (;;) {
    size_t len = 0;
    const char* text = NULL;
    const size_t at = borg_find_label(p + start, count - start, &len, &text);
    if (at == (size_t)-1) break;
    const size_t i = start + at;

    if (i > start) {
      const ssize_t w = borg_panel_write(fd, p + start, i - start);
      if (w < 0) return total > 0 ? total : -1;
      total += w;
      if ((size_t)w != i - start) return total;
    }

    {
      unsigned char frame[256];
      const size_t n = strlen(text);
      memcpy(frame, p + i, len);           /* same frame, same length */
      frame[4] = (unsigned char)n;         /* the count is the text's length */
      memcpy(frame + 5, text, n);
      memset(frame + 5 + n, 0, len - 5 - n);
      const ssize_t w = borg_panel_write(fd, frame, len);
      if (w < 0) return total > 0 ? total : -1;
      total += w;
      if ((size_t)w != len) return total;
    }
    start = i + len;
  }

  if (start < count) {
    const ssize_t w = borg_panel_write(fd, p + start, count - start);
    if (w < 0) return total > 0 ? total : -1;
    total += w;
  }

  /* While the menu is open these writes are the unit's own labels, not the
   * daemon's, and recording them would make the restore put the menu labels
   * back instead of the instrument's. Recorded from the daemon's buffer, so the
   * saved label is the instrument's MANUAL, not this object's BORG.
   *
   * NOT WHILE THE OPEN IS PENDING EITHER. The page that ends the wait is the
   * COPY screen the chord opened -- its row is DEC/INC, not the oscillator
   * page's -- and capturing it would make the fallback restore paint that row
   * onto the page the menu is drawn over. The last row captured is therefore
   * the one that was on the display before the chord, which is the row to put
   * back. */
  if (!borg_menu_open() && !g_open_pending) borg_labels_capture(p, count);

  /* AFTER the page, never before it. This is the chord's other half: the unit
   * is called here, on the only side of a panel write that is guaranteed to be
   * past the daemon's repaint, so the menu's console line and its labels leave
   * last and are what the display keeps. See borg_open_if_ready. */
  borg_open_if_ready(p, count);
  return total;
}

int close(int fd) {
  borg_forget(fd);
  return (int)syscall(SYS_close, fd);
}

int dup(int oldfd) {
  const int fd = (int)syscall(SYS_dup, oldfd);
  const int err = errno;
  if (fd >= 0 && borg_find(oldfd)) borg_add(fd);
  errno = err;
  return fd;
}

int dup2(int oldfd, int newfd) {
  const int follow = borg_find(oldfd);
  borg_forget(newfd);
  const int fd = (int)syscall(SYS_dup2, oldfd, newfd);
  const int err = errno;
  if (fd >= 0 && follow) borg_add(fd);
  errno = err;
  return fd;
}

int dup3(int oldfd, int newfd, int flags) {
  const int follow = borg_find(oldfd);
  borg_forget(newfd);
  const int fd = (int)syscall(SYS_dup3, oldfd, newfd, flags);
  const int err = errno;
  if (fd >= 0 && follow) borg_add(fd);
  errno = err;
  return fd;
}

/* ---- the stdio entry points ---------------------------------------------- */

/* glibc's own implementations, called directly. These are the public _IO_*
 * symbols the libc aliases point at; no loader lookup, see the file comment.
 * glibc's fopen uses an internal open64 alias that interposition does not see,
 * which is why this object reimplements fopen rather than only hooking open. */
extern size_t _IO_fread(void*, size_t, size_t, FILE*);
extern char* _IO_fgets(char*, int, FILE*);
extern int _IO_fclose(FILE*);

/* The mode string as open(2) flags: "r", "w", "a", an optional "+", and the
 * two letters that change meaning rather than capability, "x" and "e". */
static int borg_mode_flags(const char* mode) {
  if (mode == NULL || mode[0] == '\0') return -1;
  int flags;
  switch (mode[0]) {
    case 'r': flags = O_RDONLY; break;
    case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
  }
  if (strchr(mode, '+') != NULL) flags = (flags & ~O_WRONLY) | O_RDWR;
  if (strchr(mode, 'x') != NULL) flags |= O_EXCL;
#ifdef O_CLOEXEC
  if (strchr(mode, 'e') != NULL) flags |= O_CLOEXEC;
#endif
  return flags;
}

static FILE* borg_fopen_impl(const char* path, const char* mode) {
  const int flags = borg_mode_flags(mode);
  if (flags < 0) {
    errno = EINVAL;
    return NULL;
  }
  const int fd = (int)syscall(SYS_openat, AT_FDCWD, path, flags, 0666);
  if (fd < 0) return NULL;
  FILE* f = fdopen(fd, mode);
  if (f == NULL) {
    const int err = errno;
    syscall(SYS_close, fd);
    errno = err;
    return NULL;
  }
  if (borg_is_panel(path)) borg_add(fileno(f));
  return f;
}

FILE* fopen(const char* path, const char* mode) {
  return borg_fopen_impl(path, mode);
}

FILE* fopen64(const char* path, const char* mode) {
  return borg_fopen_impl(path, mode);
}

int fclose(FILE* f) {
  borg_forget(fileno(f));
  return _IO_fclose(f);
}

/* THE STDIO PATH HAS TO FILTER TOO, or the whole thing is decorative. For a
 * while it only *watched*: borg_feed was called here and borg_filter was not,
 * and on the board that was the difference between the shim seeing a chord and
 * the shim stopping one -- the unit opened its menu while microkorgd, reading
 * the same keys through a FILE*, opened SCOPE. Reading the keys is not the same
 * job as withholding them.
 *
 * The count returned is in elements, as fread's contract requires; a partial
 * element left by a withheld frame is dropped, which can only happen if the
 * caller asks for elements larger than one byte from a binary stream. */
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* f) {
  const size_t got = _IO_fread(ptr, size, nmemb, f);
  const size_t bytes = got * size;
  if (bytes > 0 && borg_find(fileno(f))) {
    /* The count is returned unchanged: a withheld press is neutralised in
     * place, so no element is ever lost and fread's short-count-means-EOF rule
     * cannot be tripped. */
    borg_consume((unsigned char*)ptr, bytes, 'f');
  }
  return got;
}

char* fgets(char* s, int size, FILE* f) {
  char* got = _IO_fgets(s, size, f);
  if (got != NULL && size > 0 && borg_find(fileno(f))) {
    /* strlen, not size: the panel's frames carry 0x00 padding, and the string
     * this call produced ends at the first of them. The keys are before it.
     * Nothing is truncated here either -- the neutralised byte keeps its place
     * and the terminator stays where fgets put it. */
    const size_t n = strlen(got);
    borg_consume((unsigned char*)got, n, 'g');
  }
  return got;
}
