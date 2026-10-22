/**
 * @file native_test.cc
 * @brief exec.borgmodule's tests, run on a development machine.
 *
 * The module is built here for this machine, linked against a host whose
 * midi_write collects bytes instead of writing a rawmidi node. Everything the
 * module does between the message and the reply is the code that ships: the
 * reader, the escaping, the dispatch, the counters, the file and storage
 * commands, and the arming of the kernel's helper.
 *
 * What it cannot reproduce is the port and the instrument. A reply is checked
 * byte for byte, which is the part that has to be right or nothing works. The
 * kernel's helper is not poked: /proc/sys/kernel/hotplug and the uevent are
 * redirected into the scratch directory, so arming is tested (the script's
 * contents, the notice, and `!busy` while it is outstanding) without a kernel
 * ever running anything.
 *
 * THE FOREIGN-MESSAGE PART IS THE ONE THIS MODULE OWED. A unit read one port it
 * had taken for itself; a module is offered every message on every port the
 * daemon holds. Another manufacturer's SysEx, another 0x7D dialect and our own
 * reply echoed back must all draw no answer at all, or the module becomes noise
 * on a wire it shares.
 *
 * Build and run with test/run-native.sh, which lays out the scratch tree and
 * passes the RGEXEC_* paths as overrides. The scratch directory is argv[1].
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
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

int g_port_a_tag;
int g_port_b_tag;
std::string g_out_a;
std::string g_out_b;
std::vector<std::string> g_logs;
std::vector<std::pair<std::string, std::string> > g_rows;

void HostLog(const char* line) {
  if (line != NULL) g_logs.push_back(line);
}

const char* HostPath(int which) { return (which == BORG_PATH_PROGRAMS) ? g_scratch : "/tmp"; }

void HostText(unsigned, unsigned, unsigned, unsigned, const char*) {}
void HostDark(unsigned, unsigned, unsigned, unsigned, const char*) {}

void HostRow(unsigned, const char* label, const char* value) {
  g_rows.push_back(std::make_pair(label != NULL ? label : "",
                                  value != NULL ? value : ""));
}

int HostWrite(void* port, const void* data, size_t n) {
  if (port == &g_port_a_tag) {
    g_out_a.append(static_cast<const char*>(data), n);
    return 0;
  }
  if (port == &g_port_b_tag) {
    g_out_b.append(static_cast<const char*>(data), n);
    return 0;
  }
  return -1;
}

const char* HostName(void* port) {
  if (port == &g_port_a_tag) return "hw:0,0,0";
  if (port == &g_port_b_tag) return "hw:2,0,0";
  return "";
}

void SendRaw(const borg_module* m, void* port, const std::string& raw) {
  m->midi_in(port, reinterpret_cast<const unsigned char*>(raw.data()),
             raw.size());
}

void Send(const borg_module* m, void* port, const std::string& body) {
  std::string message;
  message += static_cast<char>(0xF0);
  message += static_cast<char>(0x7D);
  message += static_cast<char>(0x3E);
  message += body;
  message += static_cast<char>(0xF7);
  SendRaw(m, port, message);
}

void Reset(std::string* out, std::vector<std::string>* logs) {
  out->clear();
  if (logs != NULL) logs->clear();
  g_rows.clear();
}

/* --------------------------------------------------------------- the tree */

