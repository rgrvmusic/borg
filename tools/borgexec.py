#!/usr/bin/env python3
"""Talk to the exec.borgmodule from the computer at the other end of the cable.

    tools/borgexec.py watch                # decode whatever the drumlogue sends
    tools/borgexec.py ping                 # is the channel open at all?
    tools/borgexec.py run info             # the counters behind the refusal
    tools/borgexec.py run storage all      # the instrument's storage
    tools/borgexec.py run 'uname -a'       # run one command, print its output
    tools/borgexec.py -p hw:3,0 run info

Every request goes out as `@exec <command>`, which is the module's namespace on
a port the borg extension shares with whatever else is playing. You do not have
to type the prefix; `run info` and `run '@exec info'` send the same bytes.

WHAT IS ANSWERED WITHOUT A PROCESS. Nothing in drumlogued may create a process,
so `info`, `probe`, `cat`, `ls` and `storage` are answered inside the daemon:

    info      the process and thread counters, and a verdict
    probe     whether execve is permitted, asked with four paths that must fail
    cat       a file, a page at a time
    ls        a directory
    storage   status | devs | block | mounts | userfs | gpio | probe | all
    help      the list, which is also what a bare `@exec` answers

ANY OTHER WORD IS RUN BY THE KERNEL. The module points /proc/sys/kernel/hotplug
at a script in /tmp and pokes a uevent; the kernel's own helper thread starts the
command, so the refusal never applies. Its output and status come back over this
same cable. `run 'uname -a'` is that route, and so is anything else that is not
one of the words above.

WHY THIS EXISTS RATHER THAN `amidi -d | xxd`. Two reasons, and the second is the
one that matters once something is wrong:

  * The dialect escapes every byte outside 0x20-0x7E, so output arrives as
    `\\x0A`-laden text that has to be decoded before it is readable.
  * A run answers with interleaved `O` (output), `!` (refusal) and `E` (exit)
    messages, and "did the command finish, and how" is a question about the `E`.

It talks to the rawmidi node directly, for the same reason the module does: the
node is the interface, and a stdio parser in between is what makes a nine-byte
reply wait for a 4 KB buffer to fill.

The wire format is restated here rather than shared with the module, because a
parser that shares its constants with the encoder agrees with it about a mistake.
"""

import argparse
import errno
import os
import re
import select
import sys
import time

SYSEX_START = 0xF0
SYSEX_END = 0xF7
MANUF_ID = 0x7D  # non-commercial / educational use

TAG_COMMAND = 0x3E  # '>'  in
TAG_OUTPUT = 0x4F  # 'O'  out
TAG_EXIT = 0x45  # 'E'  out
TAG_NOTICE = 0x21  # '!'  out

PREFIX = "@exec"

# A notice the module sends and what it means. Anything not listed is printed
# as it arrived: the text is the module's, and a table that pretended to be
# exhaustive would drop the one case somebody needs.
NOTICES = {
    b"!busy": "a command was already armed through the kernel; it was not queued",
    b"!byte": "a command carried a byte a command may not carry",
    b"!long": "a command was longer than the module accepts",
    b"!empty": "a command carried nothing",
    b"!moved": "the old bare form; every request now starts with @exec",
    b"!ns": "a request did not start with @exec",
    b"!exec": "the kernel's helper could not be armed, or had nothing to run",
    b"!cat": "the file could not be read",
    b"!ls": "the directory could not be read",
    b"!storage": "no such storage word",
}

# The drumlogue's own USB gadget. On the computer it is an ordinary Korg card,
# so its vendor is what identifies it; the instrument's other channels are not
# visible from this end.
KORG_VENDOR = "0944"

# The module gives an armed command up at sixty seconds and reports 124.
MODULE_DEADLINE = 60


def card_field(root, card, *parts):
    try:
        with open(os.path.join(root, "card%s" % card, *parts)) as f:
            return f.read().strip()
    except OSError:
        return ""


