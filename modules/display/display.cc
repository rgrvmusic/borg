/* display.borgmodule: put frames on the microKORG2 panel and read back what the
 * daemon sent, from the exec channel, without rebuilding the unit.
 *
 * WHY THIS EXISTS. Every panel layout tried so far was derived from a capture
 * and then judged by a person looking at the instrument, one build at a time.
 * That loop is hours long and each cycle wrote a unit into userfs. This module
 * runs inside microkorgd, owns the same /dev/ttymxc2 the daemon owns -- a tty
 * can be opened more than once; the daemon's own descriptor is unaffected --
 * and takes one frame per command over the same SysEx channel exec.borgmodule
 * already answers on. A frame is tried, and the next one is a command away.
 *
 * WHAT IT DOES NOT DO. It does not read the panel. The panel's uplink is the
 * daemon's read(2); a second reader would take bytes out of that stream and
 * change the instrument's behaviour. The key log the resident object keeps
 * (/tmp/logue-borg-keys.log) and its record of the daemon's downlink
 * (/tmp/logue-borg-panelw.log) are the read side of this bench.
 *
 * THE TEXT FRAME IS THE PANEL FIRMWARE'S, NOT A GUESS. Its host-message
 * dispatcher takes id 0, reads x from bytes 9-10 and y from bytes 11-12 (7-bit
 * halves), and the string from byte 19; bytes 4-8 and 13-18 are not read there
 * and are carried over verbatim from the one id-0 frame known to work, the
 * boot-time popup. Sending the string anywhere else is what made a message come
 * out six characters short: the panel was printing from byte 19 and reading
 * text that was not there.
 *
 * THE NAMESPACE. Every request begins `@display`. `raw <hex>` bypasses every
 * builder above, because a wrong frame is only worth arguing with when the bytes
 * are exactly what was meant. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "borg_module.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

using borgwire::kMaxChunk;
using borgwire::kMaxEncoded;
using borgwire::Reader;

#ifndef DISPLAY_PANEL_PATH
#define DISPLAY_PANEL_PATH "/dev/ttymxc2"
#endif
#ifndef DISPLAY_WRITE_LOG
#define DISPLAY_WRITE_LOG "/tmp/logue-borg-panelw.log"
#endif
#ifndef DISPLAY_KEY_LOG
#define DISPLAY_KEY_LOG "/tmp/logue-borg-keys.log"
#endif
#ifndef DISPLAY_CHORD_PATH
#define DISPLAY_CHORD_PATH "/tmp/logue-borg-chord"
#endif

/* The panel frame is 64 bytes whatever the string is; byte 1 is the whole
 * length, self-inclusive, and the string lives from byte 19 to the end. */
constexpr size_t kFrame = 64;
constexpr size_t kTextAt = 19;
constexpr size_t kTextMax = kFrame - kTextAt;
constexpr int kRawMax = 128;

struct Ctx {
  const borg_host* host;
  Reader reader;
  int panel;
  int write_failed;
};

Ctx g = {NULL, {}, -1, 0};

/* ------------------------------------------------------------------ output */

