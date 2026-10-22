/**
 * @file native_test.cc
 * @brief storage.borgmodule's tests, run on a development machine.
 *
 * The module is built here for this machine against a host whose midi_write
 * collects bytes instead of writing a rawmidi node. The mount table is a file
 * in the scratch directory, so the module believes a stick is mounted without
 * a mount(2) being made, and the stick itself is a scratch directory. The code
 * that ships is what runs: the mountinfo parser, the session listing, the
 * sanitizer mirror that decides whether a session is safe to switch to, the
 * wire, and the page.
 *
 * What it cannot reproduce is the switch. borg_reexec is weak and undefined
 * here, which is exactly the state a module is in when the resident object
 * cannot re-execute the daemon; every path that would switch must refuse
 * cleanly and leave the tree alone. Nothing outside the scratch directory is
 * touched.
 *
 * Build and run with test/run-native.sh, which lays out the scratch tree and
 * passes the STORAGE_* paths as overrides. The scratch directory is argv[1].
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "borg_module.h"

namespace {

int g_failures;
int g_checks;
const char* g_scratch;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (ok) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s\n", what);
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool MakeDir(const std::string& path) {
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
  return false;
}

bool MakeDirs(const std::string& path) {
  std::string part;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!part.empty() && !MakeDir(part)) return false;
    }
    if (i < path.size()) part += path[i];
  }
  return true;
}

bool WriteFile(const std::string& path, const std::string& text) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  const ssize_t n = write(fd, text.data(), text.size());
  close(fd);
  return n == static_cast<ssize_t>(text.size());
}

/* ------------------------------------------------------------- the replies */

struct Message {
  unsigned char tag;
  std::string payload;
};

std::string Unescape(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '\\') {
      out += in[i++];
      continue;
    }
    if (i + 1 < in.size() && in[i + 1] == '\\') {
      out += '\\';
      i += 2;
      continue;
    }
    if (i + 3 < in.size() && in[i + 1] == 'x') {
      const int hi = in[i + 2];
      const int lo = in[i + 3];
      const int v = (hi >= 'A' ? hi - 'A' + 10 : hi - '0') * 16 +
                    (lo >= 'A' ? lo - 'A' + 10 : lo - '0');
      out += static_cast<char>(v);
      i += 4;
      continue;
    }
    out += in[i++];
  }
  return out;
}

std::vector<Message> Parse(const std::string& raw) {
  std::vector<Message> out;
  for (size_t i = 0; i < raw.size();) {
    if (static_cast<unsigned char>(raw[i]) != 0xF0) {
      ++i;
      continue;
    }
    size_t j = i + 1;
    if (j >= raw.size() || static_cast<unsigned char>(raw[j]) != 0x7D) {
      i = j;
      continue;
    }
    ++j;
    if (j >= raw.size()) break;
    Message m;
    m.tag = static_cast<unsigned char>(raw[j++]);
    const size_t start = j;
    while (j < raw.size() && static_cast<unsigned char>(raw[j]) != 0xF7) ++j;
    if (j >= raw.size()) break;
    m.payload = Unescape(raw.substr(start, j - start));
    out.push_back(m);
    i = j + 1;
  }
  return out;
}

std::string Output(const std::string& raw) {
  std::string text;
  const std::vector<Message> msgs = Parse(raw);
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].tag == 'O') text += msgs[i].payload;
  }
  return text;
}

std::string Notices(const std::string& raw) {
  std::string text;
  const std::vector<Message> msgs = Parse(raw);
  for (size_t i = 0; i < msgs.size(); ++i) {
    if (msgs[i].tag == '!') {
      if (!text.empty()) text += "\n";
      text += msgs[i].payload;
    }
  }
  return text;
}

int ExitStatus(const std::string& raw) {
  const std::vector<Message> msgs = Parse(raw);
  for (size_t i = msgs.size(); i > 0; --i) {
    if (msgs[i - 1].tag == 'E') return atoi(msgs[i - 1].payload.c_str());
  }
  return -1;
}

/* --------------------------------------------------------------- the host */

int g_port_tag;
std::string g_out;
std::vector<std::string> g_logs;
std::vector<std::pair<std::string, std::string> > g_rows;
std::string g_text;

void HostLog(const char* line) {
  if (line != NULL) g_logs.push_back(line);
}

const char* HostPath(int which) {
  return (which == BORG_PATH_PROGRAMS) ? g_scratch : "/tmp";
}