def find_node():
    """The rawmidi node to use, chosen by what the card is.

    The drumlogue's own gadget card wins when it is plugged in. With no gadget,
    a single USB MIDI interface is used, whatever its make: the instrument can
    be reached through any interface, wired or wireless. Virtual cards are used
    only when they are all that is present. A tie is refused with the choices
    rather than guessed."""
    root = "/sys/class/sound"
    snd = "/dev/snd"
    try:
        names = sorted(os.listdir(root))
    except OSError as exc:
        sys.exit("cannot list %s: %s" % (root, exc))
    try:
        entries = sorted(os.listdir(snd))
    except OSError as exc:
        sys.exit("cannot list %s: %s" % (snd, exc))

    found = []
    for name in names:
        m = re.fullmatch(r"card(\d+)", name)
        if not m:
            continue
        card = m.group(1)
        nodes = [e for e in entries
                 if re.fullmatch(r"midiC%sD\d+" % card, e)]
        if not nodes:
            continue
        device = re.fullmatch(r"midiC%sD(\d+)" % card, nodes[0]).group(1)

        vendor = card_field(root, card, "device", "..", "idVendor")
        product = card_field(root, card, "device", "..", "product")
        ident = card_field(root, card, "id")
        label = product or ident or "card %s" % card

        if vendor == KORG_VENDOR:
            rank = 0
        elif vendor:
            rank = 1
        else:
            rank = 2
        found.append((rank, card, device, label))

    if not found:
        sys.exit(
            "no rawmidi node: no sound card has a MIDI device. `amidi -l` should\n"
            "name one; pass -p hw:card,device if it does."
        )

    best = min(rank for rank, _, _, _ in found)
    picks = [f for f in found if f[0] == best]
    if len(picks) > 1:
        lines = ["more than one MIDI card to choose from:"]
        for rank, card, device, label in found:
            lines.append("  hw:%s,%s  %s" % (card, device, label))
        lines.append("pass -p hw:card,device")
        sys.exit("\n".join(lines))

    return os.path.join(snd, "midiC%sD%s" % (picks[0][1], picks[0][2]))


def node_for(spec):
    """Accept a node path, or ALSA's `hw:card,device` as `amidi -l` prints it."""
    if spec.startswith("/"):
        return spec
    m = re.fullmatch(r"hw:(\d+),(\d+)(?:,\d+)?", spec)
    if not m:
        sys.exit("--port wants a node path or hw:card,device, not %r" % spec)
    return "/dev/snd/midiC%sD%s" % (m.group(1), m.group(2))


def unescape(payload):
    """The reply side escapes every byte outside 0x20-0x7E: `\\xHH` for a byte and
    `\\\\` for a backslash. Commands are not escaped."""
    out = bytearray()
    i = 0
    while i < len(payload):
        b = payload[i]
        if b != 0x5C:  # backslash
            out.append(b)
            i += 1
            continue
        if payload[i + 1:i + 2] == b"\\":
            out.append(0x5C)
            i += 2
            continue
        if payload[i + 1:i + 2] == b"x" and len(payload) >= i + 4:
            try:
                out.append(int(payload[i + 2:i + 4], 16))
                i += 4
                continue
            except ValueError:
                pass
        out.append(b)
        i += 1
    return bytes(out)


