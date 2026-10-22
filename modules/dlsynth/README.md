# dlsynth — a second synth inside borg

`dlsynth.borgmodule` adds a drumlogue synth **unit** to the daemon borg already
runs, on a MIDI channel chosen on its own panel page. The unit is an ordinary
`.drmlgunit`: the module uses the same 26-byte runtime descriptor and the same
`unit_*` callbacks the daemon uses. It is not another host and does not know
what a `.drmlgunit` is beyond that ABI.

This module lives here rather than in the borg repository because it is
proprietary. It is built against `borg/libborg/borg_module.h`, so it needs a
borg that carries ABI 4 (see below).

## What borg must provide

The module asks for `BORG_CAP_AUDIO`, `BORG_CAP_MIDI_EVENTS`, `BORG_CAP_PAGE`
and `BORG_CAP_INPUT`. Those are the ABI 4 additions:

- `audio_render(out, frames)` is called on the audio thread with a zeroed
  interleaved stereo float block at `host->samplerate`. The module adds its
  unit's output; borg mixes it into the daemon's own PCM stream. On the
  drumlogue that stream is the codec's own shape — **S24_LE, eight channels,
  48 kHz, mmap** — and borg adds the stereo pair to every stereo pair in the
  block, because the daemon's own mix is duplicated across those outputs and
  which pair reaches the jack is not visible from inside the daemon.
- `midi_msg(port, data, n)` is offered every complete channel message, with
  running status assembled and any transform module's change already applied.
- `BORG_PATH_UNITS` resolves to the daemon's `Units/Synths` directory.

A borg built before ABI 4 refuses `dlsynth.borgmodule` at load: the module's
entry point returns `NULL` when the host ABI is below 4. Rebuild borg with
`build-drum.sh borg_drumlogue` (or the equivalent) before installing this.

## Build

```sh
./build-module.sh modules/dlsynth/dlsynth.c
```

That is the same script and the same logue SDK image that build every other
`.borgmodule`, so the module is built with the toolchain the instrument is
known to load. It writes `modules/dlsynth/dlsynth.borgmodule` and copies it to
`bin/dlsynth.borgmodule`; either can be copied to the instrument.

The unit ABI declarations the module needs are beside it, in
[`drumlogue_abi.h`](drumlogue_abi.h), because `build-module.sh` adds only
`libborg/` to the include path and the module speaks to the units directly.
The file's static asserts fail the build if the layout is wrong.

The result is armhf hard-float, needs only `libc.so.6`, and its ELF header
records the synth target it loads.

## Install

Copy `bin/dlsynth.borgmodule` directly to `userfs/Programs/` on the
instrument, beside the other `.borgmodule` files. No subdirectory is scanned.
Open the borg menu after copying: the module directory is checked again when
the menu opens, so a module added while the daemon runs appears after MUTE and
MASTER are pressed again. A load failure is written to
`userfs/Programs/borg-modules.log`.

The page is the last tab. PAGE DOWN reaches it, encoder 1 chooses a unit, and
MASTER loads it.

## The page

| Row | Meaning |
|---|---|
| `UNIT:` | the selected unit's display name from its header |
| `CHANNEL:` | the MIDI channel it listens on, 1 to 16 |
| `STATE:` | `READY`, `LOADING`, a failure reason, `NO SYNTH`, `NO DIR`, `BUSY`, or `NO UNIT`, followed by the last rendered block's peak as a percentage |
| `NOTE/REND:` | note-ons accepted, then render calls made by the host since the unit was loaded |

`NOTE/REND:` and the peak exist to answer one question from the panel: whether
the host is asking this module for audio at all. A `REND` that stays at zero
while `NOTES` rises means the daemon's playback was not mixed — the module
never saw a block. A `REND` that rises with a non-zero peak means the unit is
producing sound and the mix is the place to look.

- **Encoder 1** scrolls the synth list.
- **Encoder 2** changes the channel. It wraps, and sends All Notes Off to the
  unit before the new channel applies.
- **MASTER** loads the selected unit.

The list holds only `.drmlgunit` files whose ELF header names the drumlogue
**synth** target (`0x405`). An effect, a delay or a master effect is never
listed, and `open_unit` refuses one independently if it is reached anyway:
those ABIs have no note callbacks, so an effect would be silent and could only
take CPU. The filter reads the target from the file's own `unit_header`, not
from its name.

## MIDI

Only messages whose channel matches `CHANNEL:` are used:

