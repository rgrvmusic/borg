# borg_microkorg2

borg on the microKORG2. Playing **any C** in the program whose OSC
is this unit makes microkorgd re-exec itself in place under `libborg.so`, the
same switch `borg_drumlogue` performs on the drumlogue. The panel then shows
`!active` in the unit's STATE parameter, and **FUNCTION 5 + FUNCTION 3** opens
the menu on the panel's console, driven by FUNCTION 1-4, with FUNCTION 5 to
close it.

## What happens

1. The unit loads as the program's OSC and, a quarter of a second later,
   paints `Borg for microKORG2` / `PLAY ANY C`. That paint is the panel's
   console, not a popup — see below. STATE reads `!ready`.
2. Play a C. The note is read from the oscillator context (`pitch[8]` and the
   `trigger` bit array) on the render callback — this platform gives an osc
   unit no note callback, so that is where a note exists.
3. The unit stages its resident `libborg.so` into `/tmp`, resets the calling
   thread's scheduler, and `execve`s `/proc/self/exe` with the object in
   `LD_PRELOAD`. Same pid, fresh daemon image. This is `borg_drumlogue`'s
   mechanism, unmodified. STATE reads `!switch` while it happens and `!active`
   afterwards.
4. The daemon reloads the program and this unit. The shim's pid marker says the
   switch is done, so the unit does not ask again and draws nothing.

## The menu

Hold **FUNCTION 5**, then press **FUNCTION 3**. The menu opens on the panel's
console, drawn over the daemon's COPY screen:

```
BORG MENU
```

| button | label in field | action |
|---|---|---|
| FUNCTION 1 | `MODS` (0) | count the `.borgmodule` files in `userfs/Programs` |
| FUNCTION 2 | `SELF` (1) | pid and phase |
| FUNCTION 3 | `LOG` (2) | the last line of the unit log |
| FUNCTION 4 | — (3) | blank; the daemon's own text is blanked so it cannot show through |
| FUNCTION 5 | `CLOSE` (4) | close the menu — forwarded to the daemon as COPY's BACK |

The row is **five** fields wide, and microkorgd leaves field 4 empty on the OSC
page — so `CLOSE` goes there, under the fifth key, and field 3 is blanked while
the menu is open rather than left showing whatever the daemon put in it.

**The menu is drawn over a view the daemon opens, and that view is the way out
of it.** The console is the only free-form text a unit gets on this panel, and
it cannot be cleared from the unit — spaces append a line. What clears it is a
view change. So the chord's second key never reaches microkorgd as FUNCTION 3:
the shim rewrites it to **FUNCTION 2**, and microkorgd opens its **COPY** screen
underneath. The menu is drawn after that page has gone out — from the shim's
panel-write hook, not from the read that carried the key — so the console line
and the labels are what the display keeps. FUNCTION 5 is COPY's BACK, so on
close the shim forwards it: the daemon cancels the view, repaints the
oscillator page, and that repaint is what takes the menu off the display. COPY
rather than INIT or MANUAL because FUNCTION 5 cancels it and opening it costs
nothing; shifted FUNCTION 1 is Initialize Target, one confirmation away from
wiping the program.

**The labels go up after the console line, and that order is the whole
mechanism.** The console takes the display; the button row is a separate layer
over it, and it survives only if it is written after the console line that
shares that write. The USB storage screen is the proof the layers coexist — it
prints its own body and still labels a soft key `EXIT`. Opening the menu the
other way round left the row empty on the board.

Each action appends its answer as a new console line (`> mods 2`), so the menu
reads as a scroll rather than a highlighted list.

**Play the chord shift first** — hold FUNCTION 5, then press the third button,
the same way INIT/COPY/MANUAL are reached. A chord pressed the other way round
cannot be intercepted: the daemon acts on the first press before a second key
exists to give it meaning. FUNCTION 5 on its own still opens the daemon's shift
row, which is where the third label reads `BORG`.

**Why the console and not a screen of borg's own.** The panel has no drawable
surface: its display is a fixed set of fields, and a frame fills one of them
with a value. That was settled on the board, not read off a listing — a 0x34
EDIT CONTROL name written for slot 0 from the display module came up in the
first parameter slot and nowhere else, and no frame in the protocol draws a box
or places text at a free position while microkorgd is running. The `CLASSIC B8`
/ `TIMBRE 1` screens that INIT and COPY open are the same thing: the daemon's
text in cells that already exist, which a unit cannot create.

Id 0 is the exception. With the daemon running the panel does not position it;
the frame falls through to a console print that takes the whole 64-pixel
display, ignores `y` (markers written at 6 and at 100 both appeared, in order)
and prints one line per frame. That is the only free-form text this instrument
gives a unit, so the menu is built out of it.

