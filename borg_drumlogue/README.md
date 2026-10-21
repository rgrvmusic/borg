# Borg


A drumlogue unit that switches `drumlogued` to run extensions in place.

Select the unit, fire `GateOn()`. The daemon re-executes itself under an
embedded preload object: same pid, same process, no rootfs writes. A power cycle
restores everything.


## Build

Requires podman and a logue-sdk checkout.

```sh
LOGUE_SDK_PATH=~/Projects/logue/logue-sdk ./build-drum.sh borg_drumlogue
```

The SDK container image defaults to `docker.io/xiashj/logue-sdk:latest` and can
be overridden:

```sh
CONTAINER_IMAGE=... ./build-drum.sh borg_drumlogue
```

Output: `bin/borg.drmlgunit`.


## Install and use

1. Copy `bin/borg.drmlgunit` to `userfs/Units/Synths/` on the instrument.
2. Load the unit on a part.
3. Save the program. The daemon reloads the saved program after the switch;
   without the unit in it, the confirmation screen will not return.
4. Fire `GateOn()`.


| STATE | Screen | Meaning |
|---|---|---|
| `!ready` | `BORG READY` / `PRESS A PAD` | waiting for `GateOn()` |
| `!switch` | `BORG SWITCHING` / `LOADING EXTENSIONS` | switch in progress |
| `!active` | `BORG ACTIVE` / `EXTENSIONS LOADED` | the object is loaded; gates are ignored |
| `!err` | `BORG FAILED` / `NOTHING CHANGED` | the switch was refused; the daemon is unchanged |

`!active` describes the daemon, not the press that showed the unit. Once the
daemon runs with the object, every showing of the unit reads ACTIVE until a
power cycle, including after a program reload. The marker is only believed when
it names the unit's own process, so a stale marker cannot hide the ready state.


## Extensions

The loaded object adds a status menu, a MIDI note transform and a
`.borgmodule` loader to the running daemon. None of them starts a process or a
thread; they run in the daemon's own process.

### Status menu

Hold MUTE and press MASTER (the sixteenth part key) to open the menu. The same
chord closes it.

The menu draws `-BORG-` in the title band, a tab bar in the label row, and both
paging arrows after the tabs. PAGE UP and PAGE DOWN move between the tabs; the
active one carries a light field with dark text. The rows are light text on the
dark page, with no light field behind the names. The main sequencer views are
kept by the panel itself, so the menu does not paint over them: it switches the
panel to page mode, draws there, and switches back when it closes. Both the
pages and the sequencer views reopen with their own content intact.

| Tab | Shows |
|---|---|
| STATUS | daemon pid, uptime, audio calls, audio xruns and menu opens |
| MIDI | MIDI bytes in and out, messages and notes, the last note, transpose and velocity |

The menu draws over the daemon's screen, and the daemon's last screen is
restored when it closes. Both chord presses are held back, so opening the menu
cannot arm MUTE or change the daemon's page. A MUTE or MASTER press on its own is
still delivered, in order; every other key and the lamps behave as before.

### MIDI note transform

Incoming notes can be transposed and note-on velocity scaled:

| Variable | Default | Meaning |
|---|---|---|
| `LOGUE_BORG_MIDI_TRANSPOSE` | `0` | semitones added to every note, clamped to 0 to 127 |
| `LOGUE_BORG_MIDI_VELOCITY` | `100` | note-on velocity as a percentage, 1 to 200 |

The variables are read when the object loads. Real-time messages, system
exclusive data and running status are passed through unchanged.

### Modules

A `.borgmodule` adds a menu page, and may answer MIDI that arrives on any port
the daemon reads. It is an ARM shared object kept directly in
`userfs/Programs/`. No subdirectory is scanned. Build it against
`libborg/borg_module.h`; C and C++ sources both work.

Modules load on the first panel action after the switch. The directory is
checked again each time the menu opens, so a module copied while the daemon
runs appears after the menu is closed and reopened.

A file that fails to load is written to `userfs/Programs/borg-modules.log` and
skipped. A loaded module is never unloaded.

ABI 1 offers one menu page:

| Field | Meaning |
|---|---|
| `name` | tab title, at most 7 characters |
| `version` | free-form version string |
| `capabilities` | `BORG_CAP_PAGE` for a menu page |
| `init` | called once, off the audio path |
| `page_draw` | called on the panel thread while the page is active |

ABI 2 appends MIDI. A module that asks for `BORG_CAP_MIDI` is offered every
complete SysEx message from every input the daemon reads:

| Field | Meaning |
|---|---|
| `midi_in(port, data, n)` | one message, `F0` through `F7` inclusive; `port` is opaque |
| `tick()` | a cheap pulse, from the panel tick and from a MIDI read |
| host `midi_write(port, data, n)` | write on the output paired with the input the message arrived on |
| host `port_name(port)` | the name the daemon opened that input with |

Realtime bytes inside a message are dropped by the host. A message longer than
the host's buffer is cut short and still ends in `F7`, so a reader refuses it for
its length instead of waiting for an end that is not coming. `midi_write` may
wait briefly for room in the output and returns 0 only when every byte went out.

ABI 3 appends panel input for the module's own page. A module that asks for
`BORG_CAP_INPUT` is told when its page becomes active, and is handed the buttons
and the four endless encoders while it is shown:

