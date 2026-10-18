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

Output: `bin/borg_drumlogue.drmlgunit`.


## Install and use

1. Copy `bin/borg_drumlogue.drmlgunit` to `userfs/Units/Synths/` on the
   instrument.
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


## Extensions

The loaded object adds a status menu and a MIDI note transform to the running
daemon. Neither starts a process or a thread; both run in the daemon's own
process.

### Status menu

Hold MUTE and press MASTER (the sixteenth part key) to open the menu. The same
chord closes it.

The menu draws a tab bar across the top, with an arrow at each edge. PAGE UP
and PAGE DOWN move between the tabs, and the active tab is shown inverted.

| Tab | Shows |
|---|---|
| STATUS | daemon pid, uptime, audio calls and rate, audio xruns, menu opens |
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


## Logs

| Path | Contents |
|---|---|
| `userfs/Programs/borg-exec.log` | pre-switch state and any refusal |
| `userfs/Programs/borg-shim.log` | attach confirmation from the loaded object |
| `/tmp/logue-borg-unit.log` | unit-side errors |
| `/tmp/logue-borg-midi.log` | transformed MIDI notes |


## Notes

- A failed switch leaves the daemon unchanged. The reason is in
  `borg-exec.log`.
- The switch closes every descriptor at 3 and above, so USB MIDI connections
  drop. Reconnect them afterwards if needed.
- Nothing is installed permanently. The staged object is in `/tmp`, which is
  gone at the next power cycle.