The cost is the console's: it **cannot be cleared from the unit**. Spaces
append a line rather than erasing one, and the panel leaves the console on an
encoder turn — or on a view change. That is why the menu opens over the
daemon's COPY screen: the chord's second key is rewritten to FUNCTION 2, the
daemon opens COPY underneath while the menu is drawn on top, and on `CLOSE`
FUNCTION 5 reaches the daemon as COPY's BACK, cancels the view and repaints the
oscillator page. The repaint is the cleanup, and nothing of ours is left on the
display. The row underneath is rewritten while the menu is open too, so a
daemon repaint cannot take the labels away in the meantime.

microkorgd does not know about the menu, so the shim is what makes it work. The
unit and the shim share one process but no symbols, so they are connected
directly rather than through a file: at `unit_init` the unit resolves
`borg_shim_register` with `dlsym(RTLD_DEFAULT, …)` — the shim is `LD_PRELOAD`'d,
so it is always in the global scope — and hands the shim a `borg_unit_api`
(`borg_api.h`): the menu-open flag, a chord callback, a key callback, the five
labels for the shim to rewrite into the panel's stream, and a buffer for the
shim to publish the daemon's own labels into. There is no polling and no
timestamp to miss. Two earlier versions used `/tmp` files for this and lost the
chord on the board: the shim saw it and withheld it seven times while the unit
never logged the open, because a level file written by the shim and sampled by
the unit at 12 Hz drops a button-length event between two samples. `Teardown`
unregisters, so the shim never keeps a pointer into an unloaded image.

With that link in place:

- the shim **withholds every key from the daemon's reads while the menu is
  open** — the function row and the encoders alike — and calls the unit's key
  callback on the press that carried it. A menu that leaves an encoder turning a
  parameter underneath it is not a menu;
- it **forwards FUNCTION 5 instead of eating it**, because that press is what
  closes the menu: the unit closes it, and microkorgd gets the shift key down
  and up — the COPY screen's BACK. Its release is forwarded for the same
  reason — a release the daemon sees without its press leaves the key stuck;
- it **rewrites the chord's second key to FUNCTION 2** on its way to the
  daemon, and remembers the rewrite for that key's release, so the daemon opens
  COPY and is never told FUNCTION 3 went down and FUNCTION 2 came up. Neither
  half of the chord is withheld: both are the daemon's, and what is withheld is
  everything else;
- it **opens the menu from its panel-write hook**, not from the read that
  carried the chord: the first daemon page after the press that carries a
  soft-key row is the COPY screen going up, and the menu is drawn behind it. A
  250 ms deadline draws the menu anyway if no such page is recognised, because
  the alternative is a panel where every key is eaten and nothing was drawn;
- it **filters every read path, and keeps its parse state across calls**. It
  hooks `read` and the stdio entry points (`fread`, `fgets`). Two earlier
  versions failed on the board in different ways: one only *watched* the stdio
  paths, so the keys were seen and passed on and microkorgd opened SCOPE while
  the unit opened its menu; the next removed whole frames from the buffer it
  was given, which removes nothing when the caller reads a byte at a time —
  the key log (which always kept state) showed the chord perfectly while the
  daemon still acted on the button. A withheld frame is now **erased to
  padding** (`80 <key> <pressed>` → `00 00 00`) and the decision is walked in
  stream order from the state before the buffer; the frame's *release* is
  erased too, because a release is an event to microkorgd — turning the press
  into a release opened SCOPE. The count never changes: a short return from
  `fread` means EOF to the caller. Watching a key is not withholding it;
- it **rewrites the soft-key row as the daemon draws it**: fields 0-4 become
  `MODS`/`SELF`/`LOG`/blank/`CLOSE` while the menu is open, and the third label
  of the shifted row becomes `BORG` when it is not (`INIT COPY MANUAL` →
  `INIT COPY BORG`). Both are done on the daemon's own frame on the way out, so
  the daemon can repaint that row as often as it likes and the panel keeps the
  menu. The `MANUAL` match is on the text, not the field index, and the rewrite
  is segmented around the frame so it works in a full-screen panel write, not
  only in a small one — the label row arrives inside a large write that the
  driver drains 64 bytes at a time. That is the hint that the third button is
  the chord's button;
- it **records the daemon's own labels** into the unit's buffers as it sees
  them on the downlink — but not while the open is pending or the menu is up,
  so what is kept is the oscillator page's row and not COPY's. Closing the menu
  writes the captured fields back as a fallback for a daemon that does not
  repaint; the unit holds no guess at what the row should read.