| Field | Meaning |
|---|---|
| `page_enter()` | this page became the active menu page |
| `page_leave()` | it stopped being the active page |
| `key(index, down)` | a panel button, with the daemon's own index |
| `encoder(index, delta)` | one of the four knobs under the display, in signed steps |

PAGE UP, PAGE DOWN and the MUTE/MASTER chord stay with the menu, so a page
cannot trap the user. While a page asks for input it takes every encoder; the
daemon's page underneath does not see them. The callbacks run on the panel
thread.

The callbacks are serialized by the host, so a module needs no lock of its own.
It still must not create a process or a thread, and it must stay silent about
messages that are not its own: it is offered the traffic of every other device
on every port.

The host reads `abi` first and never reads past the layout that ABI names.
Fields are only appended, never reordered or removed. `init` receives the host
services (log, module path, text, dark text, row, and in ABI 2 the two MIDI
calls) and the module's own path. A module may draw its page but not clear the
screen. `row` has a 12-character label column and a 12-character value column;
use `text` for longer lines. The panel stops drawing a text run at a comma, so
use a space instead.

Build the example:

```sh
./build-module.sh modules/hello/hello.c
```

That writes `modules/hello/hello.borgmodule` and copies it to
`bin/hello.borgmodule`. Copy either one directly to `userfs/Programs/` on the
instrument before selecting the unit. Its `HELLO` tab
sits after the built-in tabs; PAGE DOWN reaches it, and the page shows
`MODULE:` and `HELLO MODULE`.

The exec module is the ABI 2 example and the operational one:

```sh
./build-module.sh modules/exec/exec.cc
```

Copy `modules/exec/exec.borgmodule`, or the copy in `bin/`, to
`userfs/Programs/` the same way. It
answers `@exec <command>` on every port the daemon reads, uses the kernel's
usermode helper for commands, and is driven from a computer with
`tools/borgexec.py`. See [modules/exec/README.md](../modules/exec/README.md).

The storage module is the ABI 3 example and the one that uses the panel:

```sh
./build-module.sh modules/storage/storage.cc
```

It mounts a USB stick, lists the sessions under `borg/sessions/` on it, and
switches the daemon to the chosen session's `userfs` in place. Nothing is
written to the instrument, and a power cycle returns the daemon to the internal
userfs. See [modules/storage/README.md](../modules/storage/README.md).

`LOGUE_BORG_MODULE_DIR` overrides the module directory, which is useful for a
test tree.

### Unit pages

An ordinary synth unit can add a page of its own while borg is active. The unit
is loaded into the same process, so it can call the optional API in
`libborg/borg_unit.h`. The declarations are weak: when borg is not
active the symbols are zero, the unit does not register anything, and it runs
exactly as it did before.

Register from `unit_init` or `unit_resume`, and unregister from `unit_teardown`
or `unit_suspend`. The page is drawn by borg on the panel thread, only while
the page is selected. The panel reports at 2 Hz, and that is the refresh a unit
or module page gets, so the page follows playback while the menu is held open.

The audio callbacks must not draw. Publish a small snapshot with atomics from
`unit_render`, and read that snapshot in the draw callback:

```c++
#include <atomic>
#include <cstdio>
#include "borg_unit.h"

static std::atomic<int> s_order{0};
static std::atomic<int> s_row{0};
static std::atomic<bool> s_playing{false};

static void page_draw(const borg_unit_panel* panel, void*) {
  char value[16];

  std::snprintf(value, sizeof value, "%02d", s_order.load());
  panel->row(26, "ORDER:", value);
  std::snprintf(value, sizeof value, "%02X", s_row.load());
  panel->row(35, "ROW:", value);
  panel->row(44, "STATUS:", s_playing.load() ? "PLAY" : "STOP");
}

static const borg_unit_desc s_desc = {
  BORG_UNIT_ABI, "TRACK", page_draw, nullptr
};

// In unit_init, after the unit's own setup:
if (borg_unit_register != nullptr) borg_unit_register(&s_desc);

// In unit_teardown, before the unit frees its state:
if (borg_unit_unregister != nullptr) borg_unit_unregister(&s_desc);
```

The page uses the four rows below the tab bar. `row` gives a 12-character label
column and a 12-character value column; `text` and `dark` take a box and are
for longer lines, at most 25 characters across the screen. The panel stops
drawing a text run at a comma, so use a space instead. The tab title is cut to
7 characters.

Only one page is offered per registered unit, and up to four units can be
registered. Unit pages sit after STATUS and MIDI; module pages follow them.


## Logs

| Path | Contents |
|---|---|
| `userfs/Programs/borg-exec.log` | pre-switch state and any refusal |
| `userfs/Programs/borg-shim.log` | attach confirmation from the loaded object |
| `userfs/Programs/borg-modules.log` | module load and skip lines, and unit page registrations |
| `/tmp/logue-borg-unit.log` | unit-side errors |
| `/tmp/logue-borg-midi.log` | transformed MIDI notes |


## Notes

- A failed switch leaves the daemon unchanged. The reason is in
  `borg-exec.log`.
- The switch closes every descriptor at 3 and above, so USB MIDI connections
  drop. Reconnect them afterwards if needed.
- Nothing is installed permanently. The staged object is in `/tmp`, which is
  gone at the next power cycle.