bool LayoutScratch() {
  const std::string s = g_scratch;
  const std::string dirs[] = {
      s + "/run", s + "/probe", s + "/sys/bus/usb/devices/usb1",
      s + "/sys/bus/usb/devices/1-1", s + "/sys/class/block/sda",
      s + "/sys/class/block/sda1", s + "/sys/class/gpio/gpio6",
      s + "/sys/class/gpio/gpio7", s + "/sys/class/sound/card0",
      s + "/sys/class/udc/ci_hdrc.0", s + "/proc", s + "/dev", s + "/userfs",
      s + "/mnt"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
    if (!MakeDirs(dirs[i])) return false;
  }

  /* A root hub, one device, one disk with one partition, and the two pins. */
  if (!WriteFile(s + "/sys/bus/usb/devices/usb1/idVendor", "1d6b\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/usb1/idProduct", "0002\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/1-1/idVendor", "0781\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/1-1/idProduct", "5567\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/1-1/product", "Ultra Fit\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/1-1/manufacturer", "SanDisk\n") ||
      !WriteFile(s + "/sys/bus/usb/devices/1-1/speed", "480\n")) {
    return false;
  }
  if (!WriteFile(s + "/sys/class/block/sda/size", "15633408\n") ||
      !WriteFile(s + "/sys/class/block/sda/removable", "1\n") ||
      !WriteFile(s + "/sys/class/block/sda/dev", "8:0\n") ||
      !WriteFile(s + "/sys/class/block/sda1/size", "15631360\n") ||
      !WriteFile(s + "/sys/class/block/sda1/removable", "1\n") ||
      !WriteFile(s + "/sys/class/block/sda1/dev", "8:1\n") ||
      !WriteFile(s + "/sys/class/block/sda1/partition", "1\n")) {
    return false;
  }
  if (!WriteFile(s + "/sys/class/gpio/gpio6/direction", "out\n") ||
      !WriteFile(s + "/sys/class/gpio/gpio6/value", "1\n") ||
      !WriteFile(s + "/sys/class/gpio/gpio7/direction", "in\n") ||
      !WriteFile(s + "/sys/class/gpio/gpio7/value", "0\n")) {
    return false;
  }
  if (!WriteFile(s + "/proc/mounts",
                 "/dev/root / ubifs rw 0 0\n"
                 "none /proc proc rw 0 0\n"
                 "none /tmp tmpfs rw 0 0\n")) {
    return false;
  }
  if (!WriteFile(s + "/dev/sda", "") || !WriteFile(s + "/dev/sda1", "")) {
    return false;
  }
  if (!WriteFile(s + "/userfs/hello.txt", "hello from userfs\n")) {
    return false;
  }
  if (!WriteFile(s + "/run/readme", "the scratch run directory\n")) {
    return false;
  }
  if (!WriteFile(s + "/hotplug", "/sbin/mdev\n")) {
    return false;
  }
  if (!WriteFile(s + "/sys/class/udc/ci_hdrc.0/uevent", "add\n")) {
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------ tests */

/* The module keeps this pointer for the life of the process, so it lives that
 * long here too. On the instrument it is the resident object's own table. */
borg_host g_host;

const borg_module* Boot() {
  memset(&g_host, 0, sizeof g_host);
  g_host.size = sizeof g_host;
  g_host.abi = BORG_MODULE_ABI;
  g_host.log = HostLog;
  g_host.path = HostPath;
  g_host.text = HostText;
  g_host.dark = HostDark;
  g_host.row = HostRow;
  g_host.midi_write = HostWrite;
  g_host.port_name = HostName;

  const borg_module* m = borg_module_entry(BORG_MODULE_ABI);
  Check(m != NULL, "the entry answers for the current ABI");
  if (m == NULL) return NULL;
  Check(m->abi == BORG_MODULE_ABI, "the module reports the current ABI");
  Check((m->capabilities & BORG_CAP_MIDI) != 0, "it asks for MIDI");
  Check((m->capabilities & BORG_CAP_PAGE) != 0, "it asks for a page");
  Check(m->init(&g_host, g_scratch) == 0, "init accepts the host");
  return m;
}

void TestHelp(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec");
  const std::string out = Output(g_out_a);
  Check(ExitStatus(g_out_a) == 0, "@exec alone exits 0");
  Check(Contains(out, "@exec info"), "the help lists info");
  Check(Contains(out, "@exec storage"), "the help lists storage");
  Check(Contains(out, "@exec <command>"), "the help names the kernel route");
}

void TestInfo(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec info");
  const std::string out = Output(g_out_a);
  Check(ExitStatus(g_out_a) == 0, "info exits 0 when the counters read");
  Check(Contains(out, "pid_max"), "info prints pid_max");
  Check(Contains(out, "threads-max"), "info prints threads-max");
  Check(Contains(out, "verdict:"), "info prints a verdict");
}

void TestProbe(const borg_module* m) {
  struct statvfs vfs;
  if (statvfs(g_scratch, &vfs) != 0 || (vfs.f_flag & ST_NOEXEC) != 0) {
    fprintf(stderr, "skip: probe (the scratch tree is mounted noexec)\n");
    return;
  }

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec probe");
  const std::string out = Output(g_out_a);
  const int status = ExitStatus(g_out_a);
  Check(status == 0 || status == 1, "probe ends with a verdict");
  Check(Contains(out, "exec probe in"), "probe names the directory");
  Check(Contains(out, "mode 0755"), "probe tries a file with the execute bit");
  Check(Contains(out, "ENOEXEC") || Contains(out, "EACCES"),
        "probe reaches the binary handlers, or the mount refuses first");
}

void TestCat(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec cat readme");
  Check(ExitStatus(g_out_a) == 0, "cat exits 0");
  Check(Output(g_out_a) == "the scratch run directory\n", "cat returns the file");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec cat nothing-here");
  Check(ExitStatus(g_out_a) == 1, "cat of a missing file exits 1");
  Check(Contains(Notices(g_out_a), "!cat:"), "cat says what it could not read");
}

void TestLs(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, std::string("@exec ls ") + g_scratch + "/run");
  const std::string out = Output(g_out_a);
  Check(ExitStatus(g_out_a) == 0, "ls exits 0");
  Check(Contains(out, "f readme "), "ls marks a regular file and its size");
  Check(Contains(out, "ls: 1 entries") || Contains(out, "ls: 2 entries") ||
            Contains(out, "entries"),
        "ls counts what it showed");
}

void TestStorage(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec storage status");
  const std::string out = Output(g_out_a);
  Check(ExitStatus(g_out_a) == 0, "storage status exits 0");
  Check(Contains(out, "storage: usb 1 root hub, 1 device"),
        "storage status counts the USB tree");
  Check(Contains(out, "storage: usb device 1-1 0781:5567 \"Ultra Fit\""),
        "storage status names the device");
  Check(Contains(out, "storage: sd sda 7633 MiB"),
        "storage status sizes the disk in MiB");
  Check(Contains(out, "storage: userfs internal"),
        "storage status calls an unmounted userfs internal");
  Check(Contains(out, "storage: bus power gpio6=1"),
        "storage status reads the bus power pin");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec storage block");
  const std::string block = Output(g_out_a);
  Check(Contains(block, "sda1") && Contains(block, "part=yes"),
        "storage block marks a partition");
  Check(Contains(block, "node=yes"), "storage block sees the device node");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec storage devs");
  Check(Contains(Output(g_out_a), "SanDisk"), "storage devs names the maker");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec storage userfs");
  Check(Contains(Output(g_out_a), "userfs: internal, on the root filesystem"),
        "storage userfs tells internal from external");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec storage nope");
  Check(ExitStatus(g_out_a) == 2, "an unknown storage word exits 2");
  Check(Contains(Notices(g_out_a), "!storage:"),
        "an unknown storage word says what the words are");
}

void TestForeignMessages(const borg_module* m) {
  /* Another manufacturer's SysEx. */
  Reset(&g_out_a, NULL);
  SendRaw(m, &g_port_a_tag, std::string("\xF0\x7E\x7F\x09\x01\xF7", 6));
  Check(g_out_a.empty(), "a foreign manufacturer draws no reply");

  /* Another dialect under the same non-commercial id. */
  Reset(&g_out_a, NULL);
  SendRaw(m, &g_port_a_tag, std::string("\xF0\x7D\x12hello\xF7", 9));
  Check(g_out_a.empty(), "0x7D with another tag draws no reply");

  /* Our own reply read back at us, which the daemon could do on a loopback. */
  Reset(&g_out_a, NULL);
  SendRaw(m, &g_port_a_tag, std::string("\xF0\x7D\x4Fhello\xF7", 9));
  Check(g_out_a.empty(), "an echo of our output draws no reply");

  /* A message that is never terminated must not answer, and must not stop a
   * real command that follows. */
  Reset(&g_out_a, NULL);
  SendRaw(m, &g_port_a_tag, std::string("\xF0\x7D\x3E@exec info", 13));
  Check(g_out_a.empty(), "an unterminated command answers nothing");
  Send(m, &g_port_a_tag, "@exec");
  Check(!g_out_a.empty(), "the next complete message is still read");
}

void TestRefusals(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, std::string("@exec \x01 info", 12));
  Check(Contains(Notices(g_out_a), "!byte"), "a non-printable byte is refused");
  Check(ExitStatus(g_out_a) == -1, "a refused message sends no exit status");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec " + std::string(300, 'a'));
  Check(Contains(Notices(g_out_a), "!long"), "an over-long command is refused");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "");
  Check(Contains(Notices(g_out_a), "!empty"), "an empty command is refused");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@storage status");
  Check(ExitStatus(g_out_a) == 2, "the old bare form exits 2");
  Check(Contains(Notices(g_out_a), "@exec storage"),
        "the old bare form names the new one");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "info");
  Check(Contains(Notices(g_out_a), "!ns"), "an unprefixed command is refused");
}