void HostText(unsigned, unsigned, unsigned, unsigned, const char* s) {
  if (s != NULL) g_text = s;
}

void HostDark(unsigned, unsigned, unsigned, unsigned, const char*) {}

void HostRow(unsigned, const char* label, const char* value) {
  g_rows.push_back(std::make_pair(label != NULL ? label : "",
                                  value != NULL ? value : ""));
}

int HostWrite(void* port, const void* data, size_t n) {
  if (port != &g_port_tag) return -1;
  g_out.append(static_cast<const char*>(data), n);
  return 0;
}

const char* HostName(void* port) {
  return port == &g_port_tag ? "hw:0,0,0" : "";
}

const borg_host g_host = {
  static_cast<uint32_t>(sizeof(borg_host)),
  BORG_MODULE_ABI,
  HostLog,
  HostPath,
  HostText,
  HostDark,
  HostRow,
  HostWrite,
  HostName,
  0  // samplerate (ABI 4)
};

std::string SysEx(const std::string& body) {
  std::string message;
  message += static_cast<char>(0xF0);
  message += static_cast<char>(0x7D);
  message += static_cast<char>(0x3E);
  message += body;
  message += static_cast<char>(0xF7);
  return message;
}

/* Sends the bytes as one message and returns everything the module wrote. */
std::string SendBytes(const borg_module* m, const std::string& raw) {
  g_out.clear();
  m->midi_in(&g_port_tag, reinterpret_cast<const unsigned char*>(raw.data()),
             raw.size());
  return g_out;
}

std::string Send(const borg_module* m, const std::string& body) {
  return SendBytes(m, SysEx(body));
}

void Draw(const borg_module* m) {
  g_rows.clear();
  g_text.clear();
  m->page_draw();
}

/* DrawPage writes STICK: first, then one row per visible session. */
std::string SessionLabel(int index) {
  const size_t i = static_cast<size_t>(index) + 1;
  return i < g_rows.size() ? g_rows[i].first : "";
}

std::string SessionValue(int index) {
  const size_t i = static_cast<size_t>(index) + 1;
  return i < g_rows.size() ? g_rows[i].second : "";
}

std::string RowValue(const char* label) {
  for (size_t i = 0; i < g_rows.size(); ++i) {
    if (g_rows[i].first == label) return g_rows[i].second;
  }
  return "";
}

/* --------------------------------------------------------------- the tree */

bool LayoutScratch() {
  const std::string s = g_scratch;
  const std::string sessions = s + "/base/stick/borg/sessions";
  const std::string dirs[] = {
      sessions + "/live/userfs/Programs",
      sessions + "/live/userfs/Units/Synths",
      sessions + "/live/userfs/Samples",
      sessions + "/live/userfs/Kits",
      sessions + "/nomod/userfs/Programs",
      sessions + "/dirty/userfs/Programs",
      sessions + "/two/userfs/Programs",
      sessions + "/two/userfs/Units",
      sessions + "/.hidden/userfs/Programs",
      s + "/userfs"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
    if (!MakeDirs(dirs[i])) return false;
  }

  const std::string module = "/userfs/Programs/storage.borgmodule";
  if (!WriteFile(sessions + "/live" + module, "x") ||
      !WriteFile(sessions + "/dirty" + module, "x") ||
      !WriteFile(sessions + "/two" + module, "x") ||
      !WriteFile(sessions + "/.hidden" + module, "x") ||
      !WriteFile(sessions + "/dirty/userfs/.hidden", "x") ||
      !WriteFile(sessions + "/plain-file", "x")) {
    return false;
  }

  /* A stick mount and, at first, no userfs mount. The parser reads the root
   * field of a line, so nothing here has to be a real mount. */
  return WriteFile(s + "/mountinfo",
                   "20 1 0:19 / / rw - tmpfs tmpfs rw\n"
                   "36 1 8:0 / " + s + "/base/stick rw,nosuid,nodev - vfat "
                   "/dev/sda rw\n");
}

/* The mount a switched daemon would see: the session's userfs bound over the
 * directory the daemon opens. */
bool MarkActive() {
  const std::string s = g_scratch;
  return WriteFile(s + "/mountinfo",
                   "20 1 0:19 / / rw - tmpfs tmpfs rw\n"
                   "36 1 8:0 / " + s + "/base/stick rw,nosuid,nodev - vfat "
                   "/dev/sda rw\n"
                   "99 36 8:0 /borg/sessions/live/userfs " + s +
                       "/userfs rw,nosuid,nodev - vfat /dev/sda rw\n");
}

