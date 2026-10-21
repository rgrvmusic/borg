# storage

A borg module that keeps a userfs on a USB stick and switches the daemon to it
from the panel, without writing anything to the instrument.

The daemon opens `/var/lib/drumlogued/userfs` once, at start, and sanitizes what
it finds there. It cannot be pointed somewhere else while it runs. What it can
do is start again: the module mounts the stick, binds one session's `userfs`
onto the directory the daemon expects, and asks borg to re-execute the daemon in
place. Same process, same pid; only `main` runs again.

Nothing is written to the rootfs, to the internal userfs or to the stick. The
only state is the kernel's mount table, and a power cycle clears it.


## What is on the stick

```
borg/
  sessions/
    live/
      userfs/
        Programs/
          storage.borgmodule
          exec.borgmodule
        Units/
          Synths/
            borg.drmlgunit
        Samples/
        Kits/
```

`userfs/` is the whole subtree the daemon sees. It carries the layout the daemon
would have made itself, not the stick's own root. Session names may use letters,
digits and punctuation, but not spaces or a leading dot.

The four folders are required. `Samples`, `Units` and `Kits` may be empty. A
session the daemon's sanitizer would change is refused: hidden files, links,
special files, unexpected top-level entries or names with spaces, at any depth.
A refused session is left as it was.

`Programs/storage.borgmodule` is required: without it the page is gone after the
switch, and the only way back is the wire or a power cycle. The exec module and
the borg unit are strongly recommended for the same reason. The module does not
add them to the session for you.


## On the panel

Hold MUTE and press MASTER, then PAGE UP or PAGE DOWN until `STORAGE` is the
active tab.

| Control | Does |
|---|---|
| knob 1 (under the display) | scrolls the highlight through the sessions |
| MASTER | takes the highlighted session: the row shows `PRESS AGAIN`, and MASTER again switches to it |
| NO | drops a pending choice |

The four knobs under the display are endless encoders. They turn and have no
press, so the highlighted row is taken with MASTER, the last of the sixteen part
keys. Returning to the internal userfs is the same gesture on the row marked
`ACTIVE`.

The top row shows `STICK:`. When something failed its value is the reason, such
as `NO MODULE`, `NO STICK`, `MOUNT DENIED` or `NO RELOAD`; `READING` while the
stick is being tested. `ACTIVE` marks the session the daemon is running on. The
page retries the stick when it is opened and every five seconds after a failed
attempt, so a stick inserted later appears without leaving the page.

`NO MODULE` means the highlighted session has no `Programs/storage.borgmodule`.
Copy it, and the exec module and the borg unit, into that session's userfs
before switching. The module will not add them for you.


## From the computer

The same actions are on the exec module's wire, in their own namespace:

```sh
tools/borgexec.py run '@storage status'
tools/borgexec.py run '@storage sessions'
tools/borgexec.py run '@storage use live'
tools/borgexec.py run '@storage back'
tools/borgexec.py run '@storage check live'
tools/borgexec.py run '@storage mount'
tools/borgexec.py run '@storage umount'
```

`use` and `back` end in a re-execute, so the USB MIDI connection drops. That is
the same behaviour as the unit's own switch; reconnect the cable and the module
answers again. A bare `@storage` is `status`.


## Build and install

```sh
./build-module.sh modules/storage/storage.cc
```

Copy `bin/storage.borgmodule` to `userfs/Programs/` on the instrument and to
`Programs/` inside every session's `userfs/`. It needs a host at ABI 3, which
means the current `borg.drmlgunit`; an older unit refuses it with a line in
`borg-modules.log`.

Order matters the first time: copy the unit to `Units/Synths/`, select it, fire
`GateOn()`, power cycle, then open the menu. The module is loaded from the
userfs the daemon is running on.


## Limits

- FAT and ext4 are read from the medium and mounted. exFAT and NTFS are not in
  the kernel and are refused by name.
- One stick, one mount point. The host port supplies 5 V at 200 mA, so the
  stick must be one the controller can power directly; hubs are not supported.
- A vfat mount is not journalled. Pulling the stick while the daemon is writing
  to `Programs/` can lose the last write.
- The switch closes the daemon's descriptors, as every borg switch does. Audio
  stops for the moment it takes `main` to come back.


## Test

```sh
./modules/storage/test/run-native.sh
```

Builds the module for the development machine against a host that collects
replies, with a scratch stick, a scratch mount table and a scratch userfs. It
checks the listing, the sanitizer mirror (a session without the module, with a
hidden file, with an unexpected entry), the wire and its silence about foreign
traffic, and the page and its controls. `borg_reexec` is undefined there, so
every switch path takes its refusal branch: nothing is mounted and nothing is
written.

