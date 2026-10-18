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

#ifndef LOGUE_BORG_MIDI_LOG
#define LOGUE_BORG_MIDI_LOG "/tmp/logue-borg-midi.log"
#endif

#define PANEL_FDS_MAX 8
#define CAPTURE_MAX 8192
#define PAGES 2

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

/* One class-0x85 id 0 frame: box header, then the text (at most 51 chars). */
static size_t screen_frame(unsigned char* out, unsigned char role,
                           unsigned char fg, unsigned char bg,
                           unsigned x, unsigned y, unsigned w, unsigned h,
                           const char* text) {
  size_t n = strlen(text);
  if (n > 51) n = 51;
  out[0] = 0x85;
  out[1] = (unsigned char)(13 + n);
  out[2] = 0x00;
  out[3] = role;
  out[4] = fg;
  out[5] = bg;
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
 * role 0x0f is the light band the daemon puts behind a field name. */
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
  out[11] = 0x00;                             /* kind */
  return 12;
}

/* Role 0 is the daemon's label and title style, the small font. Role 2 is the
 * large value style, which does not fit six rows on a 64-unit screen. */
static void put_styled(unsigned x, unsigned y, unsigned w, unsigned h,
                       unsigned char fg, unsigned char bg, const char* text) {
  unsigned char frame[64];
  const size_t n = screen_frame(frame, 0x20, fg, bg, x, y, w, h, text);
  if (panel_out_fd >= 0) raw_write(panel_out_fd, frame, n);
}

static void put_text(unsigned x, unsigned y, unsigned w, unsigned h,
                     const char* text) {
  put_styled(x, y, w, h, 0x0f, 0x00, text);
}

static void put_label(unsigned x, unsigned y, unsigned w, unsigned h,
                      const char* text) {
  put_styled(x, y, w, h, 0x00, 0x0f, text);
}

/* A field name on the left and its value right-aligned on the right. The
 * name sits on the light band the daemon uses for field names; the value is
 * drawn from its right edge, so a column of numbers lines up. */
static void menu_row(unsigned y, const char* label, const char* value) {
  unsigned char fill[12];
  char cell[16];

  raw_write(panel_out_fd, fill, fill_frame(fill, 0x0f, 0, y, 120, 8));
  snprintf(cell, sizeof cell, "%-12s", label);
  put_label(0, y, 120, 8, cell);
  snprintf(cell, sizeof cell, "%12s", value);
  put_text(128, y, 128, 8, cell);
}

static void menu_draw(void) {
  unsigned char fill[12];
  char value[24];

  if (panel_out_fd < 0) return;

  /* The page starts from black. A one-unit light rule under the tab bar is
   * the same fill the daemon puts behind a label, so it reads as a divider
   * rather than as another text row. */
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x00, 0, 0, 256, 64));
  raw_write(panel_out_fd, fill, fill_frame(fill, 0x0f, 0, 9, 256, 1));

  /* Two tabs, the active one as a light band with dark text. The arrows at
   * the edges sit where the two page keys act. */
  raw_write(panel_out_fd, fill,
            fill_frame(fill, 0x0f, menu_page == 0 ? 10 : 128, 0, 118, 8));
  put_styled(10, 0, 118, 8,
             menu_page == 0 ? 0x00 : 0x0f, menu_page == 0 ? 0x0f : 0x00,
             menu_page == 0 ? " [ STATUS ]" : "   STATUS  ");
  put_styled(128, 0, 118, 8,
             menu_page == 1 ? 0x00 : 0x0f, menu_page == 1 ? 0x0f : 0x00,
             menu_page == 1 ? " [ MIDI ]  " : "    MIDI   ");
  put_text(0, 0, 10, 8, "<");
  put_text(246, 0, 10, 8, ">");

  if (menu_page == 0) {
    const long up = (long)(time(NULL) - start_epoch);
    snprintf(value, sizeof value, "%d", (int)getpid());
    menu_row(11, "DAEMON PID:", value);
    if (up >= 3600) {
      snprintf(value, sizeof value, "%ldH %02ldM", up / 3600, (up / 60) % 60);
    } else if (up >= 60) {
      snprintf(value, sizeof value, "%ldM %02ldS", up / 60, up % 60);
    } else {
      snprintf(value, sizeof value, "%ld S", up);
    }
    menu_row(20, "UPTIME:", value);
    snprintf(value, sizeof value, "%lu", audio_calls);
    menu_row(29, "AUDIO CALLS:", value);
    snprintf(value, sizeof value, "%lu/S",
             up > 0 ? audio_calls / (unsigned long)up : 0UL);
    menu_row(38, "AUDIO RATE:", value);
    snprintf(value, sizeof value, "%lu", audio_errors);
    menu_row(47, "AUDIO XRUNS:", value);
    snprintf(value, sizeof value, "%lu", menu_opens);
    menu_row(56, "MENU OPENS:", value);
  } else {
    snprintf(value, sizeof value, "%lu B", midi_in_bytes);
    menu_row(11, "MIDI IN:", value);
    snprintf(value, sizeof value, "%lu B", midi_out_bytes);
    menu_row(20, "MIDI OUT:", value);
    snprintf(value, sizeof value, "%lu / %lu", midi_in_msgs, midi_notes);
    menu_row(29, "MSGS/NOTES:", value);
    snprintf(value, sizeof value, "%02lX %02lX %02lX", midi_last_status,
             midi_last_note, midi_last_vel);
    menu_row(38, "LAST NOTE:", value);
    snprintf(value, sizeof value, "%+d", midi_transpose);
    menu_row(47, "TRANSPOSE:", value);
    snprintf(value, sizeof value, "%d%%", midi_velocity);
    menu_row(56, "VELOCITY:", value);
  }
}

static void menu_show(int fd) {
  panel_out_fd = fd;
  menu_open = 1;
  menu_page = 0;
  menu_opens++;
  menu_draw();
}

static void menu_hide(int fd) {
  unsigned char fill[12];
  menu_open = 0;
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
        if (key == KEY_PAGE_DOWN) {
          menu_page = (menu_page + 1) % PAGES;
          menu_draw();
        } else if (key == KEY_PAGE_UP) {
          menu_page = (menu_page + PAGES - 1) % PAGES;
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
  unsigned char kept[512];
  size_t i = 0, out = 0;

  if (!panel_is(fd)) return (ssize_t)syscall(SYS_write, fd, buf, n);

  /* The daemon writes one small frame at a time. A larger buffer is passed
   * through untouched rather than parsed on a guess. */
  if (n > sizeof kept) return (ssize_t)syscall(SYS_write, fd, buf, n);

  while (i < n) {
    const size_t len = out_frame_len(p + i, n - i);
    if (len == 0) {
      memcpy(kept + out, p + i, n - i);
      out += n - i;
      break;
    }
    if (p[i] == 0x85) {
      capture_frame(p + i, len);
      if (!menu_open) {
        memcpy(kept + out, p + i, len);
        out += len;
      }
    } else if (out + len <= sizeof kept) {
      memcpy(kept + out, p + i, len);
      out += len;
    }
    i += len;
  }

  if (out > 0 && syscall(SYS_write, fd, kept, out) < 0) return -1;
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
