# exec.borgmodule

A command channel into `drumlogued`, over MIDI, as a `.borgmodule`.

The old `rgexec_drumlogue` unit had to open a MIDI interface in the
instrument's host port itself, because a unit is given no MIDI-out API and the
daemon already holds every port. That left the port occupied and needed a
second dongle. The borg extension runs inside the daemon, which is the process
that already reads and writes every port, so the same commands can arrive on the
gadget, the DIN sockets, or an interface in the host port, and the answer goes
out on the cable the request came in on.

Nothing here creates a process or a thread. That is the instrument's rule, not a
preference: `posix_spawn`, `fork`, the raw `clone` syscall and the bare `svc`
instruction are all refused. `execve` is permitted, and the kernel's own usermode
helper is the one process creator this process does not have to ask for. The
module uses both facts.


## Build and install

```sh
./build-module.sh modules/exec/exec.cc
```

Copy `modules/exec/exec.borgmodule`, or the copy in `bin/`, to
`userfs/Programs/` on the instrument. No subdirectory is scanned. Switch the
daemon with the borg unit first; the module
is loaded on the next panel action, and a module copied while the daemon runs
appears when the menu is closed and reopened.

The module adds an `EXEC` tab after the built-in pages and after any other
module pages, and it is offered every complete SysEx message from every port the
daemon reads.


## Commands

Every request begins `@exec`, which is the module's namespace on a wire shared
with whatever else is playing. A built-in word is answered in place; anything
else is run by the kernel.

| Request | Answer |
|---|---|
| `@exec` | the command list |
| `@exec info` | pid and thread counters, and a verdict on the refusal |
| `@exec probe` | whether `execve` is permitted, asked with four paths that must fail |
| `@exec cat <path> [offset]` | a file, up to 2048 bytes per request |
| `@exec ls [path]` | a directory, up to 64 entries |
| `@exec storage [word]` | `status`, `devs`, `block`, `mounts`, `userfs`, `gpio`, `probe`, `all` |
| `@exec <command>` | run it through the kernel's helper |

`info`, `probe`, `cat`, `ls` and `storage` read what this process can read and
answer without starting anything. `storage probe` asks whether `mount(2)` is
permitted with a tmpfs, a bind mount and a full cleanup, all inside `/tmp`.

Any other word is a command. The module writes a script in `/tmp`, points
`/proc/sys/kernel/hotplug` at it, and pokes a uevent. The kernel's helper thread
starts the script, which puts `/sbin/mdev` back before it runs the command. The
answer is read from a file by the module's tick, so the reply arrives in pieces
while the command runs. A command still running after sixty seconds is reported
as 124 and its output file is named in the notice; `@exec cat` reads the rest.

A second request while one is armed is refused with `!busy` and status 75
rather than queued. The first answer keeps the wire.


## The page

| Row | Shows |
|---|---|
| STATE | `READY`, or `RUNNING` while a kernel command is outstanding |
| COMMANDS | requests that arrived |
| LAST | the last request, as it arrived |
| PORT | the port it arrived on, as the daemon opened it |

`COMMANDS` not moving while the host tool sends means the request went to a port
the daemon is not reading, which is a wiring question rather than a command one.


## Host tool

```sh
tools/rgexec.py ping
tools/rgexec.py run info
tools/rgexec.py run storage all
tools/rgexec.py run 'uname -a'
tools/rgexec.py -p hw:3,0 run info
```

`run` adds `@exec` if it is missing, decodes the escaped reply, and prints the
exit status. The default port is the first rawmidi node of the drumlogue's
gadget card (USB vendor `0944`); `-p` takes a node path or `hw:card,device`. The
gadget presents two nodes, so if a request reaches nothing, name the other one.

`ping` sends the bare `@exec`, which is answered in place. It is the fastest way
to tell a closed channel from a refused command.


## Limits

- One complete SysEx per request, at most 200 printable bytes including the
  prefix. A longer command is refused rather than truncated.
- A reply is split into 200-byte messages. Output outside `0x20` to `0x7E` is
  escaped as `\xHH`, and a literal backslash as `\\`.
- Only messages addressed as `F0 7D 3E ... F7` are commands. Another
  manufacturer, another use of the non-commercial id, and an echo of the
  module's own output are all ignored in silence, because the module shares
  every input the daemon reads.
- The module is never unloaded, including after a refused or interrupted
  command. A power cycle removes it.


## Logs

| Path | Contents |
|---|---|
| `userfs/Programs/borg-modules.log` | load and skip lines, and the failure sentences from this module |
| `/tmp/rgx.<n>.sh` | the script the kernel was pointed at |
| `/tmp/rgx.<n>.out`, `/tmp/rgx.<n>.exit` | an armed command's output and status |

The module's own log lines are written only when something failed or changed
state, so a working command leaves no trace beyond its answer.


## Test

```sh
./modules/exec/test/run-native.sh
```

Builds the module for the development machine, links it against a host that
collects replies, and checks the framing, the silence about foreign traffic, the
in-place commands against a scratch `/sys`, `/proc/mounts`, `/dev` and userfs,
the page, and the arming of the kernel's helper. The hotplug path and the uevent
are scratch files, so no kernel helper is started and `/proc/sys` is not
touched.