class Parser:
    """Bytes in, `(tag, payload)` out, for this module's messages only.

    The port carries other traffic: Active Sensing, notes from whatever else is
    playing, and SysEx from other devices. Only 0x7D/0x3E is ours, and only the
    reply tags are read back; everything else is skipped. Real-time bytes may
    appear anywhere, including inside a message, and are ignored."""

    MAX_BODY = 2 + 4 * 200 + 3

    def __init__(self):
        self._in_message = False
        self._ours = False
        self._body = bytearray()

    def feed(self, data):
        """Returns `(messages, relevant_bytes, ours_pending)`.

        The byte count and the pending flag are what keep a chattering port from
        silencing the reader: the instrument streams Active Sensing about four
        times a second, and a wait that ends on any byte at all either never ends
        or ends too soon."""
        out = []
        relevant = 0
        for b in data:
            if b >= 0xF8:
                continue
            if b == SYSEX_START:
                self._in_message = True
                self._ours = False
                self._body = bytearray()
                continue
            if b == SYSEX_END:
                if self._in_message and self._ours and len(self._body) >= 2:
                    out.append((self._body[1], bytes(self._body[2:])))
                    relevant += 1
                self._in_message = False
                self._ours = False
                self._body = bytearray()
                continue
            if self._in_message:
                self._body.append(b)
                if len(self._body) == 1:
                    self._ours = b == MANUF_ID
                    if self._ours:
                        relevant += 2  # the F0 and this id
                elif self._ours:
                    relevant += 1
                if len(self._body) > self.MAX_BODY:
                    self._in_message = False
                    self._ours = False
                    self._body = bytearray()
        return out, relevant, self._in_message and self._ours


class Port:
    def __init__(self, node):
        try:
            self.fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
        except OSError as exc:
            sys.exit(
                "cannot open %s: %s\n"
                "`ls -l %s` -- the node is group audio, and this user has to be\n"
                "able to read and write it." % (node, exc.strerror, node)
            )
        self.node = node
        self.parser = Parser()

    def close(self):
        os.close(self.fd)

    def send(self, command):
        raw = command.encode() if isinstance(command, str) else command
        for b in raw:
            if not 0x20 <= b <= 0x7E:
                sys.exit("a command may only carry printable bytes; 0x%02X is not one" % b)
        message = bytes([SYSEX_START, MANUF_ID, TAG_COMMAND]) + raw + bytes([SYSEX_END])
        written = 0
        while written < len(message):
            try:
                written += os.write(self.fd, message[written:])
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    select.select([], [self.fd], [], 0.5)
                    continue
                sys.exit("write to %s: %s" % (self.node, exc.strerror))
        return len(raw)

    def pump(self, timeout):
        deadline = time.monotonic() + timeout
        relevant = 0
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return [], relevant
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                return [], relevant
            try:
                data = os.read(self.fd, 4096)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    time.sleep(0.01)
                    continue
                sys.exit("read from %s: %s" % (self.node, exc.strerror))
            if not data:
                time.sleep(0.01)
                continue
            messages, got, pending = self.parser.feed(data)
            relevant += got
            if got or pending:
                deadline = time.monotonic() + timeout
                return messages, relevant

    def messages(self, gap):
        while True:
            got, got_bytes = self.pump(gap)
            for message in got:
                yield message
            if got_bytes == 0:
                return

    def discard_stale(self, quiet=0.4):
        """A reply this end gave up on stays in the node's buffer, so the next
        run would read the previous answer as its own."""
        stale = 0
        while True:
            _, got_bytes = self.pump(quiet)
            if got_bytes == 0:
                break
            stale += got_bytes
        if stale:
            print("# flushed %d stale bytes from the buffer before sending: an "
                  "earlier run's\n# replies, not this command's" % stale,
                  file=sys.stderr, flush=True)


def report(tag, payload, quiet):
    if tag == TAG_OUTPUT:
        if not quiet:
            sys.stdout.buffer.write(unescape(payload))
            sys.stdout.buffer.flush()
        return None
    if tag == TAG_NOTICE:
        text = unescape(payload)
        note = NOTICES.get(text) or NOTICES.get(text.split(b" ")[0].rstrip(b":"))
        print("! %s%s" % (text.decode("ascii", "replace"),
                          "  (%s)" % note if note else ""),
              file=sys.stderr, flush=True)
        return None
    if tag == TAG_EXIT:
        try:
            return int(payload.decode("ascii", "replace").strip())
        except ValueError:
            return 255
    print("# unknown tag 0x%02X, %d bytes" % (tag, len(payload)),
          file=sys.stderr, flush=True)
    return None


def drain(port, gap, quiet):
    for tag, payload in port.messages(gap):
        status = report(tag, payload, quiet)
        if status is not None:
            return status
    return None


