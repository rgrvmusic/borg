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

**A replaced `borg.drmlgunit` does nothing to a daemon that is already active.**
GateOn is ignored, and the object the daemon is running was staged in `/tmp`
at the switch and is not read again. Replacing the unit takes effect only after
a power cycle, then load and GateOn as usual. `.borgmodule` files are the other
way round: they are read when the menu opens, so a replaced module does apply
to a running daemon.


## Extensions

The loaded object adds a status menu, a MIDI transform for `.borgmodule` to
use, and a `.borgmodule` loader to the running daemon. None of them starts a
process or a thread; they run in the daemon's own process.

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
| STATUS | daemon pid, uptime, blocks mixed per audio write, the recorded stream shape and xruns (a `MIXED/CALLS:` with a letter names why nothing was mixed), and a `STALL:` row when the audio thread has stopped |
| MIDI | MIDI bytes in and out, messages and notes, the last note, and the name of the loaded transform module (or `NONE`) |

The menu draws over the daemon's screen, and the daemon's last screen is
restored when it closes. Both chord presses are held back, so opening the menu
cannot arm MUTE or change the daemon's page. A MUTE or MASTER press on its own is
still delivered, in order; every other key and the lamps behave as before.

### MIDI note transform

The note transform is a module, `transform.borgmodule`, not a setting of the
loaded object. There is no environment variable: the daemon is started by the
instrument's own init before anything could set one, and borg's re-exec carries
that same environment, so a variable for it was unreachable in practice. The
module has a page of its own with the two controls:

| Encoder | Sets |
|---|---|
| 1 | transpose, -24 to +24 semitones |
| 2 | note-on velocity, 5 to 200 percent |

Transpose moves note-on and note-off alike, so the right voice is released;
velocity 0 on `0x9n` stays a note-off and is not scaled up. The defaults are 0
and 100 percent, which pass every message through unchanged. The STATUS page's
MIDI tab (`XFORM:`) names the module when one is loaded.

The capability is `BORG_CAP_MIDI_TRANSFORM` (ABI 5): the message is still the
daemon's copy and the data bytes are writable, so the transform happens before
the daemon's own unit plays the note and before any `MIDI_EVENTS` module sees
it. See [modules/transform](../modules/transform/README.md).

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

ABI 4 appends audio and channel MIDI. A module that asks for `BORG_CAP_AUDIO`
is given a block on the daemon's playback path; one that asks for
`BORG_CAP_MIDI_EVENTS` is offered every complete channel message before the
daemon's own unit sees it:

| Field | Meaning |
|---|---|
| `audio_render(out, frames)` | add into a zeroed interleaved stereo float block |
| `midi_msg(port, data, n)` | one channel message, status byte through its data bytes |
| host `samplerate` | the playback stream's rate, or 0 until one has been seen |

`audio_render` is the one callback the host does not serialize, because it runs
on the audio thread and cannot wait. It must not block, allocate, take a lock,
call a host service, or touch anything the panel or MIDI thread may be changing
without an atomic. The host calls it in blocks of at most 64 frames, so a unit
that caps its own render at 64 is safe, and mixes the result into whatever PCM
format the daemon opened (S16, S24, S32 or float, mono or stereo). **The
drumlogue opens the pcm3168a codec at S24_LE with eight channels**, and the
daemon's own mix is duplicated across those outputs, so a module's stereo pair
is added to every stereo pair in the block: which pair reaches the jack is not
visible from inside the daemon.

The stream's shape is recorded from every call the daemon makes to describe it
(`snd_pcm_hw_params_set_format`, `..._set_channels`, `..._set_rate`,
`snd_pcm_set_params`, and the installation itself in `snd_pcm_hw_params`), so
it is known before the first block. Those calls carry the values as arguments
and need nothing read back. Reading the installed parameters back with
`snd_pcm_hw_params_current` is a second, retried fallback for a stream that was
configured before this object was preloaded. A read that fails is not
remembered — latching one failure is how the mix once went silent for the rest
of a run while the call counter kept rising. A stream in a format or shape this
code cannot convert is passed through untouched.

### Scheduling

The daemon starts its playback thread at the default policy — `SCHED_OTHER` at
nice 0 — so on the instrument it competes on equal terms with the panel thread,
MIDI, and the threaded IRQs of a PREEMPT_RT kernel. The drumlogue's stream has
four 64-frame periods (5.3 ms) of buffer, and an EPIPE there means the audio
thread did not run for longer than that, while this hook's whole cost is tens
of microseconds. Borg therefore measures, over the first 256 blocks, how much
CPU the audio thread uses per cycle:

- **under three quarters** — the thread is being descheduled, and it is moved
  to `SCHED_FIFO` at 50, the priority a PREEMPT_RT kernel gives its threaded
  IRQs, so no ordinary thread can take the core away for a timeslice;
- **over that** — the core is genuinely full, and the priority is left alone,
  because raising it would only take the panel's share.

The decision, the sample it was taken from, and the resulting policy are one
line in `/tmp/logue-borg-audio.log` (`scheduling:`). A playback loop that
busy-waits out its deadline spends the whole cycle either way, so the ratio
does not depend on what the instrument happens to be playing.