/* ------------------------------------------------------------------ tests */

void TestWire(const borg_module* m) {
  std::string raw;

  raw = Send(m, "@storage status");
  Check(ExitStatus(raw) == 0, "status exits 0");
  Check(Contains(Output(raw), "stick /dev/sda"), "status names the stick");
  Check(Contains(Output(raw), "internal userfs"), "status starts internal");
  Check(Contains(Output(raw), "4 sessions"), "status counts four sessions");

  raw = Send(m, "@storage sessions");
  Check(ExitStatus(raw) == 0, "sessions exits 0");
  Check(Contains(Output(raw), "dirty") && Contains(Output(raw), "live") &&
            Contains(Output(raw), "nomod") && Contains(Output(raw), "two"),
        "sessions lists every valid session");
  Check(!Contains(Output(raw), ".hidden"), "a hidden session is not listed");
  Check(!Contains(Output(raw), "plain-file"),
        "a file in sessions is not listed");

  raw = Send(m, "@storage check live");
  Check(ExitStatus(raw) == 0, "check live exits 0");
  Check(Contains(Output(raw), "live is ready"), "check live says ready");

  raw = Send(m, "@storage check nomod");
  Check(ExitStatus(raw) == 2, "check without the module exits 2");
  Check(Contains(Notices(raw), "STORAGE MODULE MISSING"),
        "check without the module says why");

  raw = Send(m, "@storage check dirty");
  Check(ExitStatus(raw) == 2, "check with a hidden file exits 2");
  Check(Contains(Notices(raw), "HIDDEN FILES"),
        "check with a hidden file says why");

  raw = Send(m, "@storage check two");
  Check(ExitStatus(raw) == 2, "check without Samples exits 2");
  Check(Contains(Notices(raw), "NO SAMPLES"),
        "check without Samples names the folder");

  raw = Send(m, "@storage check missing");
  Check(ExitStatus(raw) == 2, "check of a missing session exits 2");
  Check(Contains(Notices(raw), "NO SUCH SESSION"),
        "check of a missing session says why");

  /* No resident switch in this build. The refusal must come before anything
   * is mounted, and the caller must be told why. */
  raw = Send(m, "@storage use live");
  Check(ExitStatus(raw) == 2, "use refuses without a switch");
  Check(Contains(Notices(raw), "the switch is not available"),
        "use says the switch is unavailable");

  raw = Send(m, "@storage back");
  Check(ExitStatus(raw) == 2, "back refuses without a switch");
  Check(Contains(Notices(raw), "the switch is not available"),
        "back says the switch is unavailable");

  raw = Send(m, "@storage nonsense");
  Check(ExitStatus(raw) == 2, "an unknown word exits 2");
  Check(Contains(Notices(raw), "no such word"), "an unknown word says so");

  raw = Send(m, "@storage help");
  Check(ExitStatus(raw) == 0, "help exits 0");
  Check(Contains(Output(raw), "use NAME"), "help lists the words");
}

void TestForeign(const borg_module* m) {
  /* Another manufacturer, another 0x7D dialect, and our own reply echoed back
   * all draw nothing. A module shares the wire with everything else. */
  g_out.clear();
  Check(SendBytes(m, std::string("\xF0\x01\x3E""nope\xF7", 8)).empty(),
        "another manufacturer draws no reply");

  g_out.clear();
  Check(SendBytes(m, std::string("\xF0\x7D\x4F""hello\xF7", 9)).empty(),
        "an echoed reply draws no reply");

  g_out.clear();
  Check(SendBytes(m, std::string("\xF0\x7D\x3E@exec info\xF7", 14)).empty(),
        "another module's namespace draws no reply");

  /* A refused message that was never ours must stay silent, even when the
   * command before it was ours. */
  Send(m, "@storage status");
  Check(SendBytes(m, std::string("\xF0\x7D\x3E\x01\xF7", 5)).empty(),
        "a malformed foreign message draws no reply");

  Check(SendBytes(m, std::string("\xF0\x7D\x3E\xF7", 4)).empty(),
        "an empty message draws no reply");

  /* Ours, and malformed: the refusal names the module. */
  const std::string raw = SendBytes(m, SysEx(std::string("@storage \x01", 10)));
  Check(Contains(Notices(raw), "!storage: byte"),
        "our malformed message is refused by name");
}