| Message | Action |
|---|---|
| Note On (`0x9n`), velocity 1–127 | `unit_note_on` |
| Note On with velocity 0, Note Off (`0x8n`) | `unit_note_off` |
| Pitch Bend (`0xEn`) | `unit_pitch_bend`, 14-bit |
| Channel Pressure (`0xDn`) | `unit_channel_pressure` |
| Polyphonic Aftertouch (`0xAn`) | `unit_aftertouch` |
| CC 120, CC 123 | `unit_all_note_off` |

Other controllers are deliberately not mapped. A synth unit has parameters, not
a fixed CC map; guessing one would make an unrelated knob change a parameter the
user did not ask for. Use the unit's own controls for those.

SysEx is not used. `midi_in` was not requested, so the module is not offered
the traffic of the other ports.

## Environment

Read at module init:

| Variable | Meaning |
|---|---|
| `LOGUE_BORG_DLSYNTH_DIR` | unit directory; default `BORG_PATH_UNITS` (`/var/lib/drumlogued/userfs/Units/Synths`) |
| `LOGUE_BORG_DLSYNTH_CHANNEL` | initial channel, 1 to 16; default 1 |
| `LOGUE_BORG_DLSYNTH_UNIT` | file name, file stem or header display name to load at start; otherwise the first sorted entry |

These are useful for a test tree and for choosing a unit without the panel.
**Nothing is started on install alone.** A name in `LOGUE_BORG_DLSYNTH_UNIT` is
an explicit choice and is loaded during init; otherwise the highlighted unit is
loaded when MASTER is pressed. That distinction matters because the directory
can hold sample-based units, and this module supplies no sample banks (see
below): a crash caused by opening a page the user had not asked for would be
indistinguishable from a broken module. The panel does not persist its choices:
a restart returns to the environment, and no unit is loaded unless the variable
names one.

## Why the unit is copied to /tmp

The daemon may already have the same `.drmlgunit` mapped for one of its own
parts. `dlopen` returns the same handle for the same file, and calling
`unit_init` on it again would reset the part the user is playing. The module
copies the file to `BORG_PATH_TMP` under a unique name, opens the copy, and
unlinks it immediately; the mapping survives the unlink. That gives a distinct
instance with no state shared with the daemon's own unit.

If the selected unit is also the daemon's part, both respond to MIDI on their
own channels. Set `CHANNEL:` to something other than the daemon's channel if
that is not wanted.

## Threading and load safety

The panel thread loads and closes units. Before closing, it clears an audio
gate and waits up to 200 ms for the audio thread to leave the unit; a timeout
refuses the load with `BUSY` instead of tearing a unit down under a render.
MIDI arrives on the MIDI thread, so it only writes small events to a lock-free
ring; the audio thread applies them. Unit callbacks therefore run on one
thread at a time, except init and teardown, which the gate covers.

## What is not there

- **No sample banks.** The runtime descriptor reports zero banks, like the
  reference offline host. Oscillator synths work (`Tesseract_synth` renders at
  the same peak through dlsynth and `dlhost`); a unit that dereferences a
  sample can instead be silent or fault. `loguePAD2`/`loguePAD16` are such
  units — `dlhost` crashes on them under qemu as well. Until banks are
  implemented, select oscillator/wave units, not sample players. Supplying
  banks means reading the unit's own `wav/` layout (or the daemon's bank
  directory) and filling `get_num_sample_banks`, `get_num_samples_for_bank`
  and `get_sample`; that is not implemented.
- **One unit at a time**, a single channel, and no parameter page: the unit's
  declared defaults are pushed after `unit_init` and are not exposed on the
  panel. The unit's own front panel is not available either.
- **No persistence.** The page selection and channel are in memory only, and
  after a restart no unit is loaded unless `LOGUE_BORG_DLSYNTH_UNIT` names one.
- **Not yet verified through the instrument's codec.** The functional test loads
  a real `Tesseract_synth.drmlgunit` under qemu-arm and checks the synth filter
  against a mixed directory and an effects-only one, plus channel filtering and
  the panel controls. On armhf, driven without the daemon, a note on channel 1
  makes the factory `01_Nano` render at 0.5096 peak and `acidbass` at 0.0079 —
  the same magnitudes `dlhost` reports — while `Vault-synth` renders silence,
  which is what a sample-based unit does without banks. The host's half was
  measured in the daemon itself: the playback stream is recorded as
  `S24_LE 8ch 48000`, `audio_render` is entered on every 64-frame block, and a
  note reaches the unit's output. The verdict is still a manual run on the board
  with an oscillator unit; a sample player is expected to be silent here (see
  above).