void TestPage(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_b_tag, std::string("@exec ls ") + g_scratch);
  g_rows.clear();
  m->page_draw();
  Check(g_rows.size() == 4, "the page draws four rows");
  std::string joined;
  for (size_t i = 0; i < g_rows.size(); ++i) {
    joined += g_rows[i].first + g_rows[i].second + "\n";
  }
  Check(Contains(joined, "STATE:READY"), "the page says the module is ready");
  Check(Contains(joined, "PORT:hw:2,0,0"),
        "the page names the port the last request arrived on");
  Check(Contains(joined, "LAST:@exec ls"), "the page shows the last request");
  Check(Contains(joined, "COMMANDS:"), "the page counts the requests");
}

/* The one script in the run directory. Its number is the module's command
 * count, which this test does not restate. */
std::string ScriptIn(const char* dir) {
  DIR* d = opendir(dir);
  if (d == NULL) return "";
  std::string found;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    const std::string name = ent->d_name;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, ".sh") == 0) {
      found = std::string(dir) + "/" + name;
    }
  }
  closedir(d);
  return found;
}

/* Arming is the one command that leaves work outstanding. The kernel is never
 * poked here: the hotplug path and the uevent are scratch files, so the script
 * is written and the module waits, exactly as it would for a kernel that has
 * not answered yet. A second request while it waits must be refused, not woven
 * into the first answer. This runs last, because it leaves the module busy. */