void Logf(const char* fmt, ...) {
  char msg[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  if (g.host != NULL && g.host->log != NULL) g.host->log(msg);
}

void Raw(void* port, const char* buf, int len) {
  if (g.write_failed || port == NULL || len <= 0) return;
  if (g.host->midi_write(port, buf, static_cast<size_t>(len)) != 0) {
    g.write_failed = 1;
    Logf("midi write failed");
  }
}

void Emit(void* port, const char* buf, size_t len) {
  char enc[kMaxEncoded];
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

/* ------------------------------------------------------------------- panel */

int Panel(void) {
  if (g.panel >= 0) return g.panel;
  g.panel = open(DISPLAY_PANEL_PATH, O_WRONLY | O_NOCTTY | O_NONBLOCK);
  if (g.panel < 0) Logf("%s: %s", DISPLAY_PANEL_PATH, strerror(errno));
  return g.panel;
}

/* One frame, one write: the panel restarts its parser on any byte with bit 7
 * set, so a frame split across two writes is two messages. */
int Send(const unsigned char* frame, size_t n) {
  const int fd = Panel();
  if (fd < 0) return -1;
  const ssize_t wrote = write(fd, frame, n);
  return wrote == (ssize_t)n ? 0 : -1;
}

/* Id 0, the layout the panel firmware actually decodes. x and y are 7-bit
 * halves; 0x01 at byte 3 is the font the boot popup uses, and the string is
 * NUL-free inside the frame -- the panel measures it. */
void BuildText(unsigned char* out, unsigned x, unsigned y, const char* text) {
  memset(out, 0, kFrame);
  out[0] = 0x85;
  out[1] = (unsigned char)kFrame;
  out[2] = 0x00;   /* id 0: text */
  out[3] = 0x01;
  out[4] = 0x0e;
  out[5] = 0x00;
  out[6] = 0x7b;
  out[7] = 0x7c;
  out[8] = 0x7f;
  out[9] = (unsigned char)(x & 0x7f);
  out[10] = (unsigned char)((x >> 7) & 0x7f);
  out[11] = (unsigned char)(y & 0x7f);
  out[12] = (unsigned char)((y >> 7) & 0x7f);
  out[13] = 0x20;
  out[14] = 0x01;
  out[15] = 0x0f;
  out[16] = 0x00;
  out[17] = 0x00;
  out[18] = 0x01;
  size_t n = strlen(text);
  if (n > kTextMax - 1) n = kTextMax - 1;
  memcpy(out + kTextAt, text, n);
}

/* ------------------------------------------------------------------- small */

unsigned Number(const char** p) {
  while (**p == ' ') ++*p;
  char* end = NULL;
  const unsigned long v = strtoul(*p, &end, 0);
  if (end == *p) return 0;
  *p = end;
  return (unsigned)v;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Reads a file whole, up to cap, and answers it on the channel. The files this
 * bench reads are the resident object's logs, small and on tmpfs. */
void AtFile(void* port, const char* path, size_t cap) {
  char buf[2048];
  if (cap > sizeof buf) cap = sizeof buf;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    char line[300];
    snprintf(line, sizeof line, "!%s: %s", path, strerror(errno));
    Notice(port, line);
    AtEnd(port, 2);
    return;
  }
  const ssize_t got = read(fd, buf, cap);
  close(fd);
  if (got <= 0) {
    Notice(port, "!empty");
    AtEnd(port, 1);
    return;
  }
  Emit(port, buf, static_cast<size_t>(got));
  AtEnd(port, 0);
}

void HexDump(void* port, const unsigned char* p, size_t n) {
  char line[3 * kFrame + 8];
  size_t t = 0;
  for (size_t i = 0; i < n && t + 4 < sizeof line; ++i) {
    t += (size_t)snprintf(line + t, sizeof line - t, "%02x ", p[i]);
  }
  line[t] = '\0';
  Say(port, "%s", line);
}

/* --------------------------------------------------------------- dispatch */

void Help(void* port) {
  Say(port, "@display text <x> <y> <text>   id 0, string at byte 19, 45 max\n");
  Say(port, "@display hex <x> <y> <text>    build it, answer the bytes, send nothing\n");
  Say(port, "@display raw <hex bytes>        write them to the panel as one frame\n");
  Say(port, "@display log [bytes]            the daemon's recorded downlink\n");
  Say(port, "@display keys                   the panel's key ids, as the shim saw them\n");
  Say(port, "@display chord                  the chord byte the unit reads\n");
  Say(port, "@display panel                  whether the panel port opens\n");
  AtEnd(port, 0);
}

void AtCommand(void* port, const char* rest) {
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
  if (strcmp(word, "text") == 0 || strcmp(word, "hex") == 0) {
    const char* p = after;
    const unsigned x = Number(&p);
    const unsigned y = Number(&p);
    const char* text = p;
    unsigned char frame[kFrame];
    BuildText(frame, x, y, text);
    if (word[0] == 'h') {
      HexDump(port, frame, kFrame);
      AtEnd(port, 0);
      return;
    }
    if (Send(frame, kFrame) != 0) {
      char line[300];
      snprintf(line, sizeof line, "!panel write: %s", strerror(errno));
      Notice(port, line);
      AtEnd(port, 2);
      return;
    }
    Say(port, "sent %u bytes at %u,%u\n", (unsigned)kFrame, x, y);
    AtEnd(port, 0);
    return;
  }
  if (strcmp(word, "raw") == 0) {
    unsigned char frame[kRawMax];
    size_t len = 0;
    const char* p = after;
    while (*p != '\0' && len < kRawMax) {
      while (*p == ' ') ++p;
      if (*p == '\0') break;
      const int hi = HexDigit(p[0]);
      const int lo = HexDigit(p[1]);
      if (hi < 0 || lo < 0) {
        Notice(port, "!raw: two hex digits per byte");
        AtEnd(port, 2);
        return;
      }
      frame[len++] = (unsigned char)((hi << 4) | lo);
      p += 2;
    }
    if (len == 0) {
      Notice(port, "!raw: no bytes");
      AtEnd(port, 2);
      return;
    }
    if (Send(frame, len) != 0) {
      char line[300];
      snprintf(line, sizeof line, "!panel write: %s", strerror(errno));
      Notice(port, line);
      AtEnd(port, 2);
      return;
    }
    Say(port, "sent %u bytes\n", (unsigned)len);
    AtEnd(port, 0);
    return;
  }
  if (strcmp(word, "log") == 0) {
    unsigned want = Number(&after);
    AtFile(port, DISPLAY_WRITE_LOG, want != 0 ? want : 2048);
    return;
  }
  if (strcmp(word, "keys") == 0) {
    AtFile(port, DISPLAY_KEY_LOG, 2048);
    return;
  }
  if (strcmp(word, "chord") == 0) {
    AtFile(port, DISPLAY_CHORD_PATH, 8);
    return;
  }
  if (strcmp(word, "panel") == 0) {
    const int fd = Panel();
    if (fd < 0) {
      char line[300];
      snprintf(line, sizeof line, "!%s: %s", DISPLAY_PANEL_PATH,
               strerror(errno));
      Notice(port, line);
      AtEnd(port, 2);
      return;
    }
    Say(port, "%s open, fd %d\n", DISPLAY_PANEL_PATH, fd);
    AtEnd(port, 0);
    return;
  }

  Notice(port, "!display: unknown word; try @display help");
  AtEnd(port, 2);
}

void Handle(void* port, const char* cmd) {
  g.write_failed = 0;
  if (strncmp(cmd, "@display", 8) != 0 ||
      (cmd[8] != '\0' && cmd[8] != ' ')) {
    /* Every module is offered every message; answering another module's
     * namespace would race the module that owns it. */
    if (cmd[0] == '@') return;
    Notice(port, "!ns: commands start with @display");
    AtEnd(port, 2);
    return;
  }
  AtCommand(port, cmd[8] == ' ' ? cmd + 9 : "");
}

/* -------------------------------------------------------------- callbacks */

void display_midi_in(void* port, const unsigned char* data, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (g.reader.Feed(data[i])) {
      Handle(port, g.reader.Command());
      continue;
    }
    const char* text = borgwire::RefusalText(g.reader.TakeRefusal());
    if (text != nullptr) Notice(port, text);
  }
}

int display_init(const borg_host* host, const char* module_path) {
  (void)module_path;
  g.host = host;
  if (host == NULL || host->midi_write == NULL) return 1;
  Logf("display module ready");
  return 0;
}

const borg_module display_module = {
  BORG_MODULE_ABI,
  "DISPLAY",
  "1.0.0",
  BORG_CAP_MIDI,
  display_init,
  nullptr,  // page_draw
  display_midi_in,
  nullptr,  // tick
  nullptr,  // page_enter
  nullptr,  // page_leave
  nullptr,  // key
  nullptr,  // encoder
  nullptr,  // audio_render (ABI 4)
  nullptr,  // midi_msg (ABI 4)
  nullptr   // midi_transform (ABI 5)
};

}  // namespace

extern "C" const borg_module* borg_module_entry(uint32_t host_abi) {
  if (host_abi < 2) return NULL;
  return &display_module;
}