Both the recorded format and the number of blocks actually mixed are on the
STATUS page (`PCM/XRUNS:` and `MIXED/CALLS:`). When calls rise but nothing is
mixed, `MIXED/CALLS:` gains one letter naming the reason, and the same reason
is written once to `borg-modules.log`:

| Letter | Reason |
|---|---|
| `n` | no loaded module asks for audio |
| `u` | the stream's format is not resolved |
| `b` | another block holds the scratch buffer |
| `s` | the block is larger than the scratch buffer |
| `x` | neither write entry point was entered at all |

`PCM/XRUNS:` names the shape of the handle being written: `S24 8C/0` is
S24_LE, eight channels, no xruns. A format this code cannot convert is shown
by its numeric id (`F5 8C/0`) rather than as `NONE`, so the instrument's own
stream can be identified from the panel. `NONE 0C` means no write has reached
the hook yet, which with `x` on the row below is the signature of a playback
path this object is not on.

When no block has been written for two seconds, that row becomes `STALL:` and
names where the audio thread is:

| Value | Stage |
|---|---|
| `S0` | no write entered: the daemon's playback thread is gone or waiting |
| `S1` | inside this hook, copying or mixing a block |
| `S2 M0` | inside module 0's `audio_render` — on the instrument, a unit's render |
| `S3` | inside the library's own write, so the stream is not consuming |

This exists because a wedged daemon takes the MIDI thread with it, so the log
cannot be fetched: the panel is the only report left. The same line is written
once per stall to `/tmp/logue-borg-audio.log`, with the counters and the last
skip reason, for a run that is still alive when it is read.

A module with nothing to add leaves the buffer alone. `samplerate` is written
before the first audio block; `init` may still see 0, which should be treated as
48000 — the drumlogue's fixed rate.

`midi_msg` runs on the MIDI thread, serialized with the other callbacks. Its
`n` is 2 or 3; running status has already been assembled, realtime bytes have
been dropped, and any `BORG_CAP_MIDI_TRANSFORM` module has already had the
message, so a module hears the notes the daemon's own unit hears. System
messages are not offered — SysEx remains `midi_in`. `port` is the same opaque
handle `midi_in` receives.

`midi_transform` is the same message one step earlier, with writable data
bytes and before the daemon's unit. It is the only callback that can change
what the instrument plays. Transform modules run in load order and before every
`midi_msg`. ABI 5 adds it as `BORG_CAP_MIDI_TRANSFORM`:

| Field | Meaning |
|---|---|
| `midi_transform(port, data, n)` | one channel message; the data bytes are writable, the status byte is not |

`transform.borgmodule` is the working example: transpose and velocity scaling,
with both settings on its own page.

`BORG_PATH_UNITS` names the daemon's `Units/Synths` directory, so a module can
load a `.drmlgunit` of its own without hardcoding the userfs path.

The callbacks are serialized by the host, except `audio_render`, so a module
needs no lock of its own for the others. It still must not create a process or
a thread, and it must stay silent about messages that are not its own: it is
offered the traffic of every other device on every port.

The host reads `abi` first and never reads past the layout that ABI names.
Fields are only appended, never reordered or removed. `init` receives the host
services (log, the path selector, text, dark text, row, in ABI 2 the two MIDI
calls, and in ABI 4 `samplerate`) and the module's own path. A module may draw its page but not clear the
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
| `userfs/Programs/borg-crash.log` | the daemon's last moment, if it faulted |
| `/tmp/logue-borg-unit.log` | unit-side errors |
| `/tmp/logue-borg-midi.log` | transformed MIDI notes |
| `/tmp/logue-borg-audio.log` | audio path timing, the first xrun, and one line per audio stall naming the stage |

The daemon installs no `SIGSEGV` or `SIGBUS` handler, and nothing restarts it,
so one bad pointer anywhere — including inside a unit it hosts — takes the whole
instrument down without a line anywhere. When borg is active it installs a
handler that records the signal, the PC, LR, SP, the faulting address and which
loaded object the addresses fall in, then re-raises with the default action.
That report goes to userfs because a dead daemon forces a power cycle and
`/tmp` would be wiped with it; it is one line, written once, as the process
dies.

`/tmp/logue-borg-audio.log` is the opposite case. It is written on the panel
thread, never the audio thread, at most every ten seconds, and on the first four
xruns: the hook's time and the module's share of it, the gap between the
previous write's return and the next hook entry (the daemon's own work for the
block), the time spent inside the library's write, the stream's buffer and
period read back from the handle, and the same block again in the audio
thread's **CPU** time — the one number that separates an overworked core from
a thread that was not running. The hook's time alone does not decide an xrun:
the block has to be produced before the buffer runs dry, so it is the gap plus
the mix that has to fit, and the buffer is measured rather than assumed. It
lives in tmpfs
deliberately — a diagnostic that writes to flash every
few seconds wears out the instrument it is diagnosing. Read it while the daemon
is alive (`@exec cat /tmp/logue-borg-audio.log`); the crash report above is the
part that survives.


## Notes

- A failed switch leaves the daemon unchanged. The reason is in
  `borg-exec.log`.
- The switch closes every descriptor at 3 and above, so USB MIDI connections
  drop. Reconnect them afterwards if needed.
- Nothing is installed permanently. The staged object is in `/tmp`, which is
  gone at the next power cycle.

