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

A `.borgmodule` adds a page to the menu. It is an ordinary shared object kept
in the instrument's userfs at `userfs/Programs/borg_modules/`, built against
`emulator/shim/borg_module.h`.

Modules are loaded once, on the first panel read or write after the switch. A
file whose entry point or ABI does not match is written to
`userfs/Programs/borg-modules.log` and skipped; the rest still load. A module
that loads is never unloaded while the daemon runs. A module copied into the
directory after that point is picked up on the next switch, not by reopening
the menu.

ABI 1 offers one menu page:

| Field | Meaning |
|---|---|
| `name` | tab title, at most 7 characters |
| `version` | free-form version string |
| `capabilities` | `BORG_CAP_PAGE` for a menu page |
| `init` | called once, off the audio path |
| `page_draw` | called on the panel thread while the page is active |

The host reads `abi` first and never reads past the layout that ABI names.
Fields are only appended, never reordered or removed. `init` receives the host
services (log, module path, text, dark text, row) and the module's own path. A
module may draw its page but not clear the screen, and it may not create a
process or a thread.

Build the example:

```sh
./build-module.sh modules/hello/hello.c
```

That writes `modules/hello/hello.borgmodule`. Copy it to
`userfs/Programs/borg_modules/` on the instrument before selecting the unit.
Its `HELLO` tab sits after the built-in tabs; PAGE DOWN reaches it, and the page
shows `MODULE:` and `Hello, module!`.

`LOGUE_BORG_MODULE_DIR` overrides the module directory, which is useful for a
test tree.


## Logs

| Path | Contents |
|---|---|
| `userfs/Programs/borg-exec.log` | pre-switch state and any refusal |
| `userfs/Programs/borg-shim.log` | attach confirmation from the loaded object |
| `userfs/Programs/borg-modules.log` | module load and skip lines |
| `/tmp/logue-borg-unit.log` | unit-side errors |
| `/tmp/logue-borg-midi.log` | transformed MIDI notes |


## Notes

- A failed switch leaves the daemon unchanged. The reason is in
  `borg-exec.log`.
- The switch closes every descriptor at 3 and above, so USB MIDI connections
  drop. Reconnect them afterwards if needed.
- Nothing is installed permanently. The staged object is in `/tmp`, which is
  gone at the next power cycle.