void TestPage(const borg_module* m) {
  m->page_enter();
  m->tick();
  Draw(m);

  Check(RowValue("STICK:") == "READY 4", "the page says the stick is ready");
  Check(SessionLabel(0) == "> dirty", "the cursor starts on the first session");
  Check(SessionLabel(1) == "  live", "the second row follows");
  Check(SessionValue(1) == "USE", "a session is offered by name");

  m->encoder(0, 1);
  Draw(m);
  Check(SessionLabel(1) == "> live", "the knob moves the cursor");

  /* The four knobs under the display have no press. BD is a part key and is
   * not one of this page's controls. */
  m->key(0, 1);
  m->key(0, 0);
  Draw(m);
  Check(SessionValue(0) == "USE" && SessionValue(1) == "USE",
        "a part key does not choose a row");

  m->key(0x0f, 1);
  Draw(m);
  Check(SessionLabel(1) == "> live", "MASTER keeps the cursor on its row");
  Check(SessionValue(1) == "PRESS AGAIN", "the first MASTER asks once more");

  m->key(0x0f, 0);
  Draw(m);
  Check(SessionValue(1) == "PRESS AGAIN", "a release does not confirm a row");

  m->key(0x30, 1);
  Draw(m);
  Check(SessionValue(1) == "USE", "NO drops the pending choice");

  /* Leaving and re-entering the page clears a pending press and rescans. */
  m->key(0x0f, 1);
  Draw(m);
  Check(SessionValue(1) == "PRESS AGAIN", "MASTER arms the row again");
  m->page_leave();
  m->page_enter();
  m->tick();
  Draw(m);
  Check(RowValue("STICK:") == "READY 4", "the page re-enters cleanly");
  Check(SessionLabel(1) == "> live", "the cursor survives a re-entry");
  Check(SessionValue(1) == "USE", "a re-entry forgets the pending press");
}

void TestPageSwitch(const borg_module* m) {
  /* Take the highlighted row with MASTER twice. Without a resident switch the
   * module refuses; the page says why and nothing is mounted. */
  m->page_enter();
  m->tick();
  m->key(0x0f, 1);
  Draw(m);
  Check(SessionValue(1) == "PRESS AGAIN", "the first MASTER is pending");
  m->key(0x0f, 1);
  Draw(m);
  Check(RowValue("STICK:") == "NO RELOAD",
        "a refused switch names the reason on the page");
}

void TestActive(const borg_module* m) {
  if (!MarkActive()) {
    Check(false, "the active mount can be written");
    return;
  }

  m->page_leave();
  std::string raw = Send(m, "@storage status");
  Check(ExitStatus(raw) == 0, "status exits 0 with a session active");
  Check(Contains(Output(raw), "session live is active"),
        "status recovers the active session from the mount table");

  raw = Send(m, "@storage sessions");
  Check(Contains(Output(raw), "live") && Contains(Output(raw), "active"),
        "sessions marks the active one");

  raw = Send(m, "@storage use live");
  Check(ExitStatus(raw) == 2,
        "use of the active session still needs a resident switch");

  raw = Send(m, "@storage back");
  Check(ExitStatus(raw) == 2, "back refuses without a switch");
}

}  // namespace

extern "C" const borg_module* borg_module_entry(uint32_t host_abi);

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <scratch>\n", argv[0]);
    return 2;
  }
  g_scratch = argv[1];
  if (!LayoutScratch()) {
    fprintf(stderr, "cannot lay out %s\n", g_scratch);
    return 2;
  }

  const borg_module* m = borg_module_entry(BORG_MODULE_ABI);
  Check(m != NULL, "entry offers the current ABI");
  Check(borg_module_entry(2) == NULL, "entry refuses an older host");
  if (m == NULL) {
    fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
    return 1;
  }
  Check(m->abi == BORG_MODULE_ABI, "the module declares the current ABI");
  Check((m->capabilities & BORG_CAP_INPUT) != 0, "the module asks for input");

  Check(m->init(&g_host, "/scratch/storage.borgmodule") == 0, "init succeeds");
  Check(m->page_draw != NULL && m->midi_in != NULL && m->key != NULL &&
            m->encoder != NULL && m->tick != NULL,
        "the callbacks are present");

  TestWire(m);
  TestForeign(m);
  TestPage(m);
  TestPageSwitch(m);
  TestActive(m);

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