**Why FUNCTION 3 and not FUNCTION 1.** FUNCTION 5 is microkorgd's shift key, and
its shifted functions are all taken: `FUNCTION 1` is `INIT` — *Initialize
Target*, a confirmation screen away from wiping the program — and `FUNCTION 3`
is `MANUAL`, a QR code that changes nothing. The chord uses FUNCTION 3, and the
daemon never sees it: the shim rewrites that key to FUNCTION 2, so what opens
underneath is COPY, the screen FUNCTION 5 cancels. A mis-fired chord therefore
costs nothing, and INIT is left working.

The menu is the only thing this unit puts on the panel. It is taken down by
`CLOSE`, and by `CLOSE` alone: the forwarded FUNCTION 5 makes microkorgd cancel
COPY and repaint the oscillator page, and that repaint is what ends the console.
The shim's row rewrite is on the daemon's own writes, so a repaint cannot take
the menu's labels away while it is up, and the captured fields go back as a
fallback once it is down.

## The banner is the console, and that is accepted

The panel's id-0 text frame (`85 40 00 …`) is the obvious way to draw a
message. It is **not a positioned draw while microkorgd is running**: the
panel's dispatcher only draws it when an external state check says the app UI is
not up, and with the daemon running the frame falls through to a console print
that takes the whole display. The menu is built out of the same frame, for the
same reason.

That was proven with the display module, not by reading code: a marker at
`y = 6` and another at `y = 100` — the screen is 64 pixels tall — both appeared,
in order, as lines. The panel's keys do nothing to the console (it has no
cursor), only turning an encoder closes it, spaces append a line rather than
erasing one, and the console's top row is not repainted when the daemon draws
again — it stays wrong until a power cycle.

So the ready banner is painted **once**, late, and is not taken down by this
unit. That is the cost of having it.

**The switch is painted too** — `BORG SWITCHING` / `PLEASE WAIT` — because
seeing it is how you know the key you pressed was the one the unit acted on. It
cannot be taken down either: the image that drew it is replaced by `execve`
before it could clear it, and the image that comes back cannot, because spaces
append a line instead of erasing one. One encoder turn leaves console mode; the
top row stays wrong until a power cycle. That is the accepted cost.

## Files and logs

Everything diagnostic is in `/tmp` (tmpfs) — this unit must not spend the
instrument's flash on bookkeeping:

| path | what |
|---|---|
| `/tmp/logue-borg.log` | shim attach and key events |
| `/tmp/logue-borg-keys.log` | key events with read path (`r`/`f`/`g`) and buffer size, then the filter's `pass`/`drop` verdict for each; first 240 lines |
| `/tmp/logue-borg-panelw.log` | the daemon's downlink, as the shim saw it |
| `/tmp/logue-borg.active` | shim marker: pid, image, and the build string — `@exec cat /tmp/logue-borg.active` names the object that is running |
| `/tmp/logue-borg-unit.log` | the unit: state, note, switch, panel errors |

The staged `libborg.so` also goes to `/tmp`. The only userfs write is a
fallback: if `/tmp` cannot hold the shim, it is written to
`userfs/Programs/libborg.so` instead and the unit log says so.

The unit log's menu line is the one that says which side failed:
`shim: registered, menu on FUNCTION 3 + FUNCTION 5` means the link is up,
`shim: no borg_shim_register in the global scope` means the preload was not
reached, and `menu: open, MODS (N frames rendered)` carries the render count so
a menu that opens without the panel following can be told apart from one that
never opened.

## Build

```sh
bash ~/Projects/gbraad-music/borg/build-mk2.sh
```

Needs the logue SDK checkout and the `logue-sdk/dev-env` container. The shim is
compiled with the platform's cross compiler first, then included in the unit by
`shim_blob.S`. Install the `.mk2unit` from `bin/` into `Units/Oscs/Slot<N>/` on
the instrument's volume and select it as a program's OSC.

To move the activation note, change `kActivatePitchClass` in `borg.cc` and the
"PLAY ANY C" text together. A pitch class, not a MIDI note: the OCTAVE SHIFT
buttons send a different note number for the same key, so a fixed note such as
60 would only work at one shift setting. Any C presses the same key class. Note
0 was the first choice and cannot be played on the keyboard at all.

## Recovery

Nothing is changed on flash and no file is left behind that triggers the
switch, so a power cycle always returns the instrument to its normal state.
A refused switch shows `!err` in STATE and the note can be played again to
retry.

If the menu is closed while the shim has no record of the daemon's labels (it
starts out with none, until the daemon next redraws that row), the five labels
stay as `MODS`/`SELF`/`LOG`/blank/`CLOSE` until microkorgd redraws them — and
the close itself redraws them, because the forwarded FUNCTION 5 cancels COPY
and repaints the oscillator page.