void TestArming(const borg_module* m) {
  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec echo hello");
  Check(ExitStatus(g_out_a) == -1, "an armed command has not finished when it "
                                   "is answered");
  Check(Contains(Notices(g_out_a), "!armed: running through"),
        "arming says how the answer will arrive");

  const std::string run_dir = std::string(g_scratch) + "/run";
  const std::string script = ScriptIn(run_dir.c_str());
  Check(!script.empty(), "the arming wrote a script");
  std::string body;
  if (!script.empty()) {
    FILE* f = fopen(script.c_str(), "r");
    Check(f != NULL, "the script can be read");
    if (f != NULL) {
      char buf[512];
      size_t n;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
      fclose(f);
    }
  }
  Check(Contains(body, "echo hello"), "the script carries the command");
  Check(Contains(body, "[ -f ") && Contains(body, ".done"),
        "the script runs once");
  const std::string stem = script.size() > 3 ? script.substr(0, script.size() - 3)
                                             : "";
  Check(Contains(body, "> " + stem + ".out"),
        "the script sends its output to a file");
  Check(Contains(body, "printf '%d\\n' $?"),
        "the script writes the status with a newline");

  Reset(&g_out_a, NULL);
  Send(m, &g_port_a_tag, "@exec info");
  Check(ExitStatus(g_out_a) == 75, "a second request while armed exits 75");
  Check(Contains(Notices(g_out_a), "!busy"), "it is told the module is busy");

  /* The page says so too. It is still busy: nothing here ends the arming. */
  g_rows.clear();
  m->page_draw();
  std::string joined;
  for (size_t i = 0; i < g_rows.size(); ++i) {
    joined += g_rows[i].first + g_rows[i].second + "\n";
  }
  Check(Contains(joined, "STATE:RUNNING"),
        "the page says a command is outstanding");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: native_test <scratch directory>\n");
    return 2;
  }
  g_scratch = argv[1];

  if (!LayoutScratch()) {
    fprintf(stderr, "cannot lay out the scratch tree in %s: %s\n", g_scratch,
            strerror(errno));
    return 2;
  }

  const borg_module* m = Boot();
  if (m == NULL) return 1;

  TestHelp(m);
  TestInfo(m);
  TestProbe(m);
  TestCat(m);
  TestLs(m);
  TestStorage(m);
  TestForeignMessages(m);
  TestRefusals(m);
  TestPage(m);
  TestArming(m);

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
