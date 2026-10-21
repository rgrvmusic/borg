/* The exec module's dialogue with the far end, and nothing else.
 *
 * This is rgexec's wire, moved unchanged into a module. The instrument's MIDI
 * manager already holds every port it reads, and a unit is given no MIDI-out
 * API, so the module is handed complete SysEx messages by the resident object
 * and answers on the output paired with the input they arrived on.
 *
 *   in    F0 7D 3E @exec <command> F7
 *   out   F0 7D 4F <output chunk> F7                  'O'  output
 *   out   F0 7D 45 <three digits> F7                  'E'  status
 *   out   F0 7D 21 <text> F7                          '!'  refused
 *
 * 0x7D is the MIDI specification's non-commercial id. The 0x3E in a request,
 * and the rule that a body opening with a reply tag is dropped in silence,
 * keep an echo of our own output from being read back as a command.
 *
 * The header is free of system calls so that the framing can be tested without
 * the daemon. */

#ifndef BORG_EXEC_WIRE_H
#define BORG_EXEC_WIRE_H

#include <cstddef>
#include <cstdint>

namespace execwire {

constexpr uint8_t kSysExStart = 0xF0;
constexpr uint8_t kSysExEnd = 0xF7;
constexpr uint8_t kManufId = 0x7D;

constexpr uint8_t kTagCommand = 0x3E;  /* '>'  in  */
constexpr uint8_t kTagOutput = 0x4F;   /* 'O'  out */
constexpr uint8_t kTagExit = 0x45;     /* 'E'  out */
constexpr uint8_t kTagNotice = 0x21;   /* '!'  out */

/* The longest command accepted, including the "@exec " prefix. Longer is
 * refused rather than truncated: a truncated command is a different command. */
constexpr int kMaxCommand = 200;

/* Output bytes per reply message. */
constexpr int kMaxChunk = 200;

/* The most a single reply can occupy: three header bytes, every payload byte
 * escaped to four characters, and the terminator. */
constexpr int kMaxEncoded = 3 + 4 * kMaxChunk + 1;

inline bool Printable(uint8_t b) { return b >= 0x20 && b <= 0x7E; }
inline bool Clean(uint8_t b) { return Printable(b) && b != '\\'; }

inline int Escape(uint8_t b, char* out) {
  if (Clean(b)) {
    out[0] = static_cast<char>(b);
    return 1;
  }
  out[0] = '\\';
  if (b == '\\') {
    out[1] = '\\';
    return 2;
  }
  static const char kHex[] = "0123456789ABCDEF";
  out[1] = 'x';
  out[2] = kHex[(b >> 4) & 0x0F];
  out[3] = kHex[b & 0x0F];
  return 4;
}

inline int EncodeOutput(const uint8_t* data, int len, char* out) {
  if (len > kMaxChunk) len = kMaxChunk;
  int n = 0;
  out[n++] = static_cast<char>(kSysExStart);
  out[n++] = static_cast<char>(kManufId);
  out[n++] = static_cast<char>(kTagOutput);
  for (int i = 0; i < len; ++i) n += Escape(data[i], out + n);
  out[n++] = static_cast<char>(kSysExEnd);
  return n;
}

inline int EncodeNotice(const char* text, char* out) {
  int n = 0;
  out[n++] = static_cast<char>(kSysExStart);
  out[n++] = static_cast<char>(kManufId);
  out[n++] = static_cast<char>(kTagNotice);
  for (const char* p = text; *p != '\0'; ++p) {
    n += Escape(static_cast<uint8_t>(*p), out + n);
  }
  out[n++] = static_cast<char>(kSysExEnd);
  return n;
}

inline int EncodeExit(int status, char* out) {
  if (status < 0 || status > 255) status = 255;
  int n = 0;
  out[n++] = static_cast<char>(kSysExStart);
  out[n++] = static_cast<char>(kManufId);
  out[n++] = static_cast<char>(kTagExit);
  out[n++] = static_cast<char>('0' + (status / 100) % 10);
  out[n++] = static_cast<char>('0' + (status / 10) % 10);
  out[n++] = static_cast<char>('0' + status % 10);
  out[n++] = static_cast<char>(kSysExEnd);
  return n;
}

enum Refusal {
  kRefusalNone = 0,
  kRefusalOversize,
  kRefusalUnclean,
  kRefusalEmpty,
};

/* The inbound half: a byte-at-a-time SysEx reader that survives a stream it
 * does not own. Realtime bytes are ignored, anything that is not our header is
 * dropped in silence, and an F0 arriving mid-message starts a new one. Silence
 * matters more here than it did in the unit: this reader is offered every
 * message on every port the daemon holds, so another device's SysEx -- including
 * another maker's use of 0x7D -- must draw no reply at all. A message refused
 * for its contents is consumed to its terminator before the refusal is
 * reported, so the fragment before the bad byte is never run. */
class Reader {
 public:
  bool Feed(uint8_t b) {
    switch (state_) {
      case kIdle:
        if (b == kSysExStart) state_ = kHead;
        break;

      case kHead:
        if (b == kManufId) {
          len_ = 0;
          dirty_ = kRefusalNone;
          state_ = kTag;
        } else if (b == kSysExStart) {
          /* F0 F0 is a restart, not a header. */
        } else {
          /* Somebody else's message. Swallow it whole and say nothing. */
          state_ = kSwallow;
        }
        break;

      case kTag:
        if (b == kTagCommand) {
          state_ = kBody;
        } else {
          /* 0x7D with any other tag, our own output included: not a command,
           * and not ours to answer. */
          state_ = kSwallow;
        }
        break;

      case kBody:
        if (b == kSysExEnd) {
          state_ = kIdle;
          ended_ = true;
          if (dirty_ != kRefusalNone) return false;
          if (len_ == 0) {
            dirty_ = kRefusalEmpty;
            return false;
          }
          cmd_[len_] = '\0';
          return true;
        }
        if (b == kSysExStart) {
          state_ = kHead;
          len_ = 0;
          break;
        }
        if (!Printable(b)) {
          if (dirty_ == kRefusalNone) dirty_ = kRefusalUnclean;
          break;
        }
        if (len_ >= kMaxCommand) {
          if (dirty_ == kRefusalNone) dirty_ = kRefusalOversize;
          break;
        }
        cmd_[len_++] = static_cast<char>(b);
        break;

      case kSwallow:
        if (b == kSysExStart) {
          state_ = kHead;
        } else if (b == kSysExEnd) {
          state_ = kIdle;
          ended_ = true;
        }
        break;
    }
    return false;
  }

  const char* Command() const { return cmd_; }

  Refusal TakeRefusal() {
    if (!ended_) return kRefusalNone;
    ended_ = false;
    const Refusal r = dirty_;
    dirty_ = kRefusalNone;
    return r;
  }

 private:
  enum { kIdle, kHead, kTag, kBody, kSwallow } state_ = kIdle;
  char cmd_[kMaxCommand + 1] = {0};
  int len_ = 0;
  Refusal dirty_ = kRefusalNone;
  bool ended_ = false;
};

inline const char* RefusalText(Refusal r) {
  switch (r) {
    case kRefusalOversize: return "!long";
    case kRefusalUnclean: return "!byte";
    case kRefusalEmpty: return "!empty";
    case kRefusalNone:
    default: return nullptr;
  }
}

}  // namespace execwire

#endif  // BORG_EXEC_WIRE_H