def watch(port, seconds):
    deadline = None if seconds is None else time.monotonic() + seconds
    while deadline is None or time.monotonic() < deadline:
        got, _ = port.pump(0.2)
        for tag, payload in got:
            names = {TAG_OUTPUT: "O", TAG_EXIT: "E", TAG_NOTICE: "!"}
            name = names.get(tag, "0x%02X" % tag)
            if tag == TAG_EXIT:
                print("%s  exit %s" % (name, payload.decode("ascii", "replace")),
                      flush=True)
            else:
                text = unescape(payload).decode("ascii", "replace")
                print("%s  %s" % (name, text.replace("\n", "\n   ")), flush=True)
    return 0


def wire_command(command):
    """`info` and `@exec info` send the same bytes; only this decides."""
    if command == PREFIX or command.startswith(PREFIX + " "):
        return command
    return PREFIX + " " + command


def main(argv):
    ap = argparse.ArgumentParser(
        description="Talk to the exec.borgmodule over the drumlogue's MIDI.",
        epilog="The module is loaded by the borg extension and answers on every "
               "port drumlogued reads, so the gadget, the DIN sockets and an "
               "interface in the host port all work. STATE on the module's page "
               "reads RUNNING while the kernel is working on an armed command, "
               "and COMMANDS counts the requests that arrived.",
    )
    ap.add_argument("--port", "-p", default=None,
                    help="rawmidi node, or hw:card,device (default: the "
                         "drumlogue gadget, else the only USB MIDI interface)")
    ap.add_argument("--timeout", "-t", type=float, default=MODULE_DEADLINE + 5,
                    help="seconds of silence that end a reply (default: %d)"
                         % (MODULE_DEADLINE + 5))
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress the command's output, keep its status")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("run", help="send one command and print what it said").add_argument(
        "command")
    sub.add_parser("ping", help="send the bare @exec and see whether a status "
                                "comes back")
    sub.add_parser("watch", help="decode whatever the drumlogue sends").add_argument(
        "--for", dest="seconds", type=float, default=None,
        help="stop after this many seconds (default: until ^C)")

    args = ap.parse_args(argv)
    node = node_for(args.port) if args.port else find_node()
    port = Port(node)
    print("# %s" % node, file=sys.stderr, flush=True)

    try:
        if args.mode == "watch":
            return watch(port, args.seconds)

        ping = args.mode == "ping"
        command = PREFIX if ping else wire_command(args.command)
        # A ping has no excuse for taking a minute: a bare @exec is answered in
        # place, and if the channel is closed, waiting is not a diagnosis.
        gap = 2.0 if ping else args.timeout

        port.discard_stale()
        port.send(command)
        if not ping:
            print("# %d bytes: %s" % (len(command), command),
                  file=sys.stderr, flush=True)

        status = drain(port, gap, args.quiet)
        if status is None:
            print(
                "# nothing came back in %gs. The module's page on the drumlogue "
                "settles\n# it: COMMANDS counts the requests that arrived, so if "
                "it did not move, this\n# end wrote to a port the daemon is not "
                "reading, and -p names another (the\n# gadget has two). If it did "
                "move, the module read the request and the answer\n# went to the "
                "output paired with that port. The log is at\n"
                "# /var/lib/drumlogued/userfs/Programs/borg-modules.log and can be "
                "read over\n# this same cable: run 'cat "
                "/var/lib/drumlogued/userfs/Programs/borg-modules.log'." % gap,
                file=sys.stderr, flush=True,
            )
            return 2

        if status == 75:
            print(
                "# exit 75 (busy): a command is already armed through the kernel, "
                "so this\n# one was refused rather than interleaved into its "
                "answer. Its own exit status\n# arrives over this same cable; "
                "send this again when it has.",
                file=sys.stderr, flush=True,
            )
            return status

        if ping:
            print("ok   the channel is open: @exec answered %d" % status)
            return 0 if status == 0 else status

        print("# exit %d" % status, file=sys.stderr, flush=True)
        return status
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
