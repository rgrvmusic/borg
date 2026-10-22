# transform

A borg module that transposes incoming notes and scales note-on velocity,
before the daemon's own unit plays them. The two controls are on the module's
page: encoder 1 is the transpose, encoder 2 the velocity.

It exists because the transform was previously a setting of the resident
object, read from `LOGUE_BORG_MIDI_TRANSPOSE` and `LOGUE_BORG_MIDI_VELOCITY` at
load. Nothing on the instrument can set an environment variable for the
daemon — the daemon is started by the instrument's own init, and borg's re-exec
carries the same environment — so the feature could not be reached from the
panel at all. As a module it has a page, and the host no longer carries a
transform it cannot configure.

## What it needs

ABI 5, capability `BORG_CAP_MIDI_TRANSFORM`. The host hands the module each
complete channel message **on its way to the daemon's unit**, with writable
data bytes; the status byte is not to be changed, because it decides the
message's length. A transform module runs before every `MIDI_EVENTS` module, so
`dlsynth` and anything else listening hears the transformed note.

A module built for ABI 5 is refused by a borg built for ABI 4. Rebuild borg
with `./build-drum.sh borg_drumlogue` and copy the new unit before installing
this module.

## The page

| Row | Meaning |
|---|---|
| `TRANSPOSE:` | semitones, -24 to +24, `+0` by default |
| `VELOCITY:` | note-on velocity as a percentage, 5 to 200, `100%` by default |
| `TRANSFORMED:` | messages actually rewritten since the module loaded |
| `ENCODERS:` | which control changes which value |

- **Encoder 1** changes the transpose by one semitone per step.
- **Encoder 2** changes velocity by five percent per step, wrapping nowhere.

`TRANSFORMED:` stays at zero while both settings are at their defaults: nothing
was changed, and the count says exactly that. It is the quickest check that the
module is receiving messages at all.

## What is transformed, and what is not

Only note messages (`0x9n` and `0x8n`). Everything else — controllers, program
change, channel pressure, pitch bend, SysEx — is passed through untouched. A
synth unit's parameters are not a fixed CC map, so rewriting a controller
number would be a guess.

- **Note-off is transposed too** (`0x8n`, and `0x9n` with velocity 0), so the
  voice that was started is the voice that stops.
- **Velocity 0 is never scaled up.** On `0x9n` it means note-off; a curve that
  lifted it to velocity 1 would turn every release into a note-on.
- **The note is clamped to 0 to 127**, not wrapped.

Two transform modules can be installed at once. They run in load order, each
receiving what the one before left, so the result is the composition of both.

## Build and install

```sh
./build-module.sh modules/transform/transform.c
```

Copy `bin/transform.borgmodule` (or `modules/transform/transform.borgmodule`)
directly to `userfs/Programs/` on the instrument, beside the other modules. No
subdirectory is scanned; open the borg menu again after copying and the module
appears as the `XFORM` tab.

The STATUS page's MIDI tab shows `XFORM:` with the module's name when a
transform module is loaded, or `NONE`.

## Test

```sh
./modules/transform/test/run-native.sh
```

The module is compiled for the development machine and driven through the
`borg_module` table the daemon reads: the ABI gate, init, both encoders, the
page rows, and the writable message — including the note-off and velocity-0
cases that a curve gets wrong. It cannot prove the daemon's half (that the
buffer is the one it will play); the instrument does that.
